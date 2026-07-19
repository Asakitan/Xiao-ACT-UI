#include "sao/ai_editor/ai_editor_native.h"

#include "mcp_server.h"
#include "gpu_hunt_panel.h"
#if SAO_AI_EDITOR_HAS_WEBVIEW
#include "webview_bridge.h"
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kMaximumPayload = 1024U * 1024U;
constexpr DWORD kConnectTimeoutMs = 5000U;
constexpr UINT_PTR kEventTimerId = 1;
constexpr UINT_PTR kAutoExitTimerId = 2;
constexpr UINT kPipeStoppedMessage = WM_APP + 1U;
constexpr UINT kParentShutdownMessage = WM_APP + 2U;
constexpr int kOutputEditId = 1001;
constexpr int kRequestEditId = 1002;
constexpr int kSendButtonId = 1003;
constexpr int kClearButtonId = 1004;
constexpr int kGpuHuntButtonId = 1005;
constexpr wchar_t kWindowClassName[] = L"SaoAiEditorNativeWindow";
constexpr wchar_t kWindowTitle[] = L"SAO AI Editor";
constexpr std::string_view kHandshakeRequest = "SAO_AI_EDITOR_HELLO 1";
constexpr std::string_view kHandshakeResponse = "SAO_AI_EDITOR_READY 1";
constexpr std::string_view kShutdownRequest = "shutdown";
constexpr std::string_view kShutdownResponse = "shutdown-ok";

class ScopedHandle final {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE value) noexcept : value_(value) {}

    ~ScopedHandle() {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    HANDLE get() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

struct RuntimeDeleter final {
    void operator()(SaoAiEditorRuntime* runtime) const noexcept {
        sao_ai_editor_runtime_destroy(runtime);
    }
};

using RuntimeHandle = std::unique_ptr<SaoAiEditorRuntime, RuntimeDeleter>;

struct Arguments final {
    std::wstring pipe_name;
    std::wstring webview_url;
    std::wstring cli_method;
    std::wstring cli_params_inline;
    std::optional<std::filesystem::path> cli_params_file;
    std::optional<std::filesystem::path> workspace;
    std::optional<std::filesystem::path> node_executable;
    DWORD auto_exit_ms = 0;
    bool headless = false;
    bool hidden_window = false;
    bool ui_smoke_test = false;
    bool mcp_server = false;
    bool extension_host_mode = false;
    bool webview_mode = false;
    bool cli_mode = false;
    bool gpu_hunt_only = false;
    bool show_help = false;
};

void print_usage() {
    constexpr wchar_t usage[] =
        L"Usage: SaoAiEditor.exe --sao-ai-editor-pipe <pipe> "
        L"[--workspace <directory>] [--headless]\n"
        L"       SaoAiEditor.exe --ui-smoke-test [--workspace <directory>] "
        L"[--hidden] [--auto-exit-ms <milliseconds>]\n"
        L"       SaoAiEditor.exe --mcp-server [--workspace <directory>]\n"
        L"       SaoAiEditor.exe --extension-host [--workspace <directory>] "
        L"[--node-executable <path>]\n"
        L"       SaoAiEditor.exe --cli --cli-method <method> "
        L"[--cli-params <json>] [--cli-params-file <path>] "
        L"[--workspace <directory>]\n"
        L"       SaoAiEditor.exe --gpu-hunt\n";
    std::fputws(usage, stderr);
    OutputDebugStringW(usage);
}

bool parse_milliseconds(std::wstring_view value, DWORD& result) {
    if (value.empty()) {
        return false;
    }
    uint64_t parsed = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        parsed = parsed * 10U + static_cast<uint64_t>(character - L'0');
        if (parsed > 60'000U) {
            return false;
        }
    }
    if (parsed == 0) {
        return false;
    }
    result = static_cast<DWORD>(parsed);
    return true;
}

bool parse_arguments(int argc, wchar_t** argv, Arguments& result) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            result.show_help = true;
            continue;
        }
        if (argument == L"--headless") {
            if (result.headless) {
                return false;
            }
            result.headless = true;
            continue;
        }
        if (argument == L"--hidden") {
            if (result.hidden_window) {
                return false;
            }
            result.hidden_window = true;
            continue;
        }
        if (argument == L"--ui-smoke-test") {
            if (result.ui_smoke_test) {
                return false;
            }
            result.ui_smoke_test = true;
            continue;
        }
        if (argument == L"--mcp-server") {
            if (result.mcp_server) {
                return false;
            }
            result.mcp_server = true;
            continue;
        }
        if (argument == L"--extension-host") {
            if (result.extension_host_mode) {
                return false;
            }
            result.extension_host_mode = true;
            continue;
        }
        if (argument == L"--webview") {
            if (result.webview_mode) {
                return false;
            }
            result.webview_mode = true;
            continue;
        }
        if (argument == L"--cli") {
            if (result.cli_mode) {
                return false;
            }
            result.cli_mode = true;
            continue;
        }
        if (argument == L"--gpu-hunt") {
            if (result.gpu_hunt_only) {
                return false;
            }
            result.gpu_hunt_only = true;
            continue;
        }
        if (argument != L"--sao-ai-editor-pipe" &&
            argument != L"--workspace" && argument != L"--auto-exit-ms" &&
            argument != L"--webview-url" && argument != L"--cli-method" &&
            argument != L"--cli-params" && argument != L"--cli-params-file" &&
            argument != L"--node-executable") {
            return false;
        }
        if (++index >= argc || argv[index][0] == L'\0') {
            return false;
        }
        if (argument == L"--sao-ai-editor-pipe") {
            if (!result.pipe_name.empty()) {
                return false;
            }
            result.pipe_name = argv[index];
        } else if (argument == L"--workspace") {
            if (result.workspace.has_value()) {
                return false;
            }
            result.workspace = std::filesystem::path(argv[index]);
        } else if (argument == L"--node-executable") {
            if (result.node_executable.has_value()) {
                return false;
            }
            result.node_executable = std::filesystem::path(argv[index]);
        } else if (argument == L"--webview-url") {
            if (!result.webview_url.empty()) {
                return false;
            }
            result.webview_url = argv[index];
        } else if (argument == L"--cli-method") {
            if (!result.cli_method.empty()) {
                return false;
            }
            result.cli_method = argv[index];
        } else if (argument == L"--cli-params") {
            if (!result.cli_params_inline.empty() ||
                result.cli_params_file.has_value()) {
                return false;
            }
            result.cli_params_inline = argv[index];
        } else if (argument == L"--cli-params-file") {
            if (!result.cli_params_inline.empty() ||
                result.cli_params_file.has_value()) {
                return false;
            }
            result.cli_params_file = std::filesystem::path(argv[index]);
        } else if (result.auto_exit_ms != 0 ||
                   !parse_milliseconds(argv[index], result.auto_exit_ms)) {
            return false;
        }
    }
    if (result.show_help) {
        return true;
    }
    if (result.gpu_hunt_only) {
        // --gpu-hunt is a self-contained pop-up mode driven by the SAO
        // menu's Tools entry; no pipe / workspace / auto-exit are meaningful.
        return result.pipe_name.empty() && !result.mcp_server &&
               !result.extension_host_mode && !result.webview_mode &&
               !result.cli_mode && !result.ui_smoke_test &&
               !result.headless && !result.hidden_window &&
               !result.workspace.has_value() &&
               !result.node_executable.has_value() &&
               result.auto_exit_ms == 0;
    }
    if (result.mcp_server) {
        return result.pipe_name.empty() && !result.ui_smoke_test &&
               !result.headless && !result.hidden_window &&
               !result.extension_host_mode &&
               !result.node_executable.has_value() &&
               result.auto_exit_ms == 0;
    }
    if (result.extension_host_mode) {
        return result.pipe_name.empty() && !result.ui_smoke_test &&
               !result.headless && !result.hidden_window &&
               !result.mcp_server && !result.webview_mode &&
               !result.cli_mode && result.auto_exit_ms == 0;
    }
    if (result.webview_mode) {
        return result.pipe_name.empty() && !result.ui_smoke_test &&
               !result.headless && !result.mcp_server && !result.cli_mode &&
               !result.extension_host_mode &&
               !result.node_executable.has_value();
    }
    if (result.cli_mode) {
        if (result.cli_method.empty()) {
            return false;
        }
        return result.pipe_name.empty() && !result.ui_smoke_test &&
               !result.headless && !result.mcp_server &&
               !result.webview_mode && !result.extension_host_mode &&
               !result.node_executable.has_value();
    }
    if (result.node_executable.has_value()) {
        // --node-executable is only meaningful for --extension-host.
        return false;
    }
    if (result.ui_smoke_test) {
        return result.pipe_name.empty() && !result.headless;
    }
    if (result.pipe_name.empty()) {
        return false;
    }
    return !result.headless ||
           (!result.hidden_window && result.auto_exit_ms == 0);
}

bool is_local_pipe_name(std::wstring_view name) {
    constexpr std::wstring_view prefix = LR"(\\.\pipe\)";
    return name.size() > prefix.size() && name.starts_with(prefix) && name.size() < 512;
}

std::optional<std::string> wide_to_utf8(std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required, nullptr,
                            nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return std::wstring();
    }
    if (value.size() > static_cast<size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required) != required) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::filesystem::path>
resolve_workspace(const std::optional<std::filesystem::path>& requested) {
    std::error_code error;
    std::filesystem::path workspace =
        requested.has_value() ? *requested : std::filesystem::current_path(error);
    if (error || workspace.empty()) {
        return std::nullopt;
    }
    if (!workspace.is_absolute()) {
        workspace = std::filesystem::absolute(workspace, error);
        if (error) {
            return std::nullopt;
        }
    }
    if (!std::filesystem::is_directory(workspace, error) || error) {
        return std::nullopt;
    }
    workspace = std::filesystem::weakly_canonical(workspace, error);
    if (error || !workspace.is_absolute()) {
        return std::nullopt;
    }
    return workspace;
}

template <typename Operation>
bool transfer_exact(HANDLE pipe, HANDLE stop_event, uint32_t size,
                    Operation&& operation) {
    ScopedHandle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!io_event) {
        return false;
    }
    uint32_t offset = 0;
    while (offset < size) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
            return false;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = io_event.get();
        ResetEvent(io_event.get());
        DWORD transferred = 0;
        const uint32_t remaining = size - offset;
        if (!operation(offset, remaining, &transferred, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                return false;
            }
            const std::array<HANDLE, 2> waits{io_event.get(), stop_event};
            const DWORD wait = WaitForMultipleObjects(
                static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                (void)CancelIoEx(pipe, &overlapped);
                (void)WaitForSingleObject(io_event.get(), INFINITE);
                (void)GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                return false;
            }
        }
        if (transferred == 0 || transferred > remaining) {
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool read_exact(HANDLE pipe, HANDLE stop_event, void* destination,
                uint32_t size) {
    auto* cursor = static_cast<uint8_t*>(destination);
    return transfer_exact(
        pipe, stop_event, size,
        [&](uint32_t offset, uint32_t remaining, DWORD* transferred,
            OVERLAPPED* overlapped) {
            return ReadFile(pipe, cursor + offset, remaining, transferred,
                            overlapped) != FALSE;
        });
}

bool write_exact(HANDLE pipe, HANDLE stop_event, const void* source,
                 uint32_t size) {
    const auto* cursor = static_cast<const uint8_t*>(source);
    return transfer_exact(
        pipe, stop_event, size,
        [&](uint32_t offset, uint32_t remaining, DWORD* transferred,
            OVERLAPPED* overlapped) {
            return WriteFile(pipe, cursor + offset, remaining, transferred,
                             overlapped) != FALSE;
        });
}

uint32_t decode_length(const std::array<uint8_t, 4>& header) {
    return static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8U) |
           (static_cast<uint32_t>(header[2]) << 16U) | (static_cast<uint32_t>(header[3]) << 24U);
}

std::array<uint8_t, 4> encode_length(uint32_t length) {
    return {
        static_cast<uint8_t>(length),
        static_cast<uint8_t>(length >> 8U),
        static_cast<uint8_t>(length >> 16U),
        static_cast<uint8_t>(length >> 24U),
    };
}

bool receive_frame(HANDLE pipe, HANDLE stop_event, std::string& payload) {
    std::array<uint8_t, 4> header{};
    if (!read_exact(pipe, stop_event, header.data(),
                    static_cast<uint32_t>(header.size()))) {
        return false;
    }
    const uint32_t size = decode_length(header);
    if (size > kMaximumPayload) {
        return false;
    }
    payload.assign(size, '\0');
    return size == 0 || read_exact(pipe, stop_event, payload.data(), size);
}

bool send_frame(HANDLE pipe, HANDLE stop_event, std::string_view payload) {
    if (payload.size() > kMaximumPayload) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(payload.size());
    const auto header = encode_length(size);
    return write_exact(pipe, stop_event, header.data(),
                       static_cast<uint32_t>(header.size())) &&
           (payload.empty() ||
            write_exact(pipe, stop_event, payload.data(), size));
}

ScopedHandle connect_pipe(const std::wstring& pipe_name) {
    if (!WaitNamedPipeW(pipe_name.c_str(), kConnectTimeoutMs)) {
        return ScopedHandle();
    }
    return ScopedHandle(CreateFileW(
        pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
}

std::string dispatch_error_json(int32_t status) {
    return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{"
           "\"code\":-32000,\"message\":\"runtime dispatch failed\","
           "\"data\":{\"status\":" +
           std::to_string(status) + "}},\"sao\":{\"protocolVersion\":1}}";
}

std::string dispatch_runtime(sao_ai_editor_runtime_t runtime,
                             const std::string& request) {
    uint32_t required = 0;
    int32_t status = sao_ai_editor_runtime_dispatch(runtime, request.data(),
                                                    static_cast<uint32_t>(request.size()), nullptr,
                                                    0, &required);
    if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return dispatch_error_json(status);
    }

    if (required > kMaximumPayload) {
        return dispatch_error_json(SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL);
    }
    std::vector<char> response(static_cast<size_t>(required) + 1U);
    status = sao_ai_editor_runtime_dispatch(runtime, nullptr, 0, response.data(),
                                            static_cast<uint32_t>(response.size()), &required);
    if (status != SAO_AI_EDITOR_OK) {
        return dispatch_error_json(status);
    }
    return std::string(response.data(), required);
}

class RuntimeSession final {
public:
    RuntimeSession(RuntimeHandle runtime, ScopedHandle stop_event)
        : runtime_(std::move(runtime)), stop_event_(std::move(stop_event)) {}

    ~RuntimeSession() {
        request_stop();
        close();
    }

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;

    HANDLE stop_event() const noexcept {
        return stop_event_.get();
    }

    bool stopped() const noexcept {
        return WaitForSingleObject(stop_event_.get(), 0) == WAIT_OBJECT_0;
    }

    void attach_pipe(HANDLE pipe) noexcept {
        pipe_.store(pipe, std::memory_order_release);
    }

    void detach_pipe(HANDLE pipe) noexcept {
        HANDLE expected = pipe;
        (void)pipe_.compare_exchange_strong(expected, nullptr,
                                            std::memory_order_acq_rel);
    }

    void request_stop() noexcept {
        (void)SetEvent(stop_event_.get());
        const HANDLE pipe = pipe_.load(std::memory_order_acquire);
        if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE) {
            (void)CancelIoEx(pipe, nullptr);
        }
    }

    std::string dispatch(const std::string& request) {
        std::scoped_lock lock(runtime_mutex_);
        if (!runtime_) {
            return dispatch_error_json(SAO_AI_EDITOR_ERR_NOT_INITIALIZED);
        }
        return dispatch_runtime(runtime_.get(), request);
    }

    std::optional<std::string> next_event() {
        std::scoped_lock lock(runtime_mutex_);
        if (!runtime_) {
            return std::nullopt;
        }

        uint32_t required = 0;
        int32_t status = sao_ai_editor_runtime_next_event(
            runtime_.get(), nullptr, 0, &required);
        if (status == SAO_AI_EDITOR_OK && required == 0) {
            return std::nullopt;
        }
        if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL ||
            required > kMaximumPayload) {
            return dispatch_error_json(status);
        }

        std::vector<char> event(static_cast<size_t>(required) + 1U);
        status = sao_ai_editor_runtime_next_event(
            runtime_.get(), event.data(), static_cast<uint32_t>(event.size()),
            &required);
        if (status != SAO_AI_EDITOR_OK) {
            return dispatch_error_json(status);
        }
        return std::string(event.data(), required);
    }

    void close() noexcept {
        std::scoped_lock lock(runtime_mutex_);
        runtime_.reset();
    }

private:
    RuntimeHandle runtime_;
    ScopedHandle stop_event_;
    std::mutex runtime_mutex_;
    std::atomic<HANDLE> pipe_{nullptr};
};

class UiBridge final {
public:
    void publish(HWND window) noexcept {
        std::scoped_lock lock(mutex_);
        window_ = window;
    }

    void clear(HWND window) noexcept {
        std::scoped_lock lock(mutex_);
        if (window_ == window) {
            window_ = nullptr;
        }
    }

    void post(UINT message, WPARAM value = 0) noexcept {
        std::scoped_lock lock(mutex_);
        if (window_ != nullptr) {
            (void)PostMessageW(window_, message, value, 0);
        }
    }

private:
    std::mutex mutex_;
    HWND window_ = nullptr;
};

int run_pipe_requests(const std::shared_ptr<RuntimeSession>& session,
                      HANDLE pipe,
                      const std::shared_ptr<UiBridge>& bridge) {
    std::string message;
    while (receive_frame(pipe, session->stop_event(), message)) {
        if (message == kShutdownRequest) {
            session->close();
            if (!send_frame(pipe, session->stop_event(), kShutdownResponse)) {
                session->request_stop();
                if (bridge) {
                    bridge->post(kPipeStoppedMessage, 9);
                }
                return 9;
            }
            (void)FlushFileBuffers(pipe);
            session->request_stop();
            if (bridge) {
                bridge->post(kParentShutdownMessage);
            }
            return 0;
        }

        const std::string response = session->dispatch(message);
        if (!send_frame(pipe, session->stop_event(), response)) {
            const int result = session->stopped() ? 0 : 9;
            session->request_stop();
            if (bridge) {
                bridge->post(kPipeStoppedMessage,
                             static_cast<WPARAM>(result));
            }
            return result;
        }
    }

    const int result = session->stopped() ? 0 : 10;
    session->request_stop();
    if (bridge) {
        bridge->post(kPipeStoppedMessage, static_cast<WPARAM>(result));
    }
    return result;
}

class PipeWorker final {
public:
    PipeWorker(std::shared_ptr<RuntimeSession> session, ScopedHandle pipe,
               std::shared_ptr<UiBridge> bridge)
        : session_(std::move(session)), pipe_(std::move(pipe)),
          bridge_(std::move(bridge)) {
        session_->attach_pipe(pipe_.get());
        try {
            thread_ = std::thread([this] {
                exit_code_ = run_pipe_requests(session_, pipe_.get(), bridge_);
                session_->detach_pipe(pipe_.get());
            });
        } catch (...) {
            session_->detach_pipe(pipe_.get());
            throw;
        }
    }

    ~PipeWorker() {
        session_->request_stop();
        join();
        session_->detach_pipe(pipe_.get());
    }

    PipeWorker(const PipeWorker&) = delete;
    PipeWorker& operator=(const PipeWorker&) = delete;

    void join() noexcept {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int exit_code() const noexcept {
        return exit_code_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<RuntimeSession> session_;
    ScopedHandle pipe_;
    std::shared_ptr<UiBridge> bridge_;
    std::thread thread_;
    std::atomic<int> exit_code_{0};
};

struct UiState final {
    std::shared_ptr<RuntimeSession> session;
    std::shared_ptr<UiBridge> bridge;
    HWND output_edit = nullptr;
    HWND request_edit = nullptr;
    HWND send_button = nullptr;
    HWND clear_button = nullptr;
    HWND gpu_hunt_button = nullptr;
    HWND status_bar = nullptr;
    DWORD auto_exit_ms = 0;
    int exit_code = 0;
};

void set_status(const UiState& state, std::wstring_view text) {
    if (state.status_bar != nullptr) {
        SendMessageW(state.status_bar, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(text.data()));
    }
}

void append_output(const UiState& state, std::wstring_view text) {
    if (state.output_edit == nullptr || text.empty()) {
        return;
    }
    const int length = GetWindowTextLengthW(state.output_edit);
    SendMessageW(state.output_edit, EM_SETSEL, length, length);
    SendMessageW(state.output_edit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.data()));
    SendMessageW(state.output_edit, EM_SCROLLCARET, 0, 0);
}

std::wstring get_window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(copied, 0)));
    return text;
}

void dispatch_local_request(UiState& state) {
    const std::wstring request_wide = get_window_text(state.request_edit);
    const auto request = wide_to_utf8(request_wide);
    if (!request.has_value()) {
        set_status(state, L"Request must be non-empty valid Unicode");
        return;
    }

    append_output(state, L"\r\n[local request]\r\n");
    append_output(state, request_wide);
    append_output(state, L"\r\n[local response]\r\n");
    const std::string response = state.session->dispatch(*request);
    const auto response_wide = utf8_to_wide(response);
    append_output(state, response_wide.value_or(L"<invalid UTF-8 response>"));
    append_output(state, L"\r\n");
    set_status(state, L"Local request dispatched");
}

void poll_events(UiState& state) {
    constexpr size_t maximum_events_per_tick = 32;
    for (size_t index = 0; index < maximum_events_per_tick; ++index) {
        const auto event = state.session->next_event();
        if (!event.has_value()) {
            break;
        }
        append_output(state, L"\r\n[runtime event]\r\n");
        const auto event_wide = utf8_to_wide(*event);
        append_output(state, event_wide.value_or(L"<invalid UTF-8 event>"));
        append_output(state, L"\r\n");
    }
}

void layout_controls(HWND window, UiState& state) {
    RECT client{};
    if (!GetClientRect(window, &client)) {
        return;
    }
    SendMessageW(state.status_bar, WM_SIZE, 0, 0);
    RECT status_rect{};
    GetWindowRect(state.status_bar, &status_rect);
    const int status_height = status_rect.bottom - status_rect.top;
    constexpr int margin = 10;
    constexpr int gap = 8;
    constexpr int button_width = 88;
    constexpr int button_height = 28;
    const int width = static_cast<int>(
        std::max<LONG>(client.right - client.left, 320L));
    const int usable_height = static_cast<int>(std::max<LONG>(
        client.bottom - client.top - status_height, 260L));
    const int output_height = std::max(120, (usable_height * 55) / 100);
    const int request_top = margin + output_height + gap;
    const int request_height =
        std::max(70, usable_height - request_top - button_height - margin - gap);

    MoveWindow(state.output_edit, margin, margin, width - (margin * 2),
               output_height, TRUE);
    MoveWindow(state.request_edit, margin, request_top,
               width - (margin * 2), request_height, TRUE);
    const int button_top = request_top + request_height + gap;
    MoveWindow(state.send_button, margin, button_top, button_width,
               button_height, TRUE);
    MoveWindow(state.clear_button, margin + button_width + gap, button_top,
               button_width, button_height, TRUE);
    // GPU Hunt button lives to the right of the primary buttons so it does
    // not shift the muscle memory for Send / Clear.
    MoveWindow(state.gpu_hunt_button,
               margin + (button_width + gap) * 2, button_top,
               button_width + 24, button_height, TRUE);
}

bool create_controls(HWND window, UiState& state) {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(window, GWLP_HINSTANCE));
    state.output_edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutputEditId)),
        instance, nullptr);
    state.request_edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT",
        LR"({"jsonrpc":"2.0","id":1,"method":"runtime.initialize","params":{},"sao":{"protocolVersion":1}})",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRequestEditId)),
        instance, nullptr);
    state.send_button = CreateWindowExW(
        0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0,
        0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSendButtonId)), instance,
        nullptr);
    state.clear_button = CreateWindowExW(
        0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
        0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kClearButtonId)),
        instance, nullptr);
    state.gpu_hunt_button = CreateWindowExW(
        0, L"BUTTON", L"GPU Hunt",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGpuHuntButtonId)),
        instance, nullptr);
    state.status_bar = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"Ready", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, window, nullptr, instance, nullptr);
    if (state.output_edit == nullptr || state.request_edit == nullptr ||
        state.send_button == nullptr || state.clear_button == nullptr ||
        state.gpu_hunt_button == nullptr ||
        state.status_bar == nullptr) {
        return false;
    }

    const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND control : {state.output_edit, state.request_edit,
                         state.send_button, state.clear_button,
                         state.gpu_hunt_button, state.status_bar}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    append_output(state, L"SAO AI Editor native runtime ready.\r\n");
    return true;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    auto* state = reinterpret_cast<UiState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<UiState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE:
        if (!create_controls(window, *state)) {
            return -1;
        }
        state->bridge->publish(window);
        SetTimer(window, kEventTimerId, 100, nullptr);
        if (state->auto_exit_ms != 0) {
            SetTimer(window, kAutoExitTimerId, state->auto_exit_ms, nullptr);
        }
        return 0;
    case WM_SIZE:
        layout_controls(window, *state);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED &&
            LOWORD(wparam) == kSendButtonId) {
            dispatch_local_request(*state);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED &&
            LOWORD(wparam) == kClearButtonId) {
            SetWindowTextW(state->output_edit, L"");
            set_status(*state, L"Output cleared");
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED &&
            LOWORD(wparam) == kGpuHuntButtonId) {
            HINSTANCE inst = reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(window, GWLP_HINSTANCE));
            HWND panel = sao::ai_editor::gpu_hunt_panel_show(inst);
            set_status(*state,
                panel != nullptr ? L"GPU Hunt panel opened"
                                 : L"GPU Hunt panel failed to open");
            return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == kEventTimerId) {
            poll_events(*state);
            return 0;
        }
        if (wparam == kAutoExitTimerId) {
            state->session->request_stop();
            state->bridge->clear(window);
            DestroyWindow(window);
            return 0;
        }
        break;
    case kParentShutdownMessage:
        set_status(*state, L"Parent requested shutdown");
        state->exit_code = 0;
        state->bridge->clear(window);
        DestroyWindow(window);
        return 0;
    case kPipeStoppedMessage:
        state->exit_code = static_cast<int>(wparam);
        set_status(*state, L"Parent connection closed");
        state->bridge->clear(window);
        DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        state->session->request_stop();
        state->exit_code = 0;
        state->bridge->clear(window);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kEventTimerId);
        KillTimer(window, kAutoExitTimerId);
        state->bridge->clear(window);
        PostQuitMessage(state->exit_code);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class NativeWindow final {
public:
    NativeWindow(HINSTANCE instance, int show_command, bool hidden,
                 UiState& state)
        : instance_(instance), show_command_(show_command), hidden_(hidden),
          state_(state) {}

    bool create() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kWindowClassName;
        window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
        if (RegisterClassExW(&window_class) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        window_ = CreateWindowExW(
            0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 900, 720, nullptr, nullptr, instance_,
            &state_);
        if (window_ == nullptr) {
            return false;
        }
        if (!hidden_) {
            ShowWindow(window_, show_command_);
        }
        UpdateWindow(window_);
        return true;
    }

    int run() {
        MSG message{};
        while (true) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == 0) {
                return static_cast<int>(message.wParam);
            }
            if (result < 0) {
                return 1;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

private:
    HINSTANCE instance_;
    int show_command_;
    bool hidden_;
    UiState& state_;
    HWND window_ = nullptr;
};

std::shared_ptr<RuntimeSession>
create_runtime_session(const std::filesystem::path& workspace) {
    const auto workspace_utf8 = wide_to_utf8(workspace.native());
    if (!workspace_utf8.has_value()) {
        return nullptr;
    }

    SaoAiEditorRuntimeConfig runtime_config{};
    runtime_config.struct_size = sizeof(runtime_config);
    runtime_config.workspace_root_utf8 = workspace_utf8->c_str();
    sao_ai_editor_runtime_t raw_runtime = nullptr;
    const int32_t create_status =
        sao_ai_editor_runtime_create(&runtime_config, &raw_runtime);
    if (create_status != SAO_AI_EDITOR_OK || raw_runtime == nullptr) {
        return nullptr;
    }

    ScopedHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event) {
        sao_ai_editor_runtime_destroy(raw_runtime);
        return nullptr;
    }
    return std::make_shared<RuntimeSession>(
        RuntimeHandle(raw_runtime), std::move(stop_event));
}

int run_ui(HINSTANCE instance, int show_command, const Arguments& arguments,
           const std::shared_ptr<RuntimeSession>& session,
           ScopedHandle pipe = ScopedHandle()) {
    auto bridge = std::make_shared<UiBridge>();
    UiState state{session, bridge};
    state.auto_exit_ms = arguments.auto_exit_ms;
    NativeWindow window(instance, show_command, arguments.hidden_window,
                        state);
    if (!window.create()) {
        session->request_stop();
        return 11;
    }

    if (pipe) {
        PipeWorker worker(session, std::move(pipe), bridge);
        const int ui_result = window.run();
        session->request_stop();
        worker.join();
        session->close();
        return ui_result != 0 ? ui_result : worker.exit_code();
    }

    const int result = window.run();
    session->request_stop();
    session->close();
    return result;
}

int run_ui_smoke(HINSTANCE instance, int show_command,
                 const Arguments& arguments) {
    const auto workspace = resolve_workspace(arguments.workspace);
    if (!workspace.has_value()) {
        return 4;
    }
    const auto session = create_runtime_session(*workspace);
    if (!session) {
        return 7;
    }
    Arguments smoke_arguments = arguments;
    if (smoke_arguments.auto_exit_ms == 0) {
        smoke_arguments.auto_exit_ms = 1500;
    }
    return run_ui(instance, show_command, smoke_arguments, session);
}

int run_child(HINSTANCE instance, int show_command,
              const Arguments& arguments) {
    if (!is_local_pipe_name(arguments.pipe_name)) {
        return 3;
    }
    const auto workspace = resolve_workspace(arguments.workspace);
    if (!workspace.has_value()) {
        return 4;
    }

    auto pipe = connect_pipe(arguments.pipe_name);
    if (!pipe) {
        return 5;
    }

    ScopedHandle handshake_stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!handshake_stop) {
        return 5;
    }
    std::string message;
    if (!receive_frame(pipe.get(), handshake_stop.get(), message) ||
        message != kHandshakeRequest) {
        return 6;
    }

    const auto session = create_runtime_session(*workspace);
    if (!session) {
        return 7;
    }
    if (!send_frame(pipe.get(), session->stop_event(), kHandshakeResponse)) {
        return 8;
    }

    if (arguments.headless) {
        session->attach_pipe(pipe.get());
        const int result = run_pipe_requests(session, pipe.get(), nullptr);
        session->detach_pipe(pipe.get());
        session->close();
        return result;
    }
    return run_ui(instance, show_command, arguments, session,
                  std::move(pipe));
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    try {
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
        (void)InitCommonControlsEx(&controls);

        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == nullptr) {
            return 1;
        }
        Arguments arguments;
        const bool parsed = parse_arguments(argc, argv, arguments);
        LocalFree(argv);
        if (!parsed) {
            print_usage();
            return 2;
        }
        if (arguments.show_help) {
            print_usage();
            return 0;
        }
        if (arguments.cli_mode) {
            const auto workspace = resolve_workspace(arguments.workspace);
            if (!workspace.has_value()) {
                return 4;
            }
            const auto workspace_utf8 =
                wide_to_utf8(workspace->native());
            if (!workspace_utf8.has_value()) {
                return 4;
            }
            SaoAiEditorRuntimeConfig runtime_config{};
            runtime_config.struct_size = sizeof(runtime_config);
            runtime_config.workspace_root_utf8 = workspace_utf8->c_str();
            sao_ai_editor_runtime_t runtime = nullptr;
            if (sao_ai_editor_runtime_create(&runtime_config, &runtime) !=
                    SAO_AI_EDITOR_OK ||
                runtime == nullptr) {
                return 7;
            }
            const auto method_utf8 = wide_to_utf8(arguments.cli_method);
            if (!method_utf8.has_value()) {
                sao_ai_editor_runtime_destroy(runtime);
                return 4;
            }
            std::string params_text;
            if (arguments.cli_params_file.has_value()) {
                std::ifstream stream(*arguments.cli_params_file,
                                     std::ios::binary);
                if (!stream) {
                    sao_ai_editor_runtime_destroy(runtime);
                    return 4;
                }
                params_text.assign(
                    std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>());
            } else if (!arguments.cli_params_inline.empty()) {
                const auto inline_utf8 =
                    wide_to_utf8(arguments.cli_params_inline);
                if (!inline_utf8.has_value()) {
                    sao_ai_editor_runtime_destroy(runtime);
                    return 4;
                }
                params_text = *inline_utf8;
            } else {
                params_text = "{}";
            }
            std::string request =
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" +
                *method_utf8 + "\",\"params\":" + params_text + "}";
            uint32_t required = 0;
            int32_t status = sao_ai_editor_runtime_dispatch(
                runtime, request.data(),
                static_cast<uint32_t>(request.size()), nullptr, 0, &required);
            if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
                sao_ai_editor_runtime_destroy(runtime);
                return 13;
            }
            std::vector<char> response(static_cast<size_t>(required) + 1);
            status = sao_ai_editor_runtime_dispatch(
                runtime, nullptr, 0, response.data(),
                static_cast<uint32_t>(response.size()), &required);
            if (status != SAO_AI_EDITOR_OK) {
                sao_ai_editor_runtime_destroy(runtime);
                return 13;
            }
            const HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
            if (stdout_handle != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(stdout_handle, response.data(), required, &written,
                          nullptr);
                const char newline = '\n';
                WriteFile(stdout_handle, &newline, 1, &written, nullptr);
            }
            std::string_view response_view(response.data(), required);
            const bool has_error =
                response_view.find("\"error\":{") != std::string_view::npos;
            sao_ai_editor_runtime_destroy(runtime);
            return has_error ? 14 : 0;
        }
        if (arguments.mcp_server) {
            const auto workspace = resolve_workspace(arguments.workspace);
            if (!workspace.has_value()) {
                return 4;
            }
            return sao::ai_editor::native::run_mcp_server_stdio(*workspace);
        }
        if (arguments.gpu_hunt_only) {
            // Standalone GPU Hunt panel mode: no main window, no IPC
            // handshake, no runtime.  We just register a hidden owner window
            // so the panel is anchored to a HWND owned by this HINSTANCE,
            // show the panel, then pump messages until it closes.  The panel
            // itself already owns its own message loop timer; we just need
            // the process to stay alive.
            INITCOMMONCONTROLSEX icce{};
            icce.dwSize = sizeof(icce);
            icce.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
            InitCommonControlsEx(&icce);
            HWND panel = sao::ai_editor::gpu_hunt_panel_show(instance);
            if (panel == nullptr) {
                return 15;
            }
            MSG msg{};
            while (true) {
                BOOL rc = GetMessageW(&msg, nullptr, 0, 0);
                if (rc == 0 || rc == -1) {
                    break;
                }
                // Once the panel window has been destroyed and its class
                // singleton cleared, exit the loop.
                if (!IsWindow(panel)) {
                    // Drain any remaining posted messages then bail.
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            // The GpuHuntTool singleton owns a background tick thread; its
            // static destructor stops it during CRT teardown, so we do not
            // need to explicitly wire that here.  Reaching this point means
            // the panel window was closed and the message pump drained.
            return 0;
        }
        if (arguments.extension_host_mode) {
            const auto workspace = resolve_workspace(arguments.workspace);
            if (!workspace.has_value()) {
                return 4;
            }
            return sao::ai_editor::native::run_extension_host_stdio(
                *workspace,
                arguments.node_executable.value_or(
                    std::filesystem::path{}));
        }
#if SAO_AI_EDITOR_HAS_WEBVIEW
        if (arguments.webview_mode) {
            const auto workspace = resolve_workspace(arguments.workspace);
            if (!workspace.has_value()) {
                return 4;
            }
            const auto workspace_utf8 =
                wide_to_utf8(workspace->native());
            if (!workspace_utf8.has_value()) {
                return 4;
            }
            SaoAiEditorRuntimeConfig runtime_config{};
            runtime_config.struct_size = sizeof(runtime_config);
            runtime_config.workspace_root_utf8 = workspace_utf8->c_str();
            sao_ai_editor_runtime_t runtime = nullptr;
            const int32_t create_status =
                sao_ai_editor_runtime_create(&runtime_config, &runtime);
            if (create_status != SAO_AI_EDITOR_OK || runtime == nullptr) {
                return 7;
            }
            const auto url_utf8 = wide_to_utf8(arguments.webview_url);
            sao::ai_editor::native::WebViewConfig config;
            config.url = url_utf8.value_or(std::string{});
            config.user_data_folder =
                wide_to_utf8((*workspace / L".sao" /
                              L"webview").native()).value_or(std::string{});
            config.window_title = "SAO AI Editor";
            config.width = 1280;
            config.height = 800;
            config.bridge_native_runtime = true;
            config.runtime_handle = runtime;
            const int32_t status =
                sao::ai_editor::native::run_webview_bridge(config);
            sao_ai_editor_runtime_destroy(runtime);
            return status == SAO_AI_EDITOR_OK ? 0 : 12;
        }
#else
        if (arguments.webview_mode) {
            return 12;
        }
#endif
        if (arguments.ui_smoke_test) {
            return run_ui_smoke(instance, show_command, arguments);
        }
        return run_child(instance, show_command, arguments);
    } catch (...) {
        return 1;
    }
}
