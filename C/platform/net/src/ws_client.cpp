#include "sao/net/ws_client.h"

#include "winhttp_internal.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaximumMessageBytes = 16 * 1024 * 1024;

bool valid_close_code(uint16_t code) noexcept {
    if (code < 1000 || code >= 5000) return false;
    return code != 1004 && code != 1005 && code != 1006 && code != 1015;
}

}  // namespace

struct sao_net_ws_client_s {
    sao::net::internal::WinHttpHandle session;
    sao::net::internal::WinHttpHandle connection;
    sao::net::internal::WinHttpHandle socket;
    std::mutex send_mutex;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    std::thread receive_thread;
    std::thread::id receive_thread_id{};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> closed{false};
    std::atomic<sao_status_t> receive_status{SAO_STATUS_OK};
    bool receive_running = false;
    bool receive_in_flight = false;
    bool join_in_progress = false;
    bool close_in_progress = false;
    bool close_complete = false;
    bool peer_close_received = false;
    bool destroying = false;
    size_t active_close_calls = 0;
    sao_status_t close_status = SAO_STATUS_OK;
    sao_net_ws_message_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
};

namespace {

void join_receive_thread(sao_net_ws_client_handle_t handle) {
    std::thread receive_thread;
    {
        std::unique_lock<std::mutex> lock(handle->lifecycle_mutex);
        if (handle->receive_thread_id == std::this_thread::get_id()) return;
        handle->lifecycle_cv.wait(lock, [&] {
            return !handle->join_in_progress;
        });
        if (!handle->receive_thread.joinable()) return;
        handle->join_in_progress = true;
        receive_thread = std::move(handle->receive_thread);
    }

    receive_thread.join();

    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        handle->join_in_progress = false;
        if (!handle->receive_running) handle->receive_thread_id = {};
    }
    handle->lifecycle_cv.notify_all();
}

void finish_close(sao_net_ws_client_handle_t handle,
                  sao_status_t status) noexcept {
    handle->stop_requested.store(true, std::memory_order_release);
    handle->closed.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        handle->close_status = status;
        handle->close_complete = true;
        handle->close_in_progress = false;
    }
    handle->lifecycle_cv.notify_all();
}

void receive_loop(sao_net_ws_client_handle_t handle) noexcept {
    std::vector<uint8_t> message;
    std::vector<uint8_t> buffer(64 * 1024);
    int32_t message_type = SAO_NET_WS_MSG_BINARY;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
            if (handle->stop_requested.load(std::memory_order_acquire)) break;
            handle->receive_in_flight = true;
        }

        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type{};
        const DWORD error = WinHttpWebSocketReceive(
            handle->socket.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
            &bytes_read, &buffer_type);
        {
            std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
            handle->receive_in_flight = false;
            if (error == ERROR_SUCCESS &&
                buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                handle->peer_close_received = true;
            }
        }
        handle->lifecycle_cv.notify_all();

        if (error != ERROR_SUCCESS) {
            if (!handle->stop_requested.load(std::memory_order_acquire)) {
                handle->receive_status.store(
                    sao::net::internal::map_winhttp_error(error),
                    std::memory_order_release);
            }
            break;
        }

        if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            handle->closing.store(true, std::memory_order_release);
            handle->closed.store(true, std::memory_order_release);
            if (handle->callback != nullptr) {
                try {
                    handle->callback(SAO_NET_WS_MSG_CLOSE, nullptr, 0,
                                     handle->callback_user_data);
                } catch (...) {
                    handle->receive_status.store(SAO_STATUS_ERR_UNKNOWN,
                                                 std::memory_order_release);
                }
            }
            break;
        }

        const bool text = buffer_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                          buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        const bool final = buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                           buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        message_type = text ? SAO_NET_WS_MSG_TEXT : SAO_NET_WS_MSG_BINARY;
        if (bytes_read > kMaximumMessageBytes - message.size()) {
            handle->receive_status.store(SAO_STATUS_ERR_BUFFER_TOO_SMALL,
                                         std::memory_order_release);
            break;
        }
        try {
            message.insert(message.end(), buffer.begin(),
                           buffer.begin() + static_cast<ptrdiff_t>(bytes_read));
        } catch (...) {
            handle->receive_status.store(SAO_STATUS_ERR_UNKNOWN,
                                         std::memory_order_release);
            break;
        }
        if (final) {
            if (handle->callback != nullptr) {
                try {
                    handle->callback(message_type,
                                     message.empty() ? nullptr : message.data(),
                                     message.size(), handle->callback_user_data);
                } catch (...) {
                    handle->receive_status.store(SAO_STATUS_ERR_UNKNOWN,
                                                 std::memory_order_release);
                    break;
                }
            }
            message.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        handle->receive_in_flight = false;
        handle->receive_running = false;
    }
    handle->lifecycle_cv.notify_all();
}

sao_status_t close_websocket(sao_net_ws_client_handle_t handle,
                             uint16_t close_code) {
    if (!handle->socket) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_close_code(close_code)) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    bool caller_is_receive_thread = false;
    bool use_shutdown = false;
    {
        std::unique_lock<std::mutex> lock(handle->lifecycle_mutex);
        caller_is_receive_thread =
            handle->receive_thread_id == std::this_thread::get_id();
        if (handle->close_complete) {
            const auto status = handle->close_status;
            lock.unlock();
            if (!caller_is_receive_thread) join_receive_thread(handle);
            return status;
        }
        if (handle->close_in_progress) {
            if (caller_is_receive_thread) return SAO_STATUS_OK;
            handle->lifecycle_cv.wait(lock, [&] {
                return !handle->close_in_progress;
            });
            const auto status = handle->close_status;
            lock.unlock();
            join_receive_thread(handle);
            return status;
        }

        handle->close_in_progress = true;
        handle->closing.store(true, std::memory_order_release);
        use_shutdown = handle->receive_in_flight ||
                       (handle->receive_running &&
                        !caller_is_receive_thread);
        if (!use_shutdown) {
            handle->stop_requested.store(true, std::memory_order_release);
        }
    }

    DWORD error = ERROR_SUCCESS;
    if (use_shutdown) {
        {
            std::lock_guard<std::mutex> guard(handle->send_mutex);
            error = WinHttpWebSocketShutdown(handle->socket.get(), close_code,
                                             nullptr, 0);
        }
        if (error != ERROR_SUCCESS) {
            handle->stop_requested.store(true, std::memory_order_release);
        }

        join_receive_thread(handle);

        bool peer_close_received = false;
        {
            std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
            peer_close_received = handle->peer_close_received;
        }
        if (error != ERROR_SUCCESS || !peer_close_received) {
            std::lock_guard<std::mutex> guard(handle->send_mutex);
            error = WinHttpWebSocketClose(handle->socket.get(), close_code,
                                          nullptr, 0);
        }
    } else {
        {
            std::lock_guard<std::mutex> guard(handle->send_mutex);
            error = WinHttpWebSocketClose(handle->socket.get(), close_code,
                                          nullptr, 0);
        }
        if (!caller_is_receive_thread) join_receive_thread(handle);
    }

    const auto status = error == ERROR_SUCCESS
                            ? SAO_STATUS_OK
                            : sao::net::internal::map_winhttp_error(error);
    finish_close(handle, status);
    return status;
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_connect(
    const char* url_utf8, const char* headers_utf8, uint32_t timeout_ms,
    sao_net_ws_client_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (url_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    sao::net::internal::ParsedWinHttpUrl url{};
    auto status = sao::net::internal::parse_winhttp_url(url_utf8, true, url);
    if (status != SAO_STATUS_OK) return status;
    std::wstring headers;
    status = sao::net::internal::normalize_headers(headers_utf8, headers);
    if (status != SAO_STATUS_OK) return status;

    try {
        auto handle = std::make_unique<sao_net_ws_client_s>();
        handle->session.reset(WinHttpOpen(
            L"SAO Auto Net/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!handle->session) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        const auto timeout = static_cast<int>(std::min<uint32_t>(
            sao::net::internal::effective_timeout(timeout_ms), INT_MAX));
        if (!WinHttpSetTimeouts(handle->session.get(), timeout, timeout, timeout, timeout)) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        handle->connection.reset(WinHttpConnect(
            handle->session.get(), url.host.c_str(), url.port, 0));
        if (!handle->connection) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }

        const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
        sao::net::internal::WinHttpHandle request(WinHttpOpenRequest(
            handle->connection.get(), L"GET", url.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!request) return sao::net::internal::map_winhttp_error(GetLastError());
        if (!WinHttpSetTimeouts(request.get(), timeout, timeout, timeout, timeout)) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        DWORD response_timeout = sao::net::internal::effective_timeout(timeout_ms);
        if (!WinHttpSetOption(request.get(),
                              WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                              &response_timeout, sizeof(response_timeout))) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                              nullptr, 0)) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        if (!headers.empty() && !WinHttpAddRequestHeaders(
                request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        status = sao::net::internal::send_request_and_receive(
            request, nullptr, 0, timeout_ms);
        if (status != SAO_STATUS_OK) return status;
        if (url.secure) {
            status = sao::net::internal::validate_request_certificate(
                request.get(), "default");
            if (status != SAO_STATUS_OK) return status;
        }
        sao::net::internal::WinHttpHandle websocket(
            WinHttpWebSocketCompleteUpgrade(request.get(), 0));
        if (!websocket) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        DWORD close_timeout = sao::net::internal::effective_timeout(timeout_ms);
        if (!WinHttpSetOption(websocket.get(),
                              WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
                              &close_timeout, sizeof(close_timeout))) {
            return sao::net::internal::map_winhttp_error(GetLastError());
        }
        handle->socket = std::move(websocket);
        *out_handle = handle.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_start_receive(
    sao_net_ws_client_handle_t handle, sao_net_ws_message_callback_t callback,
    void* user_data) {
    if (handle == nullptr || !handle->socket) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(handle->lifecycle_mutex);
    if (handle->closing.load(std::memory_order_acquire) ||
        handle->closed.load(std::memory_order_acquire)) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (handle->receive_thread.joinable() || handle->receive_running ||
        handle->join_in_progress) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    handle->callback = callback;
    handle->callback_user_data = user_data;
    handle->stop_requested.store(false, std::memory_order_release);
    handle->receive_running = true;
    try {
        handle->receive_thread = std::thread(receive_loop, handle);
        handle->receive_thread_id = handle->receive_thread.get_id();
        return SAO_STATUS_OK;
    } catch (...) {
        handle->receive_running = false;
        handle->receive_thread_id = {};
        handle->callback = nullptr;
        handle->callback_user_data = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_send(
    sao_net_ws_client_handle_t handle, int32_t message_type,
    const uint8_t* payload, size_t payload_len) {
    if (handle == nullptr || !handle->socket) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->closing.load(std::memory_order_acquire) ||
        handle->closed.load(std::memory_order_acquire)) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if ((message_type != SAO_NET_WS_MSG_TEXT &&
         message_type != SAO_NET_WS_MSG_BINARY) ||
        payload_len > kMaximumMessageBytes ||
        payload_len > static_cast<size_t>(std::numeric_limits<DWORD>::max()) ||
        (payload_len != 0 && payload == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto type = message_type == SAO_NET_WS_MSG_TEXT
                          ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                          : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    std::lock_guard<std::mutex> guard(handle->send_mutex);
    if (handle->closing.load(std::memory_order_acquire) ||
        handle->closed.load(std::memory_order_acquire)) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    const DWORD error = WinHttpWebSocketSend(
        handle->socket.get(), type, const_cast<uint8_t*>(payload),
        static_cast<DWORD>(payload_len));
    return error == ERROR_SUCCESS ? SAO_STATUS_OK
                                  : sao::net::internal::map_winhttp_error(error);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_close(
    sao_net_ws_client_handle_t handle, uint16_t close_code) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        if (handle->destroying) return SAO_STATUS_ERR_HANDLE_INVALID;
        ++handle->active_close_calls;
    }

    const auto status = close_websocket(handle, close_code);
    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        --handle->active_close_calls;
    }
    handle->lifecycle_cv.notify_all();
    return status;
}

extern "C" void SAO_NET_CALL sao_net_ws_destroy(sao_net_ws_client_handle_t handle) {
    if (handle == nullptr) return;
    bool caller_is_receive_thread = false;
    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        caller_is_receive_thread =
            handle->receive_thread_id == std::this_thread::get_id();
    }
    if (caller_is_receive_thread) {
        handle->closing.store(true, std::memory_order_release);
        handle->stop_requested.store(true, std::memory_order_release);
        return;
    }
    if (handle->socket) (void)sao_net_ws_close(handle, 1000);
    {
        std::lock_guard<std::mutex> lock(handle->lifecycle_mutex);
        handle->destroying = true;
    }
    join_receive_thread(handle);
    {
        std::unique_lock<std::mutex> lock(handle->lifecycle_mutex);
        handle->lifecycle_cv.wait(lock, [&] {
            return handle->active_close_calls == 0 &&
                   !handle->join_in_progress;
        });
    }
    delete handle;
}
