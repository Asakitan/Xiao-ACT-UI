#include "sao/ai_editor/mcp_client.h"
#include "sao/ai_editor/mcp_codec.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {
namespace {

constexpr uint32_t kMaxServers = 64;
constexpr uint32_t kMaxServerNameBytes = 128;
constexpr uint32_t kMaxCommandLineBytes = 32U * 1024U;
constexpr uint32_t kMaxEnvBytes = 32U * 1024U;
constexpr uint32_t kDefaultStartupMs = 15000U;
constexpr uint32_t kDefaultRequestMs = 30000U;
constexpr uint32_t kShutdownGraceMs = 2000U;
constexpr uint32_t kMcpProtocolVersion = 2024;  // "2024-11-05" era

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
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    void reset(HANDLE handle = nullptr) noexcept {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
        value = handle;
    }
    HANDLE get() const noexcept { return value; }
    HANDLE release() noexcept { return std::exchange(value, nullptr); }
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
    if (parent == nullptr) {
        std::wstring empty;
        empty.push_back(L'\0');
        return empty;
    }
    std::unordered_map<std::wstring, std::wstring> entries;
    for (const wchar_t* cursor = parent; *cursor != L'\0';) {
        const std::wstring_view entry(cursor);
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

int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct PendingResponse {
    std::promise<Json> promise;
    std::future<Json> future;
    PendingResponse() : future(promise.get_future()) {}
};

class McpServer final {
public:
    McpServer() = default;
    ~McpServer() { shutdown_locked(); }

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    int32_t start(const Json& config) {
        name_ = config.value("name", "");
        if (name_.empty() || name_.size() > kMaxServerNameBytes ||
            !valid_utf8(name_)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string command = config.value("command", "");
        if (command.empty() || !valid_utf8(command)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::wstring> arguments;
        if (config.contains("args")) {
            if (!config["args"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : config["args"]) {
                if (!item.is_string()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                arguments.push_back(utf8_to_wide(item.get<std::string>()));
            }
        }
        std::vector<std::pair<std::wstring, std::wstring>> environment;
        if (config.contains("env")) {
            if (!config["env"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            size_t env_bytes = 0;
            for (const auto& [key, value] : config["env"].items()) {
                if (!value.is_string() || !valid_utf8(key) ||
                    !valid_utf8(value.get<std::string>())) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                env_bytes += key.size() + value.get<std::string>().size() + 2U;
                if (env_bytes > kMaxEnvBytes) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                environment.emplace_back(utf8_to_wide(key),
                                         utf8_to_wide(value.get<std::string>()));
            }
        }
        const std::string cwd = config.value("cwd", "");
        if (!cwd.empty() && !valid_utf8(cwd)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        startup_ms_ = std::clamp<uint32_t>(config.value("startupMs", 0U), 500U,
                                           120000U);
        if (startup_ms_ == 0) {
            startup_ms_ = kDefaultStartupMs;
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
        if (!SetHandleInformation(owned_stdin_write.get(),
                                  HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(owned_stdout_read.get(),
                                  HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(owned_stderr_read.get(),
                                  HANDLE_FLAG_INHERIT, 0)) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }

        const std::wstring executable = utf8_to_wide(command);
        std::wstring command_line = build_command_line(executable, arguments);
        if (command_line.size() > kMaxCommandLineBytes) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<wchar_t> command_line_buffer(command_line.begin(),
                                                 command_line.end());
        command_line_buffer.push_back(L'\0');
        const std::wstring env_block = build_environment_block(environment);
        std::vector<wchar_t> env_buffer(env_block.begin(), env_block.end());
        std::wstring working_directory;
        if (!cwd.empty()) {
            working_directory = utf8_to_wide(cwd);
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
                            working_directory.empty()
                                ? nullptr
                                : working_directory.c_str(),
                            &startup, &process_information)) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        process_ = ScopedHandle(process_information.hProcess);
        thread_ = ScopedHandle(process_information.hThread);
        stdin_write_ = std::move(owned_stdin_write);
        stdout_read_ = std::move(owned_stdout_read);
        stderr_read_ = std::move(owned_stderr_read);
        // Server-side ends now belong to the child; close ours so EOF works.
        owned_stdin_read.reset();
        owned_stdout_write.reset();
        owned_stderr_write.reset();

        stopping_.store(false, std::memory_order_release);
        reader_ = std::thread([this] { reader_loop(); });
        stderr_reader_ = std::thread([this] { stderr_reader_loop(); });
        started_at_ms_ = unix_milliseconds();
        return handshake();
    }

    const std::string& name() const noexcept { return name_; }

    int32_t list_tools(Json& result) {
        return call("tools/list", Json::object(), 15000U, result);
    }

    int32_t call_tool(const std::string& tool_name,
                      const Json& arguments,
                      uint32_t timeout_ms,
                      Json& result) {
        Json params = Json{{"name", tool_name}};
        params["arguments"] = arguments.is_null() ? Json::object() : arguments;
        return call("tools/call", params, timeout_ms, result);
    }

    int32_t list_prompts(Json& result) {
        return call("prompts/list", Json::object(), 15000U, result);
    }

    int32_t list_resources(Json& result) {
        return call("resources/list", Json::object(), 15000U, result);
    }

    int32_t read_resource(const std::string& uri, uint32_t timeout_ms,
                          Json& result) {
        return call("resources/read", Json{{"uri", uri}}, timeout_ms, result);
    }

    void shutdown_locked() {
        std::lock_guard<std::mutex> guard(shutdown_mutex_);
        if (shutdown_started_) {
            return;
        }
        shutdown_started_ = true;
        stopping_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> pending_guard(pending_mutex_);
            for (auto& [id, pending_entry] : pending_) {
                (void)id;
                try {
                    Json error{{"error",
                                {{"code", -32000},
                                 {"message", "server shut down"}}}};
                    pending_entry->promise.set_value(std::move(error));
                } catch (...) {
                }
            }
            pending_.clear();
        }
        if (process_) {
            const bool exited =
                WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;
            if (!exited && stdin_write_) {
                // A single-shot polite shutdown request; ignore failures.
                const std::string request = R"({"jsonrpc":"2.0","method":"shutdown"})";
                send_raw(request);
                CloseHandle(stdin_write_.release());
                WaitForSingleObject(process_.get(), kShutdownGraceMs);
            } else if (stdin_write_) {
                CloseHandle(stdin_write_.release());
            }
            if (WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) {
                TerminateProcess(process_.get(), 1);
                WaitForSingleObject(process_.get(), 500);
            }
        }
        if (stdout_read_) {
            CancelIoEx(stdout_read_.get(), nullptr);
        }
        if (stderr_read_) {
            CancelIoEx(stderr_read_.get(), nullptr);
        }
        if (reader_.joinable()) {
            reader_.join();
        }
        if (stderr_reader_.joinable()) {
            stderr_reader_.join();
        }
    }

    Json snapshot() const {
        std::lock_guard<std::mutex> guard(state_mutex_);
        return Json{{"name", name_},
                    {"protocolVersion", protocol_version_},
                    {"serverInfo", server_info_},
                    {"capabilities", capabilities_},
                    {"startedAt", started_at_ms_}};
    }

private:
    int32_t call(std::string_view method, const Json& params,
                 uint32_t timeout_ms, Json& result) {
        if (stopping_.load(std::memory_order_acquire)) {
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        const uint32_t budget = timeout_ms == 0
            ? kDefaultRequestMs
            : std::clamp<uint32_t>(timeout_ms, 100U, 10U * 60U * 1000U);
        const int64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        auto response = std::make_shared<PendingResponse>();
        {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_[id] = response;
        }
        Json request{{"jsonrpc", "2.0"},
                     {"id", id},
                     {"method", std::string(method)},
                     {"params", params}};
        const std::string payload = dump_json(request);
        if (!send_framed(payload)) {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_.erase(id);
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        if (response->future.wait_for(std::chrono::milliseconds(budget)) !=
            std::future_status::ready) {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_.erase(id);
            return SAO_AI_EDITOR_ERR_TIMEOUT;
        }
        Json envelope = response->future.get();
        if (envelope.contains("error")) {
            result = envelope["error"];
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        result = envelope.value("result", Json::object());
        return SAO_AI_EDITOR_OK;
    }

    int32_t handshake() {
        Json init_params{
            {"protocolVersion", "2024-11-05"},
            {"capabilities",
             Json{{"roots", Json{{"listChanged", false}}},
                  {"sampling", Json::object()}}},
            {"clientInfo",
             Json{{"name", "sao-ai-editor"}, {"version", "1.0"}}}};
        Json result;
        const int32_t status =
            call("initialize", init_params, startup_ms_, result);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        {
            std::lock_guard<std::mutex> guard(state_mutex_);
            protocol_version_ = result.value("protocolVersion", "");
            server_info_ = result.value("serverInfo", Json::object());
            capabilities_ = result.value("capabilities", Json::object());
        }
        const std::string notification =
            R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})";
        (void)send_framed(notification);
        return SAO_AI_EDITOR_OK;
    }

    bool send_framed(const std::string& payload) {
        std::string header =
            "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
        std::lock_guard<std::mutex> guard(write_mutex_);
        return send_raw(header) && send_raw(payload);
    }

    bool send_raw(const std::string& data) {
        if (!stdin_write_) {
            return false;
        }
        size_t offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            const DWORD chunk =
                static_cast<DWORD>(std::min<size_t>(data.size() - offset,
                                                    static_cast<size_t>(64U * 1024U)));
            if (!WriteFile(stdin_write_.get(), data.data() + offset, chunk,
                           &written, nullptr) ||
                written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    void reader_loop() {
        sao_ai_editor_mcp_decoder_t decoder = nullptr;
        if (sao_ai_editor_mcp_decoder_create(4U * 1024U * 1024U, &decoder) !=
            SAO_AI_EDITOR_OK) {
            return;
        }
        std::vector<char> buffer(64U * 1024U);
        while (!stopping_.load(std::memory_order_acquire)) {
            DWORD read = 0;
            if (!ReadFile(stdout_read_.get(), buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr) ||
                read == 0) {
                break;
            }
            uint32_t required = 0;
            int32_t status = sao_ai_editor_mcp_decoder_feed(
                decoder, buffer.data(), read, nullptr, 0, &required);
            if (status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL || required == 0) {
                continue;
            }
            std::vector<char> messages(static_cast<size_t>(required) + 1U);
            status = sao_ai_editor_mcp_decoder_feed(
                decoder, nullptr, 0, messages.data(),
                static_cast<uint32_t>(messages.size()), &required);
            if (status != SAO_AI_EDITOR_OK) {
                continue;
            }
            Json parsed = Json::parse(messages.data(), messages.data() + required,
                                      nullptr, false);
            if (!parsed.is_array()) {
                continue;
            }
            for (auto& message : parsed) {
                dispatch_message(std::move(message));
            }
        }
        sao_ai_editor_mcp_decoder_destroy(decoder);
    }

    void stderr_reader_loop() {
        std::vector<char> buffer(4096);
        while (!stopping_.load(std::memory_order_acquire)) {
            DWORD read = 0;
            if (!ReadFile(stderr_read_.get(), buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr) ||
                read == 0) {
                break;
            }
            std::lock_guard<std::mutex> guard(state_mutex_);
            stderr_tail_.append(buffer.data(), read);
            if (stderr_tail_.size() > 16U * 1024U) {
                stderr_tail_.erase(0, stderr_tail_.size() - 16U * 1024U);
            }
        }
    }

    void dispatch_message(Json message) {
        if (!message.is_object()) {
            return;
        }
        if (message.contains("id") && (message.contains("result") ||
                                        message.contains("error"))) {
            const auto& id_value = message["id"];
            int64_t id = 0;
            if (id_value.is_number_integer()) {
                id = id_value.get<int64_t>();
            } else {
                return;
            }
            std::shared_ptr<PendingResponse> pending;
            {
                std::lock_guard<std::mutex> guard(pending_mutex_);
                const auto found = pending_.find(id);
                if (found != pending_.end()) {
                    pending = found->second;
                    pending_.erase(found);
                }
            }
            if (pending) {
                try {
                    pending->promise.set_value(std::move(message));
                } catch (...) {
                }
            }
        }
        // Notifications are silently discarded — the AI editor runtime does
        // not yet surface MCP change notifications to the user.
    }

    std::string name_;
    uint32_t startup_ms_ = kDefaultStartupMs;
    int64_t started_at_ms_ = 0;
    ScopedHandle process_;
    ScopedHandle thread_;
    ScopedHandle stdin_write_;
    ScopedHandle stdout_read_;
    ScopedHandle stderr_read_;
    std::thread reader_;
    std::thread stderr_reader_;
    std::atomic<bool> stopping_{false};
    std::mutex write_mutex_;
    std::mutex pending_mutex_;
    std::unordered_map<int64_t, std::shared_ptr<PendingResponse>> pending_;
    std::atomic<int64_t> next_id_{1};
    mutable std::mutex state_mutex_;
    std::string protocol_version_;
    Json server_info_;
    Json capabilities_;
    std::string stderr_tail_;
    std::mutex shutdown_mutex_;
    bool shutdown_started_ = false;
};

}  // namespace

}  // namespace sao::ai_editor::native

struct SaoAiEditorMcpClient {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<
        sao::ai_editor::native::McpServer>> servers;
    std::string pending_output;
    std::mutex pending_mutex;

    std::shared_ptr<sao::ai_editor::native::McpServer> find_locked(
        const std::string& name) const {
        const auto found = servers.find(name);
        if (found == servers.end()) {
            return nullptr;
        }
        return found->second;
    }
};

namespace {

int32_t emit_json(SaoAiEditorMcpClient& client,
                  const sao::ai_editor::native::Json& value,
                  char* output,
                  uint32_t capacity,
                  uint32_t* out_length) {
    std::lock_guard<std::mutex> guard(client.pending_mutex);
    if (client.pending_output.empty()) {
        client.pending_output = sao::ai_editor::native::dump_json(value);
    }
    const int32_t status = sao::ai_editor::native::copy_text_to_caller(
        client.pending_output, output, capacity, out_length);
    if (status == SAO_AI_EDITOR_OK) {
        client.pending_output.clear();
    }
    return status;
}

// Reuse the pending_output buffer for a drain-only second call
// (output != nullptr, request_json == nullptr).  Returns true iff pending
// buffer was found and consumed into the caller-supplied output — caller
// should return the yielded status directly.
bool try_drain_pending(SaoAiEditorMcpClient& client, char* output,
                      uint32_t capacity, uint32_t* out_length,
                      int32_t* out_status) {
    std::lock_guard<std::mutex> guard(client.pending_mutex);
    if (client.pending_output.empty()) {
        return false;
    }
    *out_status = sao::ai_editor::native::copy_text_to_caller(
        client.pending_output, output, capacity, out_length);
    if (*out_status == SAO_AI_EDITOR_OK) {
        client.pending_output.clear();
    }
    return true;
}

}  // namespace

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_create(sao_ai_editor_mcp_client_t* out_handle) {
    try {
        if (out_handle == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_handle = new SaoAiEditorMcpClient();
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_register(sao_ai_editor_mcp_client_t handle,
                                  const char* config_json,
                                  uint32_t config_len) {
    try {
        if (handle == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (config_json == nullptr || config_len == 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string_view input(config_json, config_len);
        if (!sao::ai_editor::native::valid_utf8(input)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        sao::ai_editor::native::Json config =
            sao::ai_editor::native::Json::parse(input, nullptr, false);
        if (!config.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        auto server = std::make_shared<sao::ai_editor::native::McpServer>();
        const int32_t status = server->start(config);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        std::lock_guard<std::mutex> guard(handle->mutex);
        if (handle->servers.size() >= sao::ai_editor::native::kMaxServers) {
            server->shutdown_locked();
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string name = server->name();
        auto existing = handle->find_locked(name);
        if (existing) {
            existing->shutdown_locked();
        }
        handle->servers[name] = std::move(server);
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_servers(sao_ai_editor_mcp_client_t handle,
                                      char* json_out,
                                      uint32_t json_cap,
                                      uint32_t* out_len) {
    try {
        if (handle == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        int32_t drain_status = SAO_AI_EDITOR_OK;
        if (try_drain_pending(*handle, json_out, json_cap, out_len,
                              &drain_status)) {
            return drain_status;
        }
        sao::ai_editor::native::Json result = sao::ai_editor::native::Json::array();
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            for (const auto& [name, server] : handle->servers) {
                (void)name;
                result.push_back(server->snapshot());
            }
        }
        return emit_json(*handle, result, json_out, json_cap, out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

namespace {

int32_t aggregate(sao_ai_editor_mcp_client_t handle,
                  const std::string& method,
                  const std::string& field,
                  char* json_out,
                  uint32_t json_cap,
                  uint32_t* out_len) {
    if (handle == nullptr) {
        return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
    }
    if (out_len == nullptr) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    int32_t drain_status = SAO_AI_EDITOR_OK;
    if (try_drain_pending(*handle, json_out, json_cap, out_len,
                          &drain_status)) {
        return drain_status;
    }
    std::vector<std::pair<std::string,
                          std::shared_ptr<sao::ai_editor::native::McpServer>>>
        servers;
    {
        std::lock_guard<std::mutex> guard(handle->mutex);
        for (auto& [name, server] : handle->servers) {
            servers.emplace_back(name, server);
        }
    }
    sao::ai_editor::native::Json aggregated = sao::ai_editor::native::Json::array();
    for (auto& [name, server] : servers) {
        sao::ai_editor::native::Json result;
        int32_t status = SAO_AI_EDITOR_OK;
        if (method == "tools/list") {
            status = server->list_tools(result);
        } else if (method == "prompts/list") {
            status = server->list_prompts(result);
        } else if (method == "resources/list") {
            status = server->list_resources(result);
        }
        if (status != SAO_AI_EDITOR_OK) {
            continue;
        }
        if (result.contains(field) && result[field].is_array()) {
            for (auto& item : result[field]) {
                if (item.is_object()) {
                    item["server"] = name;
                    aggregated.push_back(std::move(item));
                }
            }
        }
    }
    return emit_json(*handle, aggregated, json_out, json_cap, out_len);
}

}  // namespace

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_tools(sao_ai_editor_mcp_client_t handle,
                                    char* json_out,
                                    uint32_t json_cap,
                                    uint32_t* out_len) {
    try {
        return aggregate(handle, "tools/list", "tools", json_out, json_cap,
                         out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_prompts(sao_ai_editor_mcp_client_t handle,
                                      char* json_out,
                                      uint32_t json_cap,
                                      uint32_t* out_len) {
    try {
        return aggregate(handle, "prompts/list", "prompts", json_out, json_cap,
                         out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_list_resources(sao_ai_editor_mcp_client_t handle,
                                        char* json_out,
                                        uint32_t json_cap,
                                        uint32_t* out_len) {
    try {
        return aggregate(handle, "resources/list", "resources", json_out,
                         json_cap, out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_call_tool(sao_ai_editor_mcp_client_t handle,
                                   const char* request_json,
                                   uint32_t request_len,
                                   char* response_out,
                                   uint32_t response_cap,
                                   uint32_t* out_len) {
    try {
        if (handle == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        int32_t drain_status = SAO_AI_EDITOR_OK;
        if (try_drain_pending(*handle, response_out, response_cap, out_len,
                              &drain_status)) {
            return drain_status;
        }
        if (request_json == nullptr || request_len == 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string_view input(request_json, request_len);
        if (!sao::ai_editor::native::valid_utf8(input)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        sao::ai_editor::native::Json parsed =
            sao::ai_editor::native::Json::parse(input, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("server") ||
            !parsed["server"].is_string() || !parsed.contains("name") ||
            !parsed["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string server_name = parsed["server"].get<std::string>();
        const std::string tool_name = parsed["name"].get<std::string>();
        const sao::ai_editor::native::Json arguments =
            parsed.value("arguments", sao::ai_editor::native::Json::object());
        const uint32_t timeout_ms = parsed.value("timeoutMs", 0U);
        std::shared_ptr<sao::ai_editor::native::McpServer> server;
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            server = handle->find_locked(server_name);
        }
        if (!server) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        sao::ai_editor::native::Json result;
        const int32_t status =
            server->call_tool(tool_name, arguments, timeout_ms, result);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result["server"] = server_name;
        return emit_json(*handle, result, response_out, response_cap, out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_read_resource(sao_ai_editor_mcp_client_t handle,
                                       const char* request_json,
                                       uint32_t request_len,
                                       char* response_out,
                                       uint32_t response_cap,
                                       uint32_t* out_len) {
    try {
        if (handle == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        int32_t drain_status = SAO_AI_EDITOR_OK;
        if (try_drain_pending(*handle, response_out, response_cap, out_len,
                              &drain_status)) {
            return drain_status;
        }
        if (request_json == nullptr || request_len == 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string_view input(request_json, request_len);
        if (!sao::ai_editor::native::valid_utf8(input)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        sao::ai_editor::native::Json parsed =
            sao::ai_editor::native::Json::parse(input, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("server") ||
            !parsed["server"].is_string() || !parsed.contains("uri") ||
            !parsed["uri"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string server_name = parsed["server"].get<std::string>();
        const std::string uri = parsed["uri"].get<std::string>();
        const uint32_t timeout_ms = parsed.value("timeoutMs", 0U);
        std::shared_ptr<sao::ai_editor::native::McpServer> server;
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            server = handle->find_locked(server_name);
        }
        if (!server) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        sao::ai_editor::native::Json result;
        const int32_t status = server->read_resource(uri, timeout_ms, result);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result["server"] = server_name;
        return emit_json(*handle, result, response_out, response_cap, out_len);
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_close(sao_ai_editor_mcp_client_t handle,
                               const char* server_name) {
    try {
        if (handle == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        std::vector<std::shared_ptr<sao::ai_editor::native::McpServer>> victims;
        {
            std::lock_guard<std::mutex> guard(handle->mutex);
            if (server_name == nullptr || server_name[0] == '\0') {
                for (auto& [name, server] : handle->servers) {
                    (void)name;
                    victims.push_back(std::move(server));
                }
                handle->servers.clear();
            } else {
                const std::string_view name(server_name);
                if (!sao::ai_editor::native::valid_utf8(name)) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                const auto found = handle->servers.find(std::string(name));
                if (found == handle->servers.end()) {
                    return SAO_AI_EDITOR_ERR_NOT_FOUND;
                }
                victims.push_back(std::move(found->second));
                handle->servers.erase(found);
            }
        }
        for (auto& victim : victims) {
            victim->shutdown_locked();
        }
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_client_destroy(sao_ai_editor_mcp_client_t handle) {
    try {
        if (handle != nullptr) {
            sao_ai_editor_mcp_client_close(handle, nullptr);
            delete handle;
        }
    } catch (...) {
    }
}
