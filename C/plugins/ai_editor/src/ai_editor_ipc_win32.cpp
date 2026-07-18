#include "sao/ai_editor/ai_editor_ipc.h"

#include "ai_editor_ipc_internal.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr const char* kDefaultPipePrefix = "\\\\.\\pipe\\sao_ai_editor_ipc_";
constexpr uint32_t kMaximumPayload = 1024U * 1024U;
constexpr uint32_t kIoTimeoutMs = 5000U;

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~ScopedHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

std::string random_suffix() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buffer[24]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(rng()));
    return buffer;
}

bool copy_string(char* destination, size_t capacity, const std::string& source) {
    if (destination == nullptr || capacity == 0 || source.size() + 1 > capacity) {
        return false;
    }
    std::memcpy(destination, source.data(), source.size());
    destination[source.size()] = '\0';
    return true;
}

bool utf8_to_wide(const std::string& input, std::wstring& output) {
    if (input.empty()) {
        return false;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            input.data(),
                                            static_cast<int>(input.size()),
                                            nullptr, 0);
    if (length <= 0) {
        return false;
    }
    output.assign(static_cast<size_t>(length), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               input.data(), static_cast<int>(input.size()),
                               output.data(), length) == length;
}

uint32_t decode_length(const std::array<uint8_t, 4>& header) {
    return static_cast<uint32_t>(header[0]) |
           (static_cast<uint32_t>(header[1]) << 8U) |
           (static_cast<uint32_t>(header[2]) << 16U) |
           (static_cast<uint32_t>(header[3]) << 24U);
}

std::array<uint8_t, 4> encode_length(uint32_t length) {
    return {
        static_cast<uint8_t>(length),
        static_cast<uint8_t>(length >> 8U),
        static_cast<uint8_t>(length >> 16U),
        static_cast<uint8_t>(length >> 24U),
    };
}

int32_t wait_overlapped(HANDLE pipe,
                        OVERLAPPED& overlapped,
                        uint32_t timeout_ms,
                        HANDLE process,
                        DWORD& transferred) {
    std::array<HANDLE, 2> handles{overlapped.hEvent, process};
    const DWORD count = process != nullptr ? 2U : 1U;
    const DWORD wait_result = WaitForMultipleObjects(
        count, handles.data(), FALSE, timeout_ms);
    if (wait_result == WAIT_OBJECT_0) {
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        return SAO_AI_EDITOR_OK;
    }

    CancelIoEx(pipe, &overlapped);
    (void)GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    if (process != nullptr && wait_result == WAIT_OBJECT_0 + 1U) {
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    return wait_result == WAIT_TIMEOUT
        ? SAO_AI_EDITOR_ERR_IPC_TIMEOUT
        : SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
}

int32_t read_exact(HANDLE pipe,
                   void* destination,
                   uint32_t size,
                   uint32_t timeout_ms,
                   HANDLE process) {
    auto* cursor = static_cast<uint8_t*>(destination);
    uint32_t remaining = size;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (remaining > 0) {
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const BOOL started = ReadFile(pipe, cursor, remaining, &transferred,
                                      &overlapped);
        if (!started) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                return SAO_AI_EDITOR_ERR_IPC_CLOSED;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                CancelIoEx(pipe, &overlapped);
                (void)GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                return SAO_AI_EDITOR_ERR_IPC_TIMEOUT;
            }
            const auto remaining_ms = static_cast<uint32_t>(deadline - now);
            const int32_t status = wait_overlapped(
                pipe, overlapped, remaining_ms, process, transferred);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        if (transferred == 0) {
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        cursor += transferred;
        remaining -= transferred;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t write_exact(HANDLE pipe, const void* source, uint32_t size) {
    const auto* cursor = static_cast<const uint8_t*>(source);
    uint32_t remaining = size;
    while (remaining > 0) {
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0;
        const BOOL started = WriteFile(pipe, cursor, remaining, &transferred,
                                       &overlapped);
        if (!started) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                return SAO_AI_EDITOR_ERR_IPC_CLOSED;
            }
            const int32_t status = wait_overlapped(
                pipe, overlapped, kIoTimeoutMs, nullptr, transferred);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        if (transferred == 0) {
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        cursor += transferred;
        remaining -= transferred;
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

struct SaoAiEditorIpc {
    std::mutex mutex;
    SaoAiEditorIpcTransport transport = SAO_AI_EDITOR_IPC_NAMED_PIPE;
    std::string pipe_name;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool connected = false;
    std::vector<uint8_t> pending_message;
};

namespace {

int32_t receive_frame_locked(SaoAiEditorIpc& ipc,
                             uint32_t timeout_ms,
                             HANDLE process) {
    std::array<uint8_t, 4> header{};
    int32_t status = read_exact(ipc.pipe, header.data(),
                                static_cast<uint32_t>(header.size()),
                                timeout_ms, process);
    if (status != SAO_AI_EDITOR_OK) {
        ipc.connected = false;
        return status;
    }
    const uint32_t length = decode_length(header);
    if (length > kMaximumPayload) {
        ipc.connected = false;
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    ipc.pending_message.assign(length, 0);
    if (length == 0) {
        return SAO_AI_EDITOR_OK;
    }
    status = read_exact(ipc.pipe, ipc.pending_message.data(), length,
                        timeout_ms, process);
    if (status != SAO_AI_EDITOR_OK) {
        ipc.pending_message.clear();
        ipc.connected = false;
    }
    return status;
}

int32_t copy_pending_locked(SaoAiEditorIpc& ipc,
                            void* buffer,
                            uint32_t buffer_cap,
                            uint32_t* out_len) {
    const auto required = static_cast<uint32_t>(ipc.pending_message.size());
    *out_len = required;
    if (required > buffer_cap || (required > 0 && buffer == nullptr)) {
        return SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    }
    if (required > 0) {
        std::memcpy(buffer, ipc.pending_message.data(), required);
    }
    ipc.pending_message.clear();
    return SAO_AI_EDITOR_OK;
}

}  // namespace

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_create(
    SaoAiEditorIpcTransport transport,
    const char* pipe_name_utf8,
    sao_ai_editor_ipc_t* out_handle) {
    try {
    if (out_handle == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (transport != SAO_AI_EDITOR_IPC_NAMED_PIPE) {
        return SAO_AI_EDITOR_ERR_NOT_IMPLEMENTED;
    }

    auto implementation = std::make_unique<SaoAiEditorIpc>();
    implementation->transport = transport;
    implementation->pipe_name = pipe_name_utf8 != nullptr && pipe_name_utf8[0] != '\0'
        ? pipe_name_utf8
        : std::string(kDefaultPipePrefix) + random_suffix();

    std::wstring wide_name;
    if (implementation->pipe_name.rfind("\\\\.\\pipe\\", 0) != 0 ||
        !utf8_to_wide(implementation->pipe_name, wide_name)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    implementation->pipe = CreateNamedPipeW(
        wide_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, kMaximumPayload + 4U, kMaximumPayload + 4U, 0, nullptr);
    if (implementation->pipe == INVALID_HANDLE_VALUE) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    *out_handle = implementation.release();
    return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_ipc_get_pipe_name(sao_ai_editor_ipc_t handle,
                                char* name_out,
                                size_t name_cap) {
    try {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (name_out == nullptr || name_cap == 0) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    return copy_string(name_out, name_cap, handle->pipe_name)
        ? SAO_AI_EDITOR_OK
        : SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t sao::ai_editor::detail::ipc_accept_with_process(
    sao_ai_editor_ipc_t handle, uint32_t timeout_ms, HANDLE process) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->connected) {
        return SAO_AI_EDITOR_OK;
    }
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL connected = ConnectNamedPipe(handle->pipe, &overlapped);
    if (connected) {
        handle->connected = true;
        return SAO_AI_EDITOR_OK;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        handle->connected = true;
        return SAO_AI_EDITOR_OK;
    }
    if (error != ERROR_IO_PENDING) {
        return SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL;
    }
    const int32_t status = wait_overlapped(
        handle->pipe, overlapped, timeout_ms, process, transferred);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    handle->connected = true;
    return SAO_AI_EDITOR_OK;
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_accept(
    sao_ai_editor_ipc_t handle, uint32_t timeout_ms) {
    try {
        return sao::ai_editor::detail::ipc_accept_with_process(
            handle, timeout_ms, nullptr);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_ipc_is_connected(sao_ai_editor_ipc_t handle,
                               bool* out_connected) {
    try {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (out_connected == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_connected = handle->connected;
    return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_send(
    sao_ai_editor_ipc_t handle, const void* payload, uint32_t payload_len) {
    try {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if ((payload == nullptr && payload_len != 0) || payload_len > kMaximumPayload) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->connected) {
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    const auto header = encode_length(payload_len);
    int32_t status = write_exact(handle->pipe, header.data(),
                                 static_cast<uint32_t>(header.size()));
    if (status == SAO_AI_EDITOR_OK && payload_len > 0) {
        status = write_exact(handle->pipe, payload, payload_len);
    }
    if (status != SAO_AI_EDITOR_OK) {
        handle->connected = false;
    }
    return status;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

int32_t sao::ai_editor::detail::ipc_recv_with_process(
    sao_ai_editor_ipc_t handle,
    void* buffer,
    uint32_t buffer_cap,
    uint32_t* out_len,
    uint32_t timeout_ms,
    HANDLE process) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (out_len == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_len = 0;
    if (!handle->connected) {
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    if (handle->pending_message.empty()) {
        const int32_t status = receive_frame_locked(*handle, timeout_ms, process);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
    }
    return copy_pending_locked(*handle, buffer, buffer_cap, out_len);
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_recv(
    sao_ai_editor_ipc_t handle,
    void* buffer,
    uint32_t buffer_cap,
    uint32_t* out_len) {
    try {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (out_len == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    *out_len = 0;
    if (!handle->connected) {
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    if (handle->pending_message.empty()) {
        std::array<uint8_t, 4> header{};
        DWORD available = 0;
        DWORD peeked = 0;
        if (!PeekNamedPipe(handle->pipe, header.data(),
                           static_cast<DWORD>(header.size()), &peeked,
                           &available, nullptr)) {
            handle->connected = false;
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        if (peeked < header.size()) {
            return SAO_AI_EDITOR_OK;
        }
        const uint32_t length = decode_length(header);
        if (length > kMaximumPayload) {
            handle->connected = false;
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        if (available < header.size() + length) {
            return SAO_AI_EDITOR_OK;
        }
        const int32_t status = receive_frame_locked(*handle, kIoTimeoutMs, nullptr);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
    }
    return copy_pending_locked(*handle, buffer, buffer_cap, out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_ipc_destroy(
    sao_ai_editor_ipc_t handle) {
    try {
    if (handle == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (handle->pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle->pipe, nullptr);
            if (handle->connected) {
                (void)FlushFileBuffers(handle->pipe);
                (void)DisconnectNamedPipe(handle->pipe);
            }
            CloseHandle(handle->pipe);
            handle->pipe = INVALID_HANDLE_VALUE;
        }
        handle->connected = false;
        handle->pending_message.clear();
    }
    delete handle;
    } catch (...) {
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_ipc_test_connect_mock(sao_ai_editor_ipc_t handle) {
    try {
        return handle == nullptr
            ? SAO_AI_EDITOR_ERR_HANDLE_INVALID
            : SAO_AI_EDITOR_ERR_NOT_IMPLEMENTED;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}
