#include "sao/net/ws_client.h"
#include "sao/net/tls.h"
#include "winhttp_internal.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr size_t kMaximumMessageBytes = 16 * 1024 * 1024;

bool valid_close_code(uint16_t code) noexcept {
    return code >= 1000 && code < 5000 && code != 1004 && code != 1005 &&
           code != 1006 && code != 1015;
}

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
    size_t active_api_calls = 0;
    size_t active_callbacks = 0;
    sao_status_t close_status = SAO_STATUS_OK;
    sao_net_ws_message_callback_t callback = nullptr;
    void* callback_user_data = nullptr;
    std::shared_ptr<sao_net_ws_client_s> reap_hold;
    sao_net_ws_client_s* reap_next = nullptr;
    bool reap_queued = false;
};

namespace {

std::mutex g_registry_mutex;
std::unordered_map<sao_net_ws_client_handle_t,
                   std::shared_ptr<sao_net_ws_client_s>> g_registry;

class ApiLease final {
  public:
    static ApiLease acquire(sao_net_ws_client_handle_t raw) {
        ApiLease result;
        if (raw == nullptr) return result;
        std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
        const auto found = g_registry.find(raw);
        if (found == g_registry.end()) return result;
        std::lock_guard<std::mutex> lifecycle_lock(found->second->lifecycle_mutex);
        if (found->second->destroying) return result;
        ++found->second->active_api_calls;
        result.state_ = found->second;
        result.active_ = true;
        return result;
    }
    ~ApiLease() { release(); }
    ApiLease(ApiLease&& other) noexcept
        : state_(std::move(other.state_)), active_(other.active_) {
        other.active_ = false;
    }
    ApiLease& operator=(ApiLease&& other) noexcept {
        if (this == &other) return *this;
        release();
        state_ = std::move(other.state_);
        active_ = other.active_;
        other.active_ = false;
        return *this;
    }
    ApiLease(const ApiLease&) = delete;
    ApiLease& operator=(const ApiLease&) = delete;
    explicit operator bool() const noexcept { return active_; }
    sao_net_ws_client_s* get() const noexcept { return state_.get(); }
    std::shared_ptr<sao_net_ws_client_s> share() const noexcept { return state_; }
  private:
    void release() noexcept {
        if (!active_) return;
        {
            std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
            --state_->active_api_calls;
        }
        state_->lifecycle_cv.notify_all();
        active_ = false;
    }
    ApiLease() = default;
    std::shared_ptr<sao_net_ws_client_s> state_;
    bool active_ = false;
};

class CallbackLease final {
  public:
    explicit CallbackLease(sao_net_ws_client_s* state) : state_(state) {
        std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
        if (state_->callback != nullptr) {
            ++state_->active_callbacks;
            active_ = true;
        }
    }
    ~CallbackLease() {
        if (!active_) return;
        {
            std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
            --state_->active_callbacks;
        }
        state_->lifecycle_cv.notify_all();
    }
    explicit operator bool() const noexcept { return active_; }
  private:
    sao_net_ws_client_s* state_;
    bool active_ = false;
};

class ReceiveThreadReaper final {
  public:
    static ReceiveThreadReaper& instance() {
        static ReceiveThreadReaper* result = new ReceiveThreadReaper();
        return *result;
    }

    void enqueue(sao_net_ws_client_s* state) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state->reap_next = pending_;
            pending_ = state;
        }
        condition_.notify_one();
    }

  private:
    ReceiveThreadReaper() : worker_([this] { run(); }) {}

    void run() noexcept {
        for (;;) {
            sao_net_ws_client_s* state = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return pending_ != nullptr; });
                state = pending_;
                pending_ = state->reap_next;
                state->reap_next = nullptr;
            }
            reap(state);
        }
    }

    static void reap(sao_net_ws_client_s* state) noexcept {
        std::shared_ptr<sao_net_ws_client_s> hold;
        std::thread thread;
        {
            std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
            hold = state->reap_hold;
            state->lifecycle_cv.wait(lock, [state] {
                return !state->receive_running && !state->receive_in_flight &&
                       !state->join_in_progress;
            });
            if (state->receive_thread.joinable()) {
                state->join_in_progress = true;
                thread = std::move(state->receive_thread);
            }
        }
        if (thread.joinable()) thread.join();
        {
            std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
            state->receive_thread_id = {};
            state->join_in_progress = false;
            state->reap_queued = false;
            state->reap_hold.reset();
        }
        state->lifecycle_cv.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    sao_net_ws_client_s* pending_ = nullptr;
    std::thread worker_;
};

void queue_receive_reap(const std::shared_ptr<sao_net_ws_client_s>& held) noexcept {
    auto* state = held.get();
    bool enqueue = false;
    try {
        {
            std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
            if (!state->reap_queued && state->receive_thread.joinable()) {
                state->reap_queued = true;
                state->reap_hold = held;
                enqueue = true;
            }
        }
        if (enqueue) ReceiveThreadReaper::instance().enqueue(state);
    } catch (...) {
    }
}

void join_receive_thread(sao_net_ws_client_s* state) {
    std::thread thread;
    {
        std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
        if (state->receive_thread_id == std::this_thread::get_id()) return;
        state->lifecycle_cv.wait(lock, [&] { return !state->join_in_progress; });
        if (!state->receive_thread.joinable()) return;
        state->join_in_progress = true;
        thread = std::move(state->receive_thread);
    }
    thread.join();
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        state->join_in_progress = false;
        if (!state->receive_running) state->receive_thread_id = {};
    }
    state->lifecycle_cv.notify_all();
}

void finish_close(sao_net_ws_client_s* state, sao_status_t status) noexcept {
    state->stop_requested.store(true, std::memory_order_release);
    state->closed.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        state->close_status = status;
        state->close_complete = true;
        state->close_in_progress = false;
    }
    state->lifecycle_cv.notify_all();
}

void receive_loop(std::shared_ptr<sao_net_ws_client_s> held) noexcept {
    auto* state = held.get();
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        state->receive_thread_id = std::this_thread::get_id();
    }
    state->lifecycle_cv.notify_all();
    try {
        std::vector<uint8_t> message;
        std::vector<uint8_t> buffer(64 * 1024);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
                if (state->stop_requested.load(std::memory_order_acquire)) break;
                state->receive_in_flight = true;
            }
            DWORD bytes_read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD error = WinHttpWebSocketReceive(
                state->socket.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                &bytes_read, &type);
            {
                std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
                state->receive_in_flight = false;
                if (error == ERROR_SUCCESS &&
                    type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                    state->peer_close_received = true;
                }
            }
            state->lifecycle_cv.notify_all();
            if (error != ERROR_SUCCESS) {
                if (!state->stop_requested.load(std::memory_order_acquire)) {
                    state->receive_status.store(sao::net::internal::map_winhttp_error(error),
                                                std::memory_order_release);
                }
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                state->closing.store(true, std::memory_order_release);
                state->closed.store(true, std::memory_order_release);
                CallbackLease callback_lease(state);
                if (callback_lease) {
                    try { state->callback(SAO_NET_WS_MSG_CLOSE, nullptr, 0,
                                          state->callback_user_data); }
                    catch (...) { state->receive_status.store(SAO_STATUS_ERR_UNKNOWN); }
                }
                break;
            }
            if (bytes_read > kMaximumMessageBytes - message.size()) {
                state->receive_status.store(SAO_STATUS_ERR_BUFFER_TOO_SMALL);
                break;
            }
            message.insert(message.end(), buffer.begin(),
                           buffer.begin() + static_cast<ptrdiff_t>(bytes_read));
            const bool final = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                               type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            if (!final) continue;
            const int32_t message_type =
                type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                    ? SAO_NET_WS_MSG_TEXT : SAO_NET_WS_MSG_BINARY;
            CallbackLease callback_lease(state);
            if (callback_lease) {
                try { state->callback(message_type,
                                      message.empty() ? nullptr : message.data(),
                                      message.size(), state->callback_user_data); }
                catch (...) { state->receive_status.store(SAO_STATUS_ERR_UNKNOWN); }
            }
            message.clear();
            if (state->receive_status.load(std::memory_order_acquire) ==
                SAO_STATUS_ERR_UNKNOWN) break;
        }
    } catch (...) {
        state->receive_status.store(SAO_STATUS_ERR_UNKNOWN, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        state->receive_in_flight = false;
        state->receive_running = false;
        state->receive_thread_id = {};
    }
    state->lifecycle_cv.notify_all();
    queue_receive_reap(held);
}
sao_status_t close_websocket(const std::shared_ptr<sao_net_ws_client_s>& held,
                             uint16_t close_code) {
    auto* state = held.get();
    if (!state->socket) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_close_code(close_code)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    bool self = false;
    bool shutdown = false;
    {
        std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
        self = state->receive_thread_id == std::this_thread::get_id();
        if (state->close_complete) {
            const auto status = state->close_status;
            lock.unlock();
            if (!self) join_receive_thread(state);
            return status;
        }
        if (state->close_in_progress) {
            if (self) return SAO_STATUS_OK;
            state->lifecycle_cv.wait(lock, [&] { return !state->close_in_progress; });
            const auto status = state->close_status;
            lock.unlock();
            join_receive_thread(state);
            return status;
        }
        state->close_in_progress = true;
        state->closing.store(true, std::memory_order_release);
        state->stop_requested.store(true, std::memory_order_release);
        shutdown = state->receive_in_flight || (state->receive_running && !self);
    }
    DWORD error = ERROR_SUCCESS;
    if (shutdown) {
        { std::lock_guard<std::mutex> lock(state->send_mutex);
          error = WinHttpWebSocketShutdown(state->socket.get(), close_code, nullptr, 0); }
                if (!self) join_receive_thread(state);
        bool peer_closed = false;
        { std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
          peer_closed = state->peer_close_received; }
        if (error != ERROR_SUCCESS || !peer_closed) {
            std::lock_guard<std::mutex> lock(state->send_mutex);
            error = WinHttpWebSocketClose(state->socket.get(), close_code, nullptr, 0);
        }
    } else {
        { std::lock_guard<std::mutex> lock(state->send_mutex);
          error = WinHttpWebSocketClose(state->socket.get(), close_code, nullptr, 0); }
                if (!self) join_receive_thread(state);
    }
    finish_close(state, error == ERROR_SUCCESS ? SAO_STATUS_OK
                                               : sao::net::internal::map_winhttp_error(error));
    return state->close_status;
}
}  // namespace

namespace {

sao_status_t connect_websocket_impl(
    const char* url_utf8, const char* scope_utf8, const char* headers_utf8,
    uint32_t timeout_ms, sao_net_ws_client_handle_t* out_handle,
    bool secure_only) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (url_utf8 == nullptr || scope_utf8 == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto status = sao::net::internal::validate_request_scope(scope_utf8);
    if (status != SAO_STATUS_OK) return status;
    sao::net::internal::ParsedWinHttpUrl url{};
    status = sao::net::internal::parse_winhttp_url(url_utf8, true, url);
    if (status != SAO_STATUS_OK) return status;
    if (secure_only && !url.secure) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::wstring headers;
    status = sao::net::internal::normalize_headers(headers_utf8, headers);
    if (status != SAO_STATUS_OK) return status;
    try {
        auto state = std::make_shared<sao_net_ws_client_s>();
        state->session.reset(WinHttpOpen(L"SAO Auto Net/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!state->session) return sao::net::internal::map_winhttp_error(GetLastError());
        const auto timeout = static_cast<int>(std::min<uint32_t>(
            sao::net::internal::effective_timeout(timeout_ms), INT_MAX));
        if (!WinHttpSetTimeouts(state->session.get(), timeout, timeout, timeout, timeout))
            return sao::net::internal::map_winhttp_error(GetLastError());
        state->connection.reset(WinHttpConnect(state->session.get(), url.host.c_str(), url.port, 0));
        if (!state->connection) return sao::net::internal::map_winhttp_error(GetLastError());
        const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
        sao::net::internal::WinHttpHandle request(WinHttpOpenRequest(
            state->connection.get(), L"GET", url.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!request) return sao::net::internal::map_winhttp_error(GetLastError());
        if (!WinHttpSetTimeouts(request.get(), timeout, timeout, timeout, timeout))
            return sao::net::internal::map_winhttp_error(GetLastError());
        DWORD response_timeout = sao::net::internal::effective_timeout(timeout_ms);
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                              &response_timeout, sizeof(response_timeout)))
            return sao::net::internal::map_winhttp_error(GetLastError());
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            return sao::net::internal::map_winhttp_error(GetLastError());
        if (!headers.empty() && !WinHttpAddRequestHeaders(
                request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
            return sao::net::internal::map_winhttp_error(GetLastError());
        if (url.secure) {
            status = sao::net::internal::configure_pinned_security(
                request.get(), scope_utf8);
            if (status != SAO_STATUS_OK) return status;
        }
        status = sao::net::internal::send_request_and_receive(request, nullptr, 0, timeout_ms);
        if (status != SAO_STATUS_OK) return status;
        if (url.secure) {
            status = sao::net::internal::validate_request_certificate(
                request.get(), url.host.c_str(), scope_utf8);
            if (status != SAO_STATUS_OK) return status;
        }
        sao::net::internal::WinHttpHandle websocket(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
        if (!websocket) return sao::net::internal::map_winhttp_error(GetLastError());
        DWORD close_timeout = sao::net::internal::effective_timeout(timeout_ms);
        if (!WinHttpSetOption(websocket.get(), WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
                              &close_timeout, sizeof(close_timeout)))
            return sao::net::internal::map_winhttp_error(GetLastError());
        state->socket = std::move(websocket);
        auto* raw_handle = state.get();
        { std::lock_guard<std::mutex> lock(g_registry_mutex); g_registry.emplace(raw_handle, state); }
        *out_handle = raw_handle;
        return SAO_STATUS_OK;
    } catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}

}  // namespace

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_connect_scoped_v2(
    const char* url_utf8, const char* scope_utf8, const char* headers_utf8,
    uint32_t timeout_ms, sao_net_ws_client_handle_t* out_handle) {
    return connect_websocket_impl(url_utf8, scope_utf8, headers_utf8,
                                  timeout_ms, out_handle, true);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_connect(
    const char* url_utf8, const char* headers_utf8, uint32_t timeout_ms,
    sao_net_ws_client_handle_t* out_handle) {
    return connect_websocket_impl(url_utf8, SAO_NET_TLS_SCOPE_DEFAULT,
                                  headers_utf8, timeout_ms, out_handle, false);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_start_receive(
    sao_net_ws_client_handle_t raw_handle, sao_net_ws_message_callback_t callback,
    void* user_data) {
    auto lease = ApiLease::acquire(raw_handle);
    if (!lease || !lease.get()->socket) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (callback == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* state = lease.get();
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (state->closing.load() || state->closed.load())
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (state->receive_thread.joinable() || state->receive_running ||
            state->join_in_progress)
            return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    try {
        (void)ReceiveThreadReaper::instance();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
    if (state->closing.load() || state->closed.load()) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (state->receive_thread.joinable() || state->receive_running || state->join_in_progress)
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    state->callback = callback;
    state->callback_user_data = user_data;
    state->stop_requested.store(false);
    state->receive_running = true;
    try { state->receive_thread = std::thread(receive_loop, lease.share()); }
    catch (...) { state->receive_running = false; state->callback = nullptr; state->callback_user_data = nullptr; return SAO_STATUS_ERR_UNKNOWN; }
    return SAO_STATUS_OK;
}
extern "C" sao_status_t SAO_NET_CALL sao_net_ws_send(
    sao_net_ws_client_handle_t raw_handle, int32_t message_type,
    const uint8_t* payload, size_t payload_len) {
    auto lease = ApiLease::acquire(raw_handle);
    if (!lease || !lease.get()->socket) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* state = lease.get();
    if (state->closing.load() || state->closed.load()) return SAO_STATUS_ERR_HANDLE_INVALID;
    if ((message_type != SAO_NET_WS_MSG_TEXT && message_type != SAO_NET_WS_MSG_BINARY) ||
        payload_len > kMaximumMessageBytes ||
        payload_len > static_cast<size_t>(std::numeric_limits<DWORD>::max()) ||
        (payload_len != 0 && payload == nullptr)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto type = message_type == SAO_NET_WS_MSG_TEXT
                          ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                          : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    std::lock_guard<std::mutex> lock(state->send_mutex);
    if (state->closing.load() || state->closed.load()) return SAO_STATUS_ERR_HANDLE_INVALID;
    const DWORD error = WinHttpWebSocketSend(state->socket.get(), type,
                                             const_cast<uint8_t*>(payload),
                                             static_cast<DWORD>(payload_len));
    return error == ERROR_SUCCESS ? SAO_STATUS_OK : sao::net::internal::map_winhttp_error(error);
}

extern "C" sao_status_t SAO_NET_CALL sao_net_ws_close(
    sao_net_ws_client_handle_t raw_handle, uint16_t close_code) {
    auto lease = ApiLease::acquire(raw_handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    return close_websocket(lease.share(), close_code);
}

extern "C" void SAO_NET_CALL sao_net_ws_destroy(sao_net_ws_client_handle_t raw_handle) {
    if (raw_handle == nullptr) return;
    std::shared_ptr<sao_net_ws_client_s> state;
    { std::lock_guard<std::mutex> lock(g_registry_mutex);
      const auto found = g_registry.find(raw_handle);
      if (found == g_registry.end()) return;
      state = found->second; g_registry.erase(found);
      std::lock_guard<std::mutex> lifecycle_lock(state->lifecycle_mutex);
      state->destroying = true; state->stop_requested.store(true); }
    bool self = false;
    { std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
      self = state->receive_thread_id == std::this_thread::get_id(); }
    (void)close_websocket(state, 1000);
    if (self) return;
    std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
    state->lifecycle_cv.wait(lock, [&] {
        return state->active_api_calls == 0 && state->active_callbacks == 0 &&
               !state->receive_running && !state->receive_in_flight &&
               !state->join_in_progress && !state->close_in_progress;
    });
}
