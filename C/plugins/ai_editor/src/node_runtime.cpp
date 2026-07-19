#include "node_runtime.h"

#include "sao/ai_editor/mcp_codec.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <unordered_map>
#include <vector>

#include "native_runtime_internal.h"

namespace sao::ai_editor::native {
namespace {

constexpr uint32_t kMaxMessageBytes = 8U * 1024U * 1024U;
constexpr uint32_t kMaxCommandLineBytes = 32U * 1024U;

void join_worker(std::thread& worker) {
    if (!worker.joinable()) {
        return;
    }
    if (worker.get_id() == std::this_thread::get_id()) {
        // Every worker owns a shared_ptr<NodeRuntime> captured at boot.  The
        // only thread that cannot join here is therefore still keeping the
        // object alive until this lambda returns; detaching is safe for the
        // thread object and does not expose a dangling `this`.
        worker.detach();
        return;
    }
    worker.join();
}

struct ScopedHandle {
    HANDLE value = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : value(handle) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(ScopedHandle&& other) noexcept
        : value(std::exchange(other.value, nullptr)) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value, nullptr));
        }
        return *this;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
        value = handle;
    }
    HANDLE release() noexcept { return std::exchange(value, nullptr); }
    HANDLE get() const noexcept { return value; }
    explicit operator bool() const noexcept {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }
};

std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring result = L"\"";
    for (size_t index = 0; index < argument.size(); ++index) {
        size_t backslashes = 0;
        while (index < argument.size() && argument[index] == L'\\') {
            ++backslashes;
            ++index;
        }
        if (index == argument.size()) {
            result.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(argument[index]);
        }
    }
    result.push_back(L'"');
    return result;
}

std::wstring build_command_line(const std::wstring& executable,
                                const std::vector<std::wstring>& arguments) {
    std::wstring result = quote_argument(executable);
    for (const auto& argument : arguments) {
        result.push_back(L' ');
        result.append(quote_argument(argument));
    }
    return result;
}

std::wstring build_environment_block(
    const std::vector<std::pair<std::wstring, std::wstring>>& extras) {
    LPWCH parent = GetEnvironmentStringsW();
    std::unordered_map<std::wstring, std::wstring> entries;
    if (parent != nullptr) {
        for (const wchar_t* cursor = parent; *cursor != L'\0';) {
            std::wstring_view entry(cursor);
            cursor += entry.size() + 1;
            const size_t equal = entry.find(L'=');
            if (equal == 0 || equal == std::wstring_view::npos) {
                continue;
            }
            std::wstring name(entry.substr(0, equal));
            for (wchar_t& character : name) {
                character = static_cast<wchar_t>(::towupper(character));
            }
            entries[std::move(name)] = std::wstring(entry.substr(equal + 1));
        }
        FreeEnvironmentStringsW(parent);
    }
    for (const auto& [name, value] : extras) {
        std::wstring upper = name;
        for (wchar_t& character : upper) {
            character = static_cast<wchar_t>(::towupper(character));
        }
        entries[std::move(upper)] = value;
    }
    std::vector<std::wstring> sorted;
    sorted.reserve(entries.size());
    for (auto& [name, value] : entries) {
        sorted.push_back(name + L"=" + value);
    }
    std::sort(sorted.begin(), sorted.end());
    std::wstring block;
    for (const auto& line : sorted) {
        block.append(line);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

NodeRuntime::~NodeRuntime() { shutdown(); }

int32_t NodeRuntime::boot(const BootOptions& options) {
    if (process_ != nullptr) {
        return SAO_AI_EDITOR_ERR_ALREADY_RUNNING;
    }
    if (options.node_executable.empty() ||
        !valid_utf8(options.node_executable) ||
        options.entry_script.empty() || !valid_utf8(options.entry_script)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::wstring executable = utf8_to_wide(options.node_executable);
    std::vector<std::wstring> arguments;
    arguments.reserve(options.node_args.size() + 1 +
                      options.extra_args.size());
    for (const auto& argument : options.node_args) {
        if (!valid_utf8(argument)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        arguments.push_back(utf8_to_wide(argument));
    }
    arguments.push_back(utf8_to_wide(options.entry_script));
    for (const auto& argument : options.extra_args) {
        if (!valid_utf8(argument)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        arguments.push_back(utf8_to_wide(argument));
    }
    std::wstring command_line = build_command_line(executable, arguments);
    if (command_line.size() > kMaxCommandLineBytes) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::vector<wchar_t> command_line_buffer(command_line.begin(),
                                             command_line.end());
    command_line_buffer.push_back(L'\0');
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    environment.reserve(options.environment.size());
    for (const auto& [name, value] : options.environment) {
        if (!valid_utf8(name) || !valid_utf8(value)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        environment.emplace_back(utf8_to_wide(name), utf8_to_wide(value));
    }
    const std::wstring env_block = build_environment_block(environment);
    std::vector<wchar_t> env_buffer(env_block.begin(), env_block.end());
    std::wstring working_directory;
    if (!options.working_directory.empty()) {
        if (!valid_utf8(options.working_directory)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        working_directory = utf8_to_wide(options.working_directory);
    }

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &security_attributes,
                    64 * 1024)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    ScopedHandle owned_stdin_read(stdin_read);
    ScopedHandle owned_stdin_write(stdin_write);
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security_attributes,
                    256 * 1024)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    ScopedHandle owned_stdout_read(stdout_read);
    ScopedHandle owned_stdout_write(stdout_write);
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stderr_read, &stderr_write, &security_attributes,
                    64 * 1024)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    ScopedHandle owned_stderr_read(stderr_read);
    ScopedHandle owned_stderr_write(stderr_write);
    if (!SetHandleInformation(owned_stdin_write.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(owned_stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(owned_stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = owned_stdin_read.get();
    startup.hStdOutput = owned_stdout_write.get();
    startup.hStdError = owned_stderr_write.get();

    PROCESS_INFORMATION process_information{};
    const DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT |
                                 CREATE_NO_WINDOW;
    if (!CreateProcessW(nullptr, command_line_buffer.data(), nullptr, nullptr,
                        TRUE, creation_flags, env_buffer.data(),
                        working_directory.empty() ? nullptr
                                                  : working_directory.c_str(),
                        &startup, &process_information)) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    process_ = process_information.hProcess;
    thread_ = process_information.hThread;
    stdin_write_ = owned_stdin_write.release();
    stdout_read_ = owned_stdout_read.release();
    stderr_read_ = owned_stderr_read.release();
    owned_stdin_read.reset();
    owned_stdout_write.reset();
    owned_stderr_write.reset();

    {
        std::lock_guard<std::mutex> guard(shutdown_mutex_);
        shutdown_started_ = false;
    }
    stopping_.store(false, std::memory_order_release);
    state_.store(State::running, std::memory_order_release);
    const std::shared_ptr<NodeRuntime> lifetime = weak_from_this().lock();
    if (!lifetime) {
        shutdown();
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    reader_ = std::thread([lifetime] { lifetime->reader_loop(); });
    dispatcher_ = std::thread([lifetime] { lifetime->dispatch_loop(); });
    stderr_reader_ = std::thread([lifetime] { lifetime->stderr_loop(); });

    Json initialize_params{
        {"protocolVersion", "sao-ai-editor/1"},
        {"clientInfo", Json{{"name", "sao-ai-editor"},
                             {"version", "1.0"}}},
        {"capabilities", Json{{"tools", Json::object()},
                               {"languages", Json::object()},
                               {"workspace", Json::object()}}}};
    Json init_result;
    const int32_t init_status =
        request("host.initialize", initialize_params, options.startup_ms,
                init_result);
    if (init_status != SAO_AI_EDITOR_OK) {
        shutdown();
        return init_status;
    }
    {
        std::lock_guard<std::mutex> guard(state_mutex_);
        protocol_version_ =
            init_result.value("protocolVersion", std::string{});
        server_info_ = init_result.value("serverInfo", Json::object());
        capabilities_ = init_result.value("capabilities", Json::object());
    }
    (void)notify("host.initialized", Json::object());
    return SAO_AI_EDITOR_OK;
}

bool NodeRuntime::alive() const noexcept {
    if (process_ == nullptr ||
        state_.load(std::memory_order_acquire) != State::running) {
        return false;
    }
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

int32_t NodeRuntime::request(std::string_view method, const Json& params,
                             uint32_t timeout_ms, Json& result) {
    int64_t id = 0;
    std::shared_ptr<Pending> pending;
    try {
        if (process_ == nullptr) {
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        const State state = state_.load(std::memory_order_acquire);
        if (state != State::running) {
            return state == State::protocol_failed
                       ? SAO_AI_EDITOR_ERR_PROTOCOL
                       : SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        id = next_id_.fetch_add(1, std::memory_order_relaxed);
        pending = std::make_shared<Pending>();
        {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            const State pending_state = state_.load(std::memory_order_acquire);
            if (pending_state != State::running) {
                return pending_state == State::protocol_failed
                           ? SAO_AI_EDITOR_ERR_PROTOCOL
                           : SAO_AI_EDITOR_ERR_IPC_CLOSED;
            }
            pending_[id] = pending;
        }
        Json request{{"jsonrpc", "2.0"},
                     {"id", id},
                     {"method", std::string(method)},
                     {"params", params}};
        if (!send_framed(request.dump())) {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_.erase(id);
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        const uint32_t budget = timeout_ms == 0 ? 30000 : timeout_ms;
        if (pending->future.wait_for(std::chrono::milliseconds(budget)) !=
            std::future_status::ready) {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_.erase(id);
            return SAO_AI_EDITOR_ERR_TIMEOUT;
        }
        Json envelope = pending->future.get();
        if (envelope.contains("error")) {
            result = envelope.at("error");
            const Json data = result.value("data", Json::object());
            if (data.is_object() && data.contains("status") &&
                data["status"].is_number_integer()) {
                return data["status"].get<int32_t>();
            }
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        result = envelope.at("result");
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        if (id != 0) {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_.erase(id);
        }
        fail_protocol("node runtime request failed");
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

int32_t NodeRuntime::notify(std::string_view method, const Json& params) {
    try {
        if (process_ == nullptr) {
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        const State state = state_.load(std::memory_order_acquire);
        if (state != State::running) {
            return state == State::protocol_failed
                       ? SAO_AI_EDITOR_ERR_PROTOCOL
                       : SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        Json notification{{"jsonrpc", "2.0"},
                           {"method", std::string(method)},
                           {"params", params}};
        return send_framed(notification.dump()) ? SAO_AI_EDITOR_OK
                                                : SAO_AI_EDITOR_ERR_IPC_CLOSED;
    } catch (...) {
        fail_protocol("node runtime notification failed");
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

bool NodeRuntime::send_framed(const std::string& payload) {
    std::string frame =
        "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    frame.append(payload);
    return send_raw(frame);
}

bool NodeRuntime::send_raw(const std::string& data) {
    bool sent = true;
    {
        std::lock_guard<std::mutex> guard(write_mutex_);
        if (stdin_write_ == nullptr ||
            state_.load(std::memory_order_acquire) != State::running) {
            return false;
        }
        size_t offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(data.size() - offset,
                                 static_cast<size_t>(64U * 1024U)));
            if (!WriteFile(stdin_write_, data.data() + offset, chunk, &written,
                           nullptr) ||
                written == 0) {
                sent = false;
                break;
            }
            offset += written;
        }
    }
    if (!sent) {
        state_.store(State::closed, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        fail_pending("node runtime input closed",
                     SAO_AI_EDITOR_ERR_IPC_CLOSED);
        dispatch_ready_.notify_all();
    }
    return sent;
}

void NodeRuntime::reader_loop() {
    sao_ai_editor_mcp_decoder_t decoder = nullptr;
    int32_t close_status = SAO_AI_EDITOR_ERR_IPC_CLOSED;
    std::string close_message = "node runtime output closed";
    try {
        if (sao_ai_editor_mcp_decoder_create(kMaxMessageBytes, &decoder) !=
            SAO_AI_EDITOR_OK) {
            close_message = "node response decoder initialization failed";
        } else {
            std::vector<char> buffer(64U * 1024U);
            while (!stopping_.load(std::memory_order_acquire)) {
                DWORD read = 0;
                if (!ReadFile(stdout_read_, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &read,
                              nullptr) ||
                    read == 0) {
                    break;
                }
                uint32_t required = 0;
                int32_t status = sao_ai_editor_mcp_decoder_feed(
                    decoder, buffer.data(), read, nullptr, 0, &required);
                if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL ||
                    required == 0) {
                    close_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                    close_message = "node runtime protocol framing failed";
                    break;
                }
                std::vector<char> messages(static_cast<size_t>(required) + 1U);
                status = sao_ai_editor_mcp_decoder_feed(
                    decoder, nullptr, 0, messages.data(),
                    static_cast<uint32_t>(messages.size()), &required);
                if (status != SAO_AI_EDITOR_OK) {
                    close_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                    close_message = "node runtime protocol drain failed";
                    break;
                }
                Json parsed = Json::parse(
                    messages.data(), messages.data() + required, nullptr,
                    false);
                if (!parsed.is_array() || parsed.is_discarded()) {
                    close_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                    close_message = "node runtime returned invalid JSON";
                    break;
                }
                for (auto& message : parsed) {
                    if (!message.is_object() ||
                        !dispatch_message(std::move(message))) {
                        close_status = SAO_AI_EDITOR_ERR_PROTOCOL;
                        close_message =
                            "node runtime returned an invalid JSON-RPC message";
                        break;
                    }
                }
                if (close_status == SAO_AI_EDITOR_ERR_PROTOCOL) {
                    break;
                }
            }
        }
    } catch (...) {
        close_status = SAO_AI_EDITOR_ERR_PROTOCOL;
        close_message = "node runtime reader failed";
    }
    if (decoder != nullptr) {
        sao_ai_editor_mcp_decoder_destroy(decoder);
    }
    if (close_status == SAO_AI_EDITOR_ERR_PROTOCOL) {
        state_.store(State::protocol_failed, std::memory_order_release);
    } else if (state_.load(std::memory_order_acquire) !=
               State::protocol_failed) {
        state_.store(State::closed, std::memory_order_release);
    }
    stopping_.store(true, std::memory_order_release);
    fail_pending(close_message, close_status);
    dispatch_ready_.notify_all();
}

void NodeRuntime::dispatch_loop() {
    try {
        for (;;) {
            Json message;
            {
                std::unique_lock<std::mutex> lock(dispatch_mutex_);
                dispatch_ready_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                           !dispatch_queue_.empty();
                });
                if (dispatch_queue_.empty()) {
                    return;
                }
                message = std::move(dispatch_queue_.front());
                dispatch_queue_.pop_front();
            }
            if (message.contains("id")) {
                handle_request_from_node(std::move(message));
            } else {
                handle_notification_from_node(std::move(message));
            }
        }
    } catch (...) {
        fail_protocol("node runtime dispatch failed");
    }
}

void NodeRuntime::stderr_loop() {
    try {
        std::vector<char> buffer(4096);
        while (!stopping_.load(std::memory_order_acquire)) {
            DWORD read = 0;
            if (!ReadFile(stderr_read_, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr) ||
                read == 0) {
                break;
            }
            std::lock_guard<std::mutex> guard(state_mutex_);
            stderr_tail_.append(buffer.data(), read);
            if (stderr_tail_.size() > 32U * 1024U) {
                stderr_tail_.erase(0, stderr_tail_.size() - 32U * 1024U);
            }
        }
    } catch (...) {
        fail_protocol("node runtime stderr reader failed");
    }
}

bool NodeRuntime::dispatch_message(Json message) {
    const auto valid_id = [](const Json& value) {
         return value.is_string() || value.is_number_integer() ||
             value.is_number_unsigned();
    };
    if (!message.is_object() || message.is_discarded() ||
        !message.contains("jsonrpc") || !message["jsonrpc"].is_string() ||
        message["jsonrpc"] != "2.0") {
        return false;
    }
    const bool has_method = message.contains("method");
    const bool has_id = message.contains("id");
    if (has_method) {
        if (!message["method"].is_string() ||
            message["method"].get_ref<const std::string&>().empty() ||
            (has_id && !valid_id(message["id"])) ||
            (message.contains("params") &&
             !message["params"].is_object() &&
             !message["params"].is_array()) ||
            message.contains("result") || message.contains("error")) {
            return false;
        }
        {
            std::lock_guard<std::mutex> guard(dispatch_mutex_);
            dispatch_queue_.push_back(std::move(message));
        }
        dispatch_ready_.notify_one();
        return true;
    }
    const bool has_result = message.contains("result");
    const bool has_error = message.contains("error");
    if (!has_id || !valid_id(message["id"]) || has_result == has_error) {
        return false;
    }
    if (has_error &&
        (!message["error"].is_object() ||
         !message["error"].contains("code") ||
         !message["error"]["code"].is_number_integer() ||
         !message["error"].contains("message") ||
         !message["error"]["message"].is_string())) {
        return false;
    }
    if (!message["id"].is_number_integer() &&
        !message["id"].is_number_unsigned()) {
        return false;
    }
    const int64_t id = message["id"].get<int64_t>();
    {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            const auto found = pending_.find(id);
            if (found != pending_.end()) {
                pending = found->second;
                pending_.erase(found);
            }
        }
        if (!pending) {
            return false;
        }
        try {
            pending->promise.set_value(std::move(message));
        } catch (...) {
        }
        return true;
    }
}

void NodeRuntime::handle_request_from_node(Json message) {
    NativeRuntime* runtime = native_runtime_.load(std::memory_order_acquire);
    Json response{{"jsonrpc", "2.0"}, {"id", message["id"]}};
    if (runtime == nullptr) {
        response["error"] = Json{{"code", -32000},
                                  {"message", "native runtime unavailable"}};
        (void)send_framed(response.dump());
        return;
    }
    const std::string method = message.value("method", std::string{});
    const Json params = message.value("params", Json::object());
    Json result;
    const int32_t status = runtime->dispatch_extension_call(method, params,
                                                             result);
    if (status != SAO_AI_EDITOR_OK) {
        response["error"] = Json{{"code", -32000},
                                  {"message", result.value("message",
                                                            "call failed")},
                                  {"data", Json{{"status", status}}}};
    } else {
        response["result"] = std::move(result);
    }
    (void)send_framed(response.dump());
}

void NodeRuntime::handle_notification_from_node(Json message) {
    NativeRuntime* runtime = native_runtime_.load(std::memory_order_acquire);
    if (runtime == nullptr) {
        return;
    }
    const std::string method = message.value("method", std::string{});
    const Json params = message.value("params", Json::object());
    Json result;
    const int32_t status = runtime->dispatch_extension_call(method, params,
                                                             result);
    if (status != SAO_AI_EDITOR_OK) {
        std::lock_guard<std::mutex> guard(state_mutex_);
        stderr_tail_.append("[node notification] ");
        stderr_tail_.append(method);
        stderr_tail_.append(" failed with status ");
        stderr_tail_.append(std::to_string(status));
        stderr_tail_.push_back('\n');
        if (stderr_tail_.size() > 32U * 1024U) {
            stderr_tail_.erase(0, stderr_tail_.size() - 32U * 1024U);
        }
    }
}

void NodeRuntime::fail_pending(std::string_view message, int32_t status) {
    std::lock_guard<std::mutex> guard(pending_mutex_);
    for (auto& [id, pending] : pending_) {
        (void)id;
        try {
            Json error{{"error", Json{{"code", -32000},
                                        {"message", std::string(message)},
                                        {"data",
                                         Json{{"status",
                                                 status}}}}}};
            pending->promise.set_value(std::move(error));
        } catch (...) {
        }
    }
    pending_.clear();
}

void NodeRuntime::fail_protocol(std::string_view message) {
    state_.store(State::protocol_failed, std::memory_order_release);
    stopping_.store(true, std::memory_order_release);
    fail_pending(message, SAO_AI_EDITOR_ERR_PROTOCOL);
    dispatch_ready_.notify_all();
}

void NodeRuntime::shutdown() {
    std::lock_guard<std::mutex> guard(shutdown_mutex_);
    if (shutdown_started_) {
        return;
    }
    shutdown_started_ = true;
    stopping_.store(true, std::memory_order_release);
    state_.store(State::closed, std::memory_order_release);
    fail_pending("node runtime shut down", SAO_AI_EDITOR_ERR_IPC_CLOSED);
    dispatch_ready_.notify_all();
    if (process_ != nullptr) {
        {
            std::lock_guard<std::mutex> write_guard(write_mutex_);
            if (stdin_write_ != nullptr) {
                CloseHandle(stdin_write_);
                stdin_write_ = nullptr;
            }
        }
        WaitForSingleObject(process_, 2000);
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 500);
        }
    }
    if (stdout_read_ != nullptr) {
        CancelIoEx(stdout_read_, nullptr);
    }
    if (stderr_read_ != nullptr) {
        CancelIoEx(stderr_read_, nullptr);
    }
    join_worker(reader_);
    dispatch_ready_.notify_all();
    join_worker(dispatcher_);
    join_worker(stderr_reader_);
    if (stdout_read_ != nullptr) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (stderr_read_ != nullptr) {
        CloseHandle(stderr_read_);
        stderr_read_ = nullptr;
    }
    if (thread_ != nullptr) {
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    native_runtime_.store(nullptr, std::memory_order_release);
}

}  // namespace sao::ai_editor::native
