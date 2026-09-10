#include "workbench_native_adapter.h"

#include "native_utils.h"
#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ai_editor/mcp_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sao::ai_editor::workbench {
namespace {

using json = nlohmann::json;

constexpr size_t kMaximumQueue = 128;
constexpr size_t kMaximumCompletions = 256;
constexpr size_t kMaximumEvents = 512;
constexpr size_t kMaximumRequestBytes = 4U * 1024U * 1024U;
constexpr size_t kMaximumResponseBytes = 4U * 1024U * 1024U;
constexpr size_t kMaximumProcessOutputBytes = 1024U * 1024U;
constexpr size_t kMaximumConsoleInputBytes = 64U * 1024U;
constexpr size_t kMaximumConsoleQueue = 64;
constexpr size_t kMaximumWorkflowHistory = 64;
constexpr size_t kMaximumConsecutiveJobs = 4;
constexpr size_t kMaximumEventPollFailures = 5;
constexpr uint32_t kRequestTimeoutMs = 5000;
constexpr uint32_t kStatusPollTimeoutMs = 250;
constexpr uint32_t kOrphanCancelTimeoutMs = 100;
constexpr std::chrono::milliseconds kEventPollInterval{75};
constexpr std::chrono::seconds kRequestEventPollLifetime{2};
constexpr std::chrono::minutes kEventPollLifetime{10};
constexpr size_t kMaximumJsonDepth = 64;
constexpr size_t kMaximumJsonNodes = 16384;
constexpr size_t kMaximumJsonStringBytes = 1024U * 1024U;

struct AdapterJob {
    std::string document_token;
    std::string request_id;
    std::string method;
    json args;
    uint64_t document_generation{};
};

struct RpcReply {
    bool ok{};
    json result;
    json error_data;
    std::string error_code;
    std::string error_message;
};

std::string transport_error_code(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_NOT_RUNNING:
        return "SAO_BACKEND_NOT_RUNNING";
    case SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL:
        return "SAO_BACKEND_CONNECT_FAILED";
    case SAO_AI_EDITOR_ERR_IPC_TIMEOUT:
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return "SAO_BACKEND_TIMEOUT";
    case SAO_AI_EDITOR_ERR_IPC_CLOSED:
        return "SAO_BACKEND_CLOSED";
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return "SAO_BACKEND_RESPONSE_TOO_LARGE";
    case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
        return "SAO_INVALID_ARGUMENT";
    case SAO_AI_EDITOR_ERR_PERMISSION_DENIED:
        return "SAO_PERMISSION_DENIED";
    case SAO_AI_EDITOR_ERR_CANCELLED:
        return "SAO_CANCELLED";
    default:
        return "SAO_BACKEND_ERROR";
    }
}

std::string transport_error_message(int32_t status) {
    switch (status) {
    case SAO_AI_EDITOR_ERR_NOT_RUNNING:
        return "Native AI backend is not running.";
    case SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL:
        return "Native AI backend connection failed.";
    case SAO_AI_EDITOR_ERR_IPC_TIMEOUT:
    case SAO_AI_EDITOR_ERR_TIMEOUT:
        return "Native AI backend request timed out.";
    case SAO_AI_EDITOR_ERR_IPC_CLOSED:
        return "Native AI backend connection closed.";
    case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
        return "Native AI backend response is too large.";
    default:
        return "Native AI backend request failed with status " + std::to_string(status) + ".";
    }
}

bool valid_string_member(const json& value, std::string_view name) {
    const auto found = value.find(std::string(name));
    return found != value.end() && found->is_string();
}

bool json_within_budget(const json& value, size_t depth, size_t* nodes) {
    if (*nodes >= kMaximumJsonNodes || depth > kMaximumJsonDepth)
        return false;
    ++*nodes;
    if (value.is_string())
        return value.get_ref<const std::string&>().size() <= kMaximumJsonStringBytes;
    if (value.is_array()) {
        for (const auto& item : value)
            if (!json_within_budget(item, depth + 1, nodes))
                return false;
        return true;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items())
            if (key.size() > 1024 || !json_within_budget(item, depth + 1, nodes))
                return false;
    }
    return true;
}

bool json_within_budget(const json& value) {
    size_t nodes = 0;
    return json_within_budget(value, 0, &nodes);
}

std::string string_member_or(const json& value, std::string_view name, std::string fallback = {}) {
    if (!value.is_object())
        return fallback;
    const auto found = value.find(std::string(name));
    return found != value.end() && found->is_string() ? found->get<std::string>()
                                                      : std::move(fallback);
}

json array_from_result(const json& result, std::initializer_list<std::string_view> names) {
    if (result.is_array())
        return result;
    if (!result.is_object())
        return json::array();
    for (const auto name : names) {
        const auto found = result.find(std::string(name));
        if (found != result.end() && found->is_array())
            return *found;
    }
    return json::array();
}

bool set_dotted_value(json& root, std::string_view path, const json& value) {
    if (path.empty() || path.size() > 512)
        return false;
    json* current = &root;
    size_t begin = 0;
    for (;;) {
        const size_t separator = path.find('.', begin);
        const std::string key(path.substr(begin, separator - begin));
        if (key.empty() || key.size() > 128)
            return false;
        if (separator == std::string_view::npos) {
            (*current)[key] = value;
            return true;
        }
        json& next = (*current)[key];
        if (!next.is_object())
            next = json::object();
        current = &next;
        begin = separator + 1;
    }
}

const json* dotted_value(const json& root, std::string_view path) {
    const json* current = &root;
    size_t begin = 0;
    while (current->is_object()) {
        const size_t separator = path.find('.', begin);
        const std::string key(path.substr(begin, separator - begin));
        const auto found = current->find(key);
        if (found == current->end())
            return nullptr;
        current = &*found;
        if (separator == std::string_view::npos)
            return current;
        begin = separator + 1;
    }
    return nullptr;
}

void collect_target_values(const json& node, std::string path, std::string_view target,
                           json& targets) {
    if (node.is_object()) {
        for (auto item = node.begin(); item != node.end(); ++item) {
            const std::string child = path.empty() ? item.key() : path + "." + item.key();
            collect_target_values(item.value(), child, target, targets);
        }
        return;
    }
    if (path.empty())
        return;
    json& entry = targets[path];
    if (!entry.is_object())
        entry = json::object();
    if (!entry.contains("values") || !entry["values"].is_object())
        entry["values"] = json::object();
    entry["values"][std::string(target)] = node;
}

json settings_overrides(const RpcReply& reply) {
    if (!reply.ok || !reply.result.is_object())
        return json::object();
    const auto overrides = reply.result.find("overrides");
    return overrides != reply.result.end() && overrides->is_object() ? *overrides : json::object();
}

NativeAdapterCompletion failed_completion(const AdapterJob& job, std::string code,
                                          std::string message, json error_data = nullptr) {
    NativeAdapterCompletion completion;
    completion.document_token = job.document_token;
    completion.request_id = job.request_id;
    completion.error_code = std::move(code);
    completion.error_message = std::move(message);
    completion.error_data = std::move(error_data);
    return completion;
}

NativeAdapterCompletion successful_completion(const AdapterJob& job, json result) {
    NativeAdapterCompletion completion;
    completion.document_token = job.document_token;
    completion.request_id = job.request_id;
    completion.ok = true;
    completion.result = std::move(result);
    return completion;
}

NativeAdapterCompletion rpc_completion(const AdapterJob& job, RpcReply reply);

struct RunBinding {
    std::string document_token;
    std::string provider_id;
    uint64_t document_generation{};
    uint64_t sequence{};
    size_t status_failures{};
};

struct PendingWorkflow {
    AdapterJob job;
    std::string execution_id;
    std::string frontend_run_id;
    std::string workflow_id;
    json last_snapshot;
    size_t status_failures{};
};

struct WorkflowExecutionBinding {
    std::string execution_id;
    std::string workflow_id;
    std::string document_token;
    uint64_t document_generation{};
    bool active{};
};

#if defined(_WIN32)
struct ManagedProcess {
    std::string id;
    std::string name;
    std::string kind;
    std::string document_token;
    uint64_t document_generation{};
    std::string output;
    HANDLE process{};
    HANDLE job{};
    HANDLE stdout_read{};
    HANDLE stdin_write{};
    DWORD process_id{};
    DWORD exit_code{STILL_ACTIVE};
    bool running{true};
    mutable std::mutex output_mutex;
    std::thread output_reader;
    std::atomic<bool> output_reader_done{false};
    std::mutex input_mutex;
    std::condition_variable input_wake;
    std::deque<std::string> input_queue;
    std::thread input_writer;
    std::atomic<bool> input_writer_done{false};
    bool input_stopping{};
};
#endif

} // namespace

struct NativeAdapter {
    sao_ai_editor_launcher_t launcher{};
    std::thread::id owner_thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<AdapterJob> jobs;
    std::deque<NativeAdapterCompletion> completions;
    std::deque<NativeAdapterEvent> events;
    std::thread worker;
    std::string document_token;
    std::string active_agent;
    std::string active_conversation_id;
    std::unordered_map<std::string, std::string> provider_conversations;
    std::string active_run_id;
    std::string run_poll_cursor;
    std::string workflow_poll_cursor;
    std::unordered_map<std::string, RunBinding> runs;
    std::unordered_map<std::string, PendingWorkflow> workflows;
    std::unordered_map<std::string, WorkflowExecutionBinding> workflow_execution_by_frontend;
    std::deque<std::string> workflow_history_order;
    std::deque<std::string> orphan_run_ids;
    std::deque<std::string> orphan_workflow_ids;
    std::filesystem::path workspace_root;
    json editor_state = json::object();
    std::unordered_map<std::string, std::vector<int64_t>> breakpoints;
    json task_definitions = json::array();
    json debug_configurations = json::array();
    std::unordered_map<std::string, json> mcp_configs;
#if defined(_WIN32)
    std::unordered_map<std::string, std::unique_ptr<ManagedProcess>> processes;
    uint64_t process_counter{};
    bool process_reset_pending{};
#endif
    uint64_t backend_request_id{};
    uint64_t run_sequence{};
    uint64_t document_generation{1};
    size_t in_flight{};
    size_t consecutive_jobs{};
    size_t consecutive_event_poll_failures{};
    size_t event_reset_empty_passes{};
    size_t dropped_events{};
    bool stopping{};
    bool worker_exited{};
    bool worker_failed{};
    bool event_reset_pending{};
    bool event_polling{};
    bool workspace_ready{};
    std::chrono::steady_clock::time_point next_event_poll{};
    std::chrono::steady_clock::time_point event_poll_deadline{};
    std::chrono::steady_clock::time_point orphan_cleanup_deadline{};
};

namespace {

bool adapter_on_owner(const NativeAdapter& adapter) noexcept {
    return adapter.owner_thread == std::this_thread::get_id();
}

bool job_current(NativeAdapter& adapter, const AdapterJob& job) {
    std::lock_guard lock(adapter.mutex);
    return !adapter.stopping && job.document_token == adapter.document_token &&
           job.document_generation == adapter.document_generation;
}

void request_event_poll_locked(NativeAdapter& adapter,
                               std::chrono::steady_clock::duration lifetime) {
    const auto now = std::chrono::steady_clock::now();
    const auto deadline = now + lifetime;
    adapter.event_polling = true;
    if (adapter.event_poll_deadline < deadline)
        adapter.event_poll_deadline = deadline;
    if (adapter.next_event_poll > now)
        adapter.next_event_poll = now;
    adapter.wake.notify_one();
}

void remember_workflow_binding_locked(NativeAdapter& adapter, std::string frontend_id,
                                      WorkflowExecutionBinding binding) {
    const auto history = std::find(adapter.workflow_history_order.begin(),
                                   adapter.workflow_history_order.end(), frontend_id);
    if (history != adapter.workflow_history_order.end())
        adapter.workflow_history_order.erase(history);
    const bool active = binding.active;
    adapter.workflow_execution_by_frontend[frontend_id] = std::move(binding);
    if (!active)
        adapter.workflow_history_order.push_back(std::move(frontend_id));
    while (adapter.workflow_history_order.size() > kMaximumWorkflowHistory) {
        const std::string expired = std::move(adapter.workflow_history_order.front());
        adapter.workflow_history_order.pop_front();
        const auto found = adapter.workflow_execution_by_frontend.find(expired);
        if (found != adapter.workflow_execution_by_frontend.end() && !found->second.active)
            adapter.workflow_execution_by_frontend.erase(found);
    }
}

void enqueue_unique(std::deque<std::string>& queue, const std::string& id) {
    if (!id.empty() && std::find(queue.begin(), queue.end(), id) == queue.end())
        queue.push_back(id);
}

RpcReply call_backend(NativeAdapter& adapter, std::string_view method, const json& params,
                      uint32_t timeout_ms = kRequestTimeoutMs) {
    RpcReply reply;
    if (adapter.launcher == nullptr) {
        reply.error_code = "SAO_BACKEND_NOT_RUNNING";
        reply.error_message = "Native AI backend is not attached.";
        return reply;
    }
    const uint64_t id = ++adapter.backend_request_id;
    json request{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", params}};
    std::string request_body;
    try {
        request_body = request.dump();
    } catch (...) {
        reply.error_code = "SAO_SERIALIZATION_FAILED";
        reply.error_message = "Native backend request serialization failed.";
        return reply;
    }
    if (request_body.size() > kMaximumRequestBytes ||
        request_body.size() > std::numeric_limits<uint32_t>::max()) {
        reply.error_code = "SAO_REQUEST_TOO_LARGE";
        reply.error_message = "Native backend request exceeds 4 MiB.";
        return reply;
    }

    thread_local std::vector<char> response(kMaximumResponseBytes);
    uint32_t response_len = 0;
    int32_t status = sao_ai_editor_request(
        adapter.launcher, request_body.data(), static_cast<uint32_t>(request_body.size()),
        response.data(), static_cast<uint32_t>(response.size()), &response_len, timeout_ms);
    if (status != SAO_AI_EDITOR_OK) {
        reply.error_code = transport_error_code(status);
        reply.error_message = transport_error_message(status);
        return reply;
    }
    if (response_len > response.size()) {
        reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
        reply.error_message = "Native backend returned an invalid response length.";
        return reply;
    }
    const json document =
        json::parse(response.data(), response.data() + response_len, nullptr, false, false);
    const auto response_id = document.is_object() ? document.find("id") : document.end();
    if (!document.is_object() || !valid_string_member(document, "jsonrpc") ||
        document["jsonrpc"] != "2.0" || response_id == document.end() ||
        response_id->type() != request["id"].type() || *response_id != request["id"]) {
        reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
        reply.error_message = "Native backend returned an invalid JSON-RPC response.";
        return reply;
    }
    const auto error = document.find("error");
    const auto result = document.find("result");
    const bool has_error = error != document.end();
    const bool has_result = result != document.end();
    if (has_error == has_result) {
        reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
        reply.error_message = "Native backend response must contain exactly one result or error.";
        return reply;
    }
    if (has_error) {
        if (!error->is_object()) {
            reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
            reply.error_message = "Native backend error payload is invalid.";
            return reply;
        }
        reply.error_code = "SAO_BACKEND_REJECTED";
        reply.error_message = "Native backend rejected the request.";
        const auto code = error->find("code");
        const auto message = error->find("message");
        if (code == error->end() || message == error->end() || !message->is_string() ||
            message->get_ref<const std::string&>().empty() ||
            message->get_ref<const std::string&>().size() > 4096 ||
            (!code->is_number_integer() && !code->is_string())) {
            reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
            reply.error_message = "Native backend error payload is invalid.";
            return reply;
        }
        if (code->is_number_integer()) {
            reply.error_code = "SAO_RPC_" + std::to_string(code->get<int64_t>());
        } else {
            const std::string& code_text = code->get_ref<const std::string&>();
            if (code_text.empty() || code_text.size() > 128) {
                reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
                reply.error_message = "Native backend error code is invalid.";
                return reply;
            }
            reply.error_code = code_text;
        }
        reply.error_message = message->get_ref<const std::string&>();
        const auto data = error->find("data");
        if (data != error->end() && !data->is_null() && json_within_budget(*data))
            reply.error_data = *data;
        return reply;
    }
    if (!json_within_budget(*result)) {
        reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
        reply.error_message = "Native backend result exceeds the structural budget.";
        return reply;
    }
    reply.ok = true;
    reply.result = *result;
    return reply;
}

RpcReply ensure_workspace(NativeAdapter& adapter) {
    RpcReply reply;
    if (adapter.workspace_ready) {
        reply.ok = true;
        reply.result = {{"workspaceRoot",
                         sao::ai_editor::native::wide_to_utf8(adapter.workspace_root.native())}};
        return reply;
    }
    reply = call_backend(adapter, "runtime.initialize", json::object());
    if (!reply.ok)
        return reply;
    const std::string root = string_member_or(reply.result, "workspaceRoot");
    if (root.empty() ||
        !sao::ai_editor::native::normalize_root(root, adapter.workspace_root, false)) {
        reply.ok = false;
        reply.error_code = "SAO_WORKSPACE_UNAVAILABLE";
        reply.error_message = "Native workspace root is unavailable.";
        return reply;
    }
    adapter.workspace_ready = true;
    return reply;
}

bool resolve_workspace_path(NativeAdapter& adapter, std::string_view value, bool for_write,
                            std::filesystem::path& result) {
    RpcReply workspace = ensure_workspace(adapter);
    return workspace.ok && sao::ai_editor::native::resolve_bounded_path(adapter.workspace_root,
                                                                        value, for_write, result);
}

std::string relative_workspace_path(const NativeAdapter& adapter,
                                    const std::filesystem::path& path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, adapter.workspace_root, error);
    return error ? std::string{} : sao::ai_editor::native::wide_to_utf8(relative.native());
}

std::string file_language(std::string_view path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos)
        return "plaintext";
    std::string extension(path.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == "cpp" || extension == "cc" || extension == "cxx" || extension == "h" ||
        extension == "hpp")
        return "cpp";
    if (extension == "c")
        return "c";
    if (extension == "js" || extension == "mjs" || extension == "cjs")
        return "javascript";
    if (extension == "ts" || extension == "tsx")
        return "typescript";
    if (extension == "py")
        return "python";
    if (extension == "json")
        return "json";
    if (extension == "md")
        return "markdown";
    if (extension == "html" || extension == "htm")
        return "html";
    if (extension == "css")
        return "css";
    if (extension == "cmake")
        return "cmake";
    return "plaintext";
}

std::string workspace_resource_path(std::string value) {
    if (value.rfind("file:///", 0) == 0) {
        value.erase(0, 8);
        std::string decoded;
        decoded.reserve(value.size());
        const auto hex = [](char character) -> int {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            return -1;
        };
        for (size_t index = 0; index < value.size(); ++index) {
            if (value[index] == '%' && index + 2 < value.size()) {
                const int high = hex(value[index + 1]);
                const int low = hex(value[index + 2]);
                if (high >= 0 && low >= 0) {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                    continue;
                }
            }
            decoded.push_back(value[index]);
        }
        value = std::move(decoded);
        std::replace(value.begin(), value.end(), '/', '\\');
    }
    return value;
}

std::string workspace_file_uri(const std::filesystem::path& path) {
    std::string native = sao::ai_editor::native::wide_to_utf8(path.native());
    std::replace(native.begin(), native.end(), '\\', '/');
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(native.size() + 8);
    for (const unsigned char byte : native) {
        const bool allowed = std::isalnum(byte) != 0 || byte == '-' || byte == '_' || byte == '.' ||
                             byte == '~' || byte == '/' || byte == ':';
        if (allowed) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0F]);
        }
    }
    return "file:///" + encoded;
}

bool text_position_offset(const std::wstring& text, const json& position, size_t* output) {
    if (output == nullptr || !position.is_object())
        return false;
    const auto line_value = position.find("line");
    const auto character_value = position.find("character");
    if (line_value == position.end() || character_value == position.end() ||
        !line_value->is_number_integer() || !character_value->is_number_integer())
        return false;
    const int64_t line = line_value->get<int64_t>();
    const int64_t character = character_value->get<int64_t>();
    if (line < 0 || character < 0)
        return false;
    size_t offset = 0;
    for (int64_t current_line = 0; current_line < line; ++current_line) {
        const size_t newline = text.find(L'\n', offset);
        if (newline == std::wstring::npos)
            return false;
        offset = newline + 1;
    }
    const size_t line_end = text.find(L'\n', offset);
    const size_t available = (line_end == std::wstring::npos ? text.size() : line_end) - offset;
    if (static_cast<uint64_t>(character) > available)
        return false;
    *output = offset + static_cast<size_t>(character);
    return true;
}

void push_event_locked(NativeAdapter& adapter, NativeAdapterEvent event);

#if defined(_WIN32)
bool expand_workspace_variables(const NativeAdapter& adapter, std::string* value) {
    if (value == nullptr)
        return false;
    const std::string root = sao::ai_editor::native::wide_to_utf8(adapter.workspace_root.native());
    const std::string name =
        sao::ai_editor::native::wide_to_utf8(adapter.workspace_root.filename().native());
    const auto replace_all = [&](std::string_view token, const std::string& replacement) {
        size_t offset = 0;
        while ((offset = value->find(token, offset)) != std::string::npos) {
            value->replace(offset, token.size(), replacement);
            offset += replacement.size();
        }
    };
    replace_all("${workspaceFolder}", root);
    replace_all("${workspaceFolderBasename}", name);
    return value->find("${") == std::string::npos;
}

std::wstring quote_process_argument(std::wstring_view value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void close_managed_process(ManagedProcess& process, bool terminate) {
    {
        std::lock_guard lock(process.input_mutex);
        process.input_stopping = true;
        process.input_queue.clear();
    }
    process.input_wake.notify_all();
    if (terminate && process.running && process.job != nullptr)
        (void)TerminateJobObject(process.job, ERROR_CANCELLED);
    if (terminate && process.running && process.process != nullptr &&
        WaitForSingleObject(process.process, 250) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process.process, ERROR_CANCELLED);
    }
    if (process.job != nullptr) {
        CloseHandle(process.job);
        process.job = nullptr;
    }
    if (process.output_reader.joinable()) {
        if (!process.output_reader_done.load(std::memory_order_acquire))
            (void)CancelSynchronousIo(process.output_reader.native_handle());
        process.output_reader.join();
    }
    if (process.input_writer.joinable()) {
        if (!process.input_writer_done.load(std::memory_order_acquire))
            (void)CancelSynchronousIo(process.input_writer.native_handle());
        process.input_wake.notify_all();
        process.input_writer.join();
    }
    if (process.stdin_write != nullptr) {
        CloseHandle(process.stdin_write);
        process.stdin_write = nullptr;
    }
    if (process.stdout_read != nullptr) {
        CloseHandle(process.stdout_read);
        process.stdout_read = nullptr;
    }
    if (process.process != nullptr) {
        CloseHandle(process.process);
        process.process = nullptr;
    }
    process.running = false;
}

json managed_process_snapshot(const ManagedProcess& process) {
    std::string output;
    {
        std::lock_guard lock(process.output_mutex);
        output = process.output;
    }
    return {{process.kind == "debug" ? "sessionId" : "executionId", process.id},
            {"name", process.name},
            {"kind", process.kind},
            {"type", process.kind},
            {"processId", process.process_id},
            {"running", process.running},
            {"output", std::move(output)},
            {"exitCode", process.running ? json(nullptr) : json(process.exit_code)}};
}

bool launch_managed_process(NativeAdapter& adapter, std::string name, std::string kind,
                            std::string command, const json& arguments, bool shell,
                            const std::filesystem::path& cwd, std::string document_token,
                            uint64_t document_generation, json& result,
                            std::string& error_message) {
    if (command.empty() || command.size() > 32768 || !arguments.is_array()) {
        error_message = "Process command is invalid.";
        return false;
    }
    if (!expand_workspace_variables(adapter, &command)) {
        error_message = "Process command contains an unsupported variable.";
        return false;
    }
    std::wstring command_line;
    std::wstring application;
    if (shell) {
        std::array<wchar_t, 32768> comspec{};
        const DWORD length =
            GetEnvironmentVariableW(L"COMSPEC", comspec.data(), static_cast<DWORD>(comspec.size()));
        if (length == 0 || length >= comspec.size()) {
            error_message = "COMSPEC is unavailable.";
            return false;
        }
        application.assign(comspec.data(), length);
        const std::wstring shell_command = sao::ai_editor::native::utf8_to_wide(command);
        if (shell_command.empty()) {
            error_message = "Shell command encoding is invalid.";
            return false;
        }
        command_line = quote_process_argument(application) + L" /d /s /c \"" + shell_command;
    } else {
        application = sao::ai_editor::native::utf8_to_wide(command);
        if (application.empty()) {
            error_message = "Process command encoding is invalid.";
            return false;
        }
        command_line = quote_process_argument(application);
    }
    for (const auto& argument : arguments) {
        std::string value;
        if (argument.is_string())
            value = argument.get<std::string>();
        else if (argument.is_number() || argument.is_boolean())
            value = argument.dump();
        else {
            error_message = "Process arguments must be scalar values.";
            return false;
        }
        if (!expand_workspace_variables(adapter, &value)) {
            error_message = "Process argument contains an unsupported variable.";
            return false;
        }
        command_line.push_back(L' ');
        const std::wstring wide_value = sao::ai_editor::native::utf8_to_wide(value);
        if (!value.empty() && wide_value.empty()) {
            error_message = "Process argument encoding is invalid.";
            return false;
        }
        command_line.append(quote_process_argument(wide_value));
    }
    if (shell)
        command_line.push_back(L'\"');

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdin_read, &stdin_write, &security, 0) ||
        !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        if (stdout_read)
            CloseHandle(stdout_read);
        if (stdout_write)
            CloseHandle(stdout_write);
        if (stdin_read)
            CloseHandle(stdin_read);
        if (stdin_write)
            CloseHandle(stdin_write);
        error_message = "Process pipe creation failed.";
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_read;
    startup.StartupInfo.hStdOutput = stdout_write;
    startup.StartupInfo.hStdError = stdout_write;
    SIZE_T attribute_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<uint8_t> attribute_storage(attribute_bytes);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    HANDLE inherited_handles[] = {stdin_read, stdout_write};
    const bool attributes_initialized =
        attribute_bytes != 0 &&
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes);
    const bool handles_restricted =
        attributes_initialized &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited_handles, sizeof(inherited_handles), nullptr, nullptr);
    if (!handles_restricted) {
        if (attributes_initialized)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        error_message = "Process handle inheritance setup failed.";
        return false;
    }
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    const std::wstring cwd_native = cwd.native();
    const BOOL created = CreateProcessW(
        shell ? application.c_str() : nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
            EXTENDED_STARTUPINFO_PRESENT,
        nullptr, cwd_native.c_str(), &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(stdout_write);
    CloseHandle(stdin_read);
    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stdin_write);
        error_message = "Process creation failed.";
        return false;
    }
    HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_handle == nullptr ||
        !SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits)) ||
        !AssignProcessToJobObject(job_handle, info.hProcess) ||
        ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(info.hProcess, ERROR_CANCELLED);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        if (job_handle)
            CloseHandle(job_handle);
        CloseHandle(stdout_read);
        CloseHandle(stdin_write);
        error_message = "Process job initialization failed.";
        return false;
    }
    CloseHandle(info.hThread);
    auto process = std::make_unique<ManagedProcess>();
    process->id =
        (kind == "debug" ? "debug-" : "task-") + std::to_string(++adapter.process_counter);
    process->name = std::move(name);
    process->kind = std::move(kind);
    process->document_token = std::move(document_token);
    process->document_generation = document_generation;
    process->process = info.hProcess;
    process->job = job_handle;
    process->stdout_read = stdout_read;
    process->stdin_write = stdin_write;
    process->process_id = info.dwProcessId;
    ManagedProcess* const reader_state = process.get();
    try {
        process->output_reader = std::thread([reader_state] {
            std::array<char, 4096> buffer{};
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(reader_state->stdout_read, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &read, nullptr) ||
                    read == 0) {
                    break;
                }
                std::lock_guard lock(reader_state->output_mutex);
                reader_state->output.append(buffer.data(), read);
                if (reader_state->output.size() > kMaximumProcessOutputBytes) {
                    reader_state->output.erase(0, reader_state->output.size() -
                                                      kMaximumProcessOutputBytes);
                }
            }
            reader_state->output_reader_done.store(true, std::memory_order_release);
        });
        process->input_writer = std::thread([reader_state] {
            for (;;) {
                std::string input;
                {
                    std::unique_lock lock(reader_state->input_mutex);
                    reader_state->input_wake.wait(lock, [reader_state] {
                        return reader_state->input_stopping || !reader_state->input_queue.empty();
                    });
                    if (reader_state->input_stopping && reader_state->input_queue.empty())
                        break;
                    input = std::move(reader_state->input_queue.front());
                    reader_state->input_queue.pop_front();
                }
                DWORD written = 0;
                if (!WriteFile(reader_state->stdin_write, input.data(),
                               static_cast<DWORD>(input.size()), &written, nullptr) ||
                    written != input.size())
                    break;
            }
            reader_state->input_writer_done.store(true, std::memory_order_release);
        });
    } catch (...) {
        close_managed_process(*process, true);
        error_message = "Process I/O worker could not be started.";
        return false;
    }
    result = managed_process_snapshot(*process);
    adapter.processes[process->id] = std::move(process);
    return true;
}

void poll_managed_processes(NativeAdapter& adapter, const std::string& document_token,
                            uint64_t document_generation) {
    for (auto& [id, owned] : adapter.processes) {
        ManagedProcess& process = *owned;
        if (process.output_reader_done.load(std::memory_order_acquire) &&
            process.output_reader.joinable())
            process.output_reader.join();
        if (process.running && WaitForSingleObject(process.process, 0) == WAIT_OBJECT_0) {
            process.running = false;
            (void)GetExitCodeProcess(process.process, &process.exit_code);
            std::lock_guard lock(adapter.mutex);
            if (document_token == adapter.document_token &&
                document_generation == adapter.document_generation &&
                process.document_token == document_token &&
                process.document_generation == document_generation)
                push_event_locked(adapter, {document_token,
                                            process.kind == "debug" ? "extension_debug_session"
                                                                    : "extension_task_lifecycle",
                                            managed_process_snapshot(process)});
        }
    }
}

void close_stale_managed_processes(NativeAdapter& adapter, const std::string& document_token,
                                   uint64_t document_generation) {
    for (auto process = adapter.processes.begin(); process != adapter.processes.end();) {
        if (process->second->document_token == document_token &&
            process->second->document_generation == document_generation) {
            ++process;
            continue;
        }
        close_managed_process(*process->second, true);
        process = adapter.processes.erase(process);
    }
}

void close_managed_processes(NativeAdapter& adapter) {
    for (auto& [id, process] : adapter.processes)
        close_managed_process(*process, true);
    adapter.processes.clear();
}
#endif

RpcReply load_settings(NativeAdapter& adapter, json* metadata = nullptr) {
    RpcReply reply = call_backend(adapter, "settings.load", {{"scope", "merged"}});
    if (!reply.ok)
        return reply;
    if (!reply.result.is_object()) {
        reply.ok = false;
        reply.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
        reply.error_message = "Native settings response is not an object.";
        return reply;
    }
    if (metadata != nullptr)
        *metadata = reply.result;
    bool explicit_theme = false;
    const auto sources = reply.result.find("sources");
    if (sources != reply.result.end() && sources->is_object()) {
        for (const auto key : {"theme", "color_theme"}) {
            const auto source = sources->find(key);
            if (source != sources->end() && source->is_string() &&
                source->get_ref<const std::string&>() != "default") {
                explicit_theme = true;
            }
        }
    }
    for (const auto key :
         {"effective", "effectiveValues", "effective_values", "settings", "data"}) {
        const auto found = reply.result.find(key);
        if (found != reply.result.end() && found->is_object()) {
            reply.result = *found;
            reply.result["_sao_explicit_theme"] = explicit_theme;
            return reply;
        }
    }
    reply.result["_sao_explicit_theme"] = explicit_theme;
    return reply;
}

RpcReply save_settings(NativeAdapter& adapter, const json& patch,
                       std::string_view scope = "system") {
    return call_backend(adapter, "settings.save",
                        {{"scope", std::string(scope)}, {"changes", patch}});
}

RpcReply save_settings_delta(NativeAdapter& adapter, const json& patch, const json& reset_keys,
                             std::string_view scope) {
    return call_backend(
        adapter, "settings.save",
        {{"scope", std::string(scope)}, {"changes", patch}, {"resetKeys", reset_keys}});
}

NativeAdapterCompletion rpc_completion(const AdapterJob& job, RpcReply reply) {
    if (reply.ok)
        return successful_completion(job, std::move(reply.result));
    return failed_completion(job, std::move(reply.error_code), std::move(reply.error_message),
                             std::move(reply.error_data));
}

NativeAdapterCompletion handle_startup_method(NativeAdapter& adapter, const AdapterJob& job,
                                              bool* handled) {
    *handled = true;
    const auto require_count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
    if (job.method == "load_config") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "load_config takes no arguments.");
        if (adapter.launcher == nullptr) {
            return successful_completion(job, {{"theme", "light"},
                                               {"color_theme", ""},
                                               {"configuration_targets", json::object()},
                                               {"backend_connected", false}});
        }
        json metadata;
        RpcReply reply = load_settings(adapter, &metadata);
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        RpcReply system = call_backend(adapter, "settings.load", {{"scope", "system"}});
        RpcReply workspace = call_backend(adapter, "settings.load", {{"scope", "workspace"}});
        if (!system.ok)
            return rpc_completion(job, std::move(system));
        if (!workspace.ok)
            return rpc_completion(job, std::move(workspace));
        json targets = json::object();
        collect_target_values(settings_overrides(system), {}, "global", targets);
        collect_target_values(settings_overrides(workspace), {}, "workspace", targets);
        const auto sources = metadata.find("sources");
        for (auto item = targets.begin(); item != targets.end(); ++item) {
            json& entry = item.value();
            std::string source;
            if (sources != metadata.end()) {
                const json* source_value = dotted_value(*sources, item.key());
                if (source_value != nullptr && source_value->is_string())
                    source = source_value->get<std::string>();
            }
            const std::string target = source == "workspace" ? "workspace" : "global";
            entry["target"] = target;
            entry["source"] = source.empty() ? "default" : source;
            const json* effective = dotted_value(reply.result, item.key());
            if (effective != nullptr)
                entry["value"] = *effective;
            else if (entry["values"].contains(target))
                entry["value"] = entry["values"][target];
        }
        reply.result["configuration_targets"] = std::move(targets);
        const bool explicit_theme = reply.result.value("_sao_explicit_theme", false);
        reply.result.erase("_sao_explicit_theme");
        if (!explicit_theme)
            reply.result["theme"] = "light";
        if (!explicit_theme)
            reply.result["color_theme"] = "";
        reply.result["backend_connected"] = true;
        return successful_completion(job, std::move(reply.result));
    }
    if (job.method == "save_config") {
        if (!require_count(1, 1) || !job.args[0].is_object())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "save_config requires one object.");
        json patch = job.args[0];
        std::string scope = "system";
        const auto target = patch.find("_settings_target");
        if (target != patch.end()) {
            if (!target->is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Settings target must be a string.");
            const std::string& value = target->get_ref<const std::string&>();
            if (value == "workspace" || value == "workspaceFolder")
                scope = "workspace";
            else if (value != "global" && value != "system")
                return failed_completion(job, "SAO_INVALID_ARGUMENT", "Unknown settings target.");
        }
        json updates = json::array();
        const auto raw_updates = patch.find("_configuration_target_updates");
        if (raw_updates != patch.end()) {
            if (!raw_updates->is_array())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Configuration target updates must be an array.");
            updates = *raw_updates;
        }
        patch.erase("_settings_target");
        patch.erase("_configuration_target_updates");
        patch.erase("configuration_targets");
        json result = json::object();
        if (!updates.empty()) {
            json system_patch = json::object();
            json workspace_patch = json::object();
            json system_resets = json::array();
            json workspace_resets = json::array();
            for (const auto& update : updates) {
                if (!update.is_object() || !valid_string_member(update, "path") ||
                    !valid_string_member(update, "target")) {
                    return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                             "Configuration target update is invalid.");
                }
                const std::string path = update["path"].get<std::string>();
                const std::string target_name = update["target"].get<std::string>();
                const auto remove_value = update.find("remove");
                if (remove_value != update.end() && !remove_value->is_boolean())
                    return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                             "Configuration remove flag is invalid.");
                const bool remove = remove_value != update.end() && remove_value->get<bool>();
                json* scoped_patch = nullptr;
                json* scoped_resets = nullptr;
                if (target_name == "global" || target_name == "system") {
                    scoped_patch = &system_patch;
                    scoped_resets = &system_resets;
                } else if (target_name == "workspace" || target_name == "workspaceFolder") {
                    scoped_patch = &workspace_patch;
                    scoped_resets = &workspace_resets;
                } else {
                    return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                             "Unknown configuration target.");
                }
                if (remove) {
                    if (path.empty() || path.size() > 512)
                        return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                                 "Configuration reset path is invalid.");
                    scoped_resets->push_back(path);
                } else {
                    const auto value = update.find("value");
                    if (value == update.end() || !set_dotted_value(*scoped_patch, path, *value))
                        return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                                 "Configuration update path is invalid.");
                }
            }
            for (const auto& delta : {std::tuple<std::string_view, const json*, const json*>{
                                          "system", &system_patch, &system_resets},
                                      std::tuple<std::string_view, const json*, const json*>{
                                          "workspace", &workspace_patch, &workspace_resets}}) {
                const auto& [delta_scope, delta_patch, delta_resets] = delta;
                if (delta_patch->empty() && delta_resets->empty())
                    continue;
                if (!job_current(adapter, job))
                    return failed_completion(job, "SAO_NAVIGATION_RESET",
                                             "Workbench document changed.");
                RpcReply saved =
                    save_settings_delta(adapter, *delta_patch, *delta_resets, delta_scope);
                if (!saved.ok)
                    return rpc_completion(job, std::move(saved));
            }
            result["target_updates"] = updates;
        } else {
            RpcReply reply = save_settings(adapter, patch, scope);
            if (!reply.ok)
                return rpc_completion(job, std::move(reply));
            result = reply.result.is_object() ? std::move(reply.result) : json::object();
        }
        result["ok"] = true;
        result["applied"] = true;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "get_scopes") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "get_scopes takes no arguments.");
        RpcReply reply = call_backend(adapter, "scopes.list", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        json scopes = array_from_result(reply.result, {"scopes", "items"});
        for (auto& scope : scopes) {
            if (!scope.is_object())
                continue;
            const auto plugin_id = scope.find("pluginId");
            if (plugin_id != scope.end() && plugin_id->is_string())
                scope["plugin_id"] = *plugin_id;
        }
        return successful_completion(job, {{"scopes", std::move(scopes)}});
    }
    if (job.method == "list_tools") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "list_tools takes no arguments.");
        return rpc_completion(job, call_backend(adapter, "tools.list", json::object()));
    }
    if (job.method == "list_agents") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_agents takes no arguments.");
        RpcReply reply = call_backend(adapter, "agents.list_defs", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        return successful_completion(
            job, {{"agents", array_from_result(reply.result, {"agents", "items"})}});
    }
    if (job.method == "list_workflows") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_workflows takes no arguments.");
        RpcReply reply = call_backend(adapter, "workflows.list_defs", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        return successful_completion(
            job, {{"workflows", array_from_result(reply.result, {"workflows", "items"})}});
    }
    if (job.method == "list_chat_providers") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_chat_providers takes no arguments.");
        RpcReply reply = call_backend(adapter, "providers.list", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        return successful_completion(
            job, {{"providers", array_from_result(reply.result, {"providers", "items"})}});
    }
    if (job.method == "list_models") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_models takes no arguments.");
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        const auto custom = settings.result.find("custom_models");
        return successful_completion(
            job,
            {{"models",
              custom != settings.result.end() && custom->is_object() ? *custom : json::object()}});
    }
    if (job.method == "list_provider_models") {
        if (!require_count(0, 3) || (job.args.size() > 0 && !job.args[0].is_string()) ||
            (job.args.size() > 1 && !job.args[1].is_string()) ||
            (job.args.size() > 2 && !job.args[2].is_string())) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Invalid provider-model arguments.");
        }
        json params = json::object();
        if (!job.args.empty() && job.args[0].is_string() &&
            !job.args[0].get_ref<const std::string&>().empty()) {
            if (job.args.size() > 1 && job.args[1].is_string() &&
                !job.args[1].get_ref<const std::string&>().empty()) {
                params["provider"] = {
                    {"id", job.args[0]}, {"type", job.args[0]}, {"base_url", job.args[1]}};
            } else {
                params["provider"] = job.args[0];
            }
        }
        if (job.args.size() > 2 && job.args[2].is_string() &&
            !job.args[2].get_ref<const std::string&>().empty()) {
            params["apiKey"] = job.args[2];
        }
        RpcReply reply = call_backend(adapter, "models.list", params);
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        return successful_completion(
            job, {{"models", array_from_result(reply.result, {"models", "items"})}});
    }
    if (job.method == "get_mode" || job.method == "get_active_agent") {
        if (!require_count(0, 1) || (!job.args.empty() && !job.args[0].is_string()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Invalid control query arguments.");
        if (job.method == "get_active_agent")
            return successful_completion(job, {{"agent_id", adapter.active_agent}});
        RpcReply reply = load_settings(adapter);
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        const std::string mode = !job.args.empty() && job.args[0].is_string()
                                     ? job.args[0].get<std::string>()
                                     : string_member_or(reply.result, "mode", "agent");
        RpcReply policy = call_backend(adapter, "permission.get", {{"mode", mode}});
        RpcReply tools = call_backend(adapter, "tools.list", {{"mode", mode}});
        json permission_map = json::object();
        json defaults = json::object();
        json tool_names = json::array();
        if (tools.ok) {
            for (const auto& tool : array_from_result(tools.result, {"tools", "items"})) {
                if (!tool.is_object())
                    continue;
                const std::string name = string_member_or(tool, "name");
                if (name.empty())
                    continue;
                const std::string permission = string_member_or(tool, "permission", "confirm");
                permission_map[name] = permission;
                defaults[name] = permission;
                tool_names.push_back(name);
            }
        }
        const auto configured = reply.result.find("permissions");
        json overrides = configured != reply.result.end() && configured->is_object()
                             ? *configured
                             : json::object();
        return successful_completion(job, {{"mode", mode},
                                           {"permissions", std::move(permission_map)},
                                           {"defaults", std::move(defaults)},
                                           {"overrides", std::move(overrides)},
                                           {"tools", std::move(tool_names)},
                                           {"policy", policy.ok ? policy.result : json::object()}});
    }
    if (job.method == "get_chat_controls") {
        if (!require_count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "get_chat_controls takes no arguments.");
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        RpcReply providers = call_backend(adapter, "providers.list", json::object());
        RpcReply agents = call_backend(adapter, "agents.list_defs", json::object());
        RpcReply workflows = call_backend(adapter, "workflows.list_defs", json::object());
        json result{
            {"ok", true},
            {"mode", string_member_or(settings.result, "mode", "agent")},
            {"provider", string_member_or(settings.result, "provider", "openai")},
            {"model", string_member_or(settings.result, "model")},
            {"agent_id", adapter.active_agent},
            {"providers", providers.ok ? array_from_result(providers.result, {"providers", "items"})
                                       : json::array()},
            {"agents",
             agents.ok ? array_from_result(agents.result, {"agents", "items"}) : json::array()},
            {"workflows", workflows.ok ? array_from_result(workflows.result, {"workflows", "items"})
                                       : json::array()}};
        return successful_completion(job, std::move(result));
    }
    if (job.method == "set_active_agent" || job.method == "clear_active_agent") {
        if ((job.method == "set_active_agent" &&
             (!require_count(1, 1) || !job.args[0].is_string())) ||
            (job.method == "clear_active_agent" && !require_count(0, 0))) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Invalid active-agent arguments.");
        }
        if (job.method == "set_active_agent") {
            const std::string candidate = job.args[0].get<std::string>();
            RpcReply validation = call_backend(adapter, "agents.get_def", {{"id", candidate}});
            if (!validation.ok)
                return rpc_completion(job, std::move(validation));
            adapter.active_agent = candidate;
        } else {
            adapter.active_agent.clear();
        }
        return successful_completion(
            job, {{"ok", true}, {"applied", true}, {"agent_id", adapter.active_agent}});
    }
    if (job.method == "set_tool_permission") {
        if (!require_count(2, 2) || !job.args[0].is_string() || !job.args[1].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Tool permission arguments are invalid.");
        const std::string tool = job.args[0].get<std::string>();
        const std::string permission = job.args[1].get<std::string>();
        if (tool.empty() || tool.size() > 128 ||
            (permission != "default" && permission != "allowed" && permission != "confirm" &&
             permission != "disabled")) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Tool permission value is invalid.");
        }
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        const auto stored_permissions = settings.result.find("permissions");
        json permissions =
            stored_permissions != settings.result.end() && stored_permissions->is_object()
                ? *stored_permissions
                : json::object();
        if (permission == "default")
            permissions.erase(tool);
        else
            permissions[tool] = permission;
        RpcReply saved = save_settings(adapter, {{"permissions", permissions}});
        if (!saved.ok)
            return rpc_completion(job, std::move(saved));
        return successful_completion(
            job, {{"ok", true}, {"applied", true}, {"tool", tool}, {"permission", permission}});
    }
    if (job.method == "set_mode" || job.method == "set_active_mode" ||
        job.method == "set_active_provider" || job.method == "set_active_model" ||
        job.method == "set_provider_model" || job.method == "set_chat_controls") {
        json patch = json::object();
        std::optional<std::string> agent_update;
        if (job.method == "set_chat_controls") {
            if (!require_count(1, 1) || !job.args[0].is_object())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "set_chat_controls requires one object.");
            const json& controls = job.args[0];
            for (const auto key : {"mode", "provider", "model", "approval"}) {
                const auto found = controls.find(key);
                if (found != controls.end())
                    patch[key] = *found;
            }
            const auto agent = controls.find("agent_id");
            if (agent != controls.end()) {
                if (!agent->is_string())
                    return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                             "Agent id must be a string.");
                agent_update = agent->get<std::string>();
                if (!agent_update->empty()) {
                    RpcReply validation =
                        call_backend(adapter, "agents.get_def", {{"id", *agent_update}});
                    if (!validation.ok)
                        return rpc_completion(job, std::move(validation));
                }
            }
        } else if (job.method == "set_mode" || job.method == "set_active_mode") {
            if (!require_count(1, 1) || !job.args[0].is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT", "Mode must be a string.");
            patch["mode"] = job.args[0];
        } else if (job.method == "set_active_model") {
            if (!require_count(1, 1) || !job.args[0].is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT", "Model must be a string.");
            patch["model"] = job.args[0];
        } else {
            if (!require_count(1, 2) || !job.args[0].is_string() ||
                (job.args.size() > 1 && !job.args[1].is_string())) {
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Provider controls are invalid.");
            }
            patch["provider"] = job.args[0];
            if (job.args.size() > 1)
                patch["model"] = job.args[1];
        }
        if (!patch.empty()) {
            RpcReply reply = save_settings(adapter, patch);
            if (!reply.ok)
                return rpc_completion(job, std::move(reply));
        }
        if (agent_update)
            adapter.active_agent = std::move(*agent_update);
        json result = patch;
        result["ok"] = true;
        result["applied"] = true;
        result["agent_id"] = adapter.active_agent;
        return successful_completion(job, std::move(result));
    }
    *handled = false;
    return {};
}

std::string result_identifier(const json& result, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        const std::string value = string_member_or(result, name);
        if (!value.empty())
            return value;
    }
    return {};
}

bool bind_run(NativeAdapter& adapter, const AdapterJob& job, const std::string& run_id,
              std::string provider_id) {
    std::lock_guard lock(adapter.mutex);
    if (adapter.stopping || job.document_token != adapter.document_token ||
        job.document_generation != adapter.document_generation) {
        return false;
    }
    adapter.runs[run_id] = {job.document_token, std::move(provider_id), job.document_generation,
                            ++adapter.run_sequence};
    adapter.active_run_id = run_id;
    request_event_poll_locked(adapter, kEventPollLifetime);
    return true;
}

RpcReply ensure_conversation(NativeAdapter& adapter, const AdapterJob& job, std::string_view model,
                             std::string_view provider) {
    std::string& active_id = provider.empty()
                                 ? adapter.active_conversation_id
                                 : adapter.provider_conversations[std::string(provider)];
    if (!active_id.empty()) {
        RpcReply existing;
        existing.ok = true;
        existing.result = {{"id", active_id}};
        return existing;
    }
    RpcReply created = call_backend(
        adapter, "conversation.create",
        {{"title", "New Chat"}, {"model", std::string(model)}, {"scope", "workspace"}});
    if (!created.ok)
        return created;
    const std::string id = result_identifier(created.result, {"id", "conversationId"});
    if (id.empty() || !job_current(adapter, job)) {
        created.ok = false;
        created.error_code = "SAO_BACKEND_PROTOCOL_ERROR";
        created.error_message = "Native conversation creation returned no current id.";
        return created;
    }
    active_id = id;
    created.result["id"] = id;
    return created;
}

NativeAdapterCompletion start_chat_request(NativeAdapter& adapter, const AdapterJob& job,
                                           std::string text, json controls, json content,
                                           std::string provider_tab) {
    if (text.size() > kMaximumRequestBytes)
        return failed_completion(job, "SAO_REQUEST_TOO_LARGE", "Chat input exceeds 4 MiB.");
    const std::string model = string_member_or(controls, "model");
    RpcReply conversation = ensure_conversation(adapter, job, model, provider_tab);
    if (!conversation.ok)
        return rpc_completion(job, std::move(conversation));
    const std::string conversation_id =
        result_identifier(conversation.result, {"id", "conversationId"});
    if (conversation_id.empty())
        return failed_completion(job, "SAO_BACKEND_PROTOCOL_ERROR",
                                 "Conversation id is unavailable.");
    json message{{"role", "user"}, {"content", std::move(content)}};
    RpcReply appended = call_backend(adapter, "conversation.append",
                                     {{"id", conversation_id}, {"message", std::move(message)}});
    if (!appended.ok)
        return rpc_completion(job, std::move(appended));
    if (!job_current(adapter, job))
        return failed_completion(job, "SAO_NAVIGATION_RESET", "Workbench document changed.");
    json params{{"conversationId", conversation_id}, {"stream", true}};
    for (const auto key : {"provider", "model", "mode", "approval"}) {
        const auto value = controls.find(key);
        if (value != controls.end())
            params[key] = *value;
    }
    RpcReply started = call_backend(adapter, "chat.run", params);
    if (!started.ok)
        return rpc_completion(job, std::move(started));
    const std::string run_id = result_identifier(started.result, {"runId", "run_id", "id"});
    if (run_id.empty())
        return failed_completion(job, "SAO_BACKEND_PROTOCOL_ERROR", "Chat run returned no run id.");
    if (!bind_run(adapter, job, run_id, std::move(provider_tab))) {
        (void)call_backend(adapter, "run.cancel", {{"runId", run_id}});
        return failed_completion(job, "SAO_NAVIGATION_RESET",
                                 "Chat run is not bound to the current document.");
    }
    json result = started.result.is_object() ? std::move(started.result) : json::object();
    result["ok"] = true;
    result["run_id"] = run_id;
    result["conversation_id"] = conversation_id;
    return successful_completion(job, std::move(result));
}

NativeAdapterCompletion handle_chat_method(NativeAdapter& adapter, const AdapterJob& job,
                                           bool* handled) {
    *handled = true;
    const auto count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
    if (job.method == "send_message") {
        if (!count(1, 3) || !job.args[0].is_string() ||
            (job.args.size() > 1 && !job.args[1].is_object()) ||
            (job.args.size() > 2 && !job.args[2].is_boolean())) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "send_message arguments are invalid.");
        }
        json controls = job.args.size() > 1 ? job.args[1] : json::object();
        const std::string text = job.args[0].get<std::string>();
        return start_chat_request(adapter, job, text, std::move(controls), text, {});
    }
    if (job.method == "send_image_message") {
        if (!count(2, 3) || !job.args[0].is_string() || !job.args[1].is_string() ||
            (job.args.size() > 2 && !job.args[2].is_string())) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "send_image_message arguments are invalid.");
        }
        const std::string text = job.args[0].get<std::string>();
        const std::string image = job.args[1].get<std::string>();
        const std::string mime = job.args.size() > 2 ? job.args[2].get<std::string>() : "image/png";
        if (image.size() > kMaximumRequestBytes || mime.size() > 128)
            return failed_completion(job, "SAO_REQUEST_TOO_LARGE",
                                     "Image message exceeds the native limit.");
        json content =
            json::array({{{"type", "text"}, {"text", text}},
                         {{"type", "image_url"},
                          {"image_url", {{"url", "data:" + mime + ";base64," + image}}}}});
        return start_chat_request(adapter, job, text, json::object(), std::move(content), {});
    }
    if (job.method == "new_chat" || job.method == "provider_new_chat") {
        if ((job.method == "new_chat" && !count(0, 0)) ||
            (job.method == "provider_new_chat" && (!count(1, 1) || !job.args[0].is_string()))) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "New-chat arguments are invalid.");
        }
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        RpcReply created = call_backend(adapter, "conversation.create",
                                        {{"title", "New Chat"},
                                         {"model", string_member_or(settings.result, "model")},
                                         {"scope", "workspace"}});
        if (!created.ok)
            return rpc_completion(job, std::move(created));
        const std::string id = result_identifier(created.result, {"id", "conversationId"});
        if (id.empty() || !job_current(adapter, job))
            return failed_completion(job, "SAO_NAVIGATION_RESET", "New conversation is stale.");
        const std::string provider =
            job.method == "provider_new_chat" ? job.args[0].get<std::string>() : std::string{};
        if (provider.empty())
            adapter.active_conversation_id = id;
        else
            adapter.provider_conversations[provider] = id;
        json result = created.result;
        result["ok"] = true;
        result["id"] = id;
        if (!provider.empty())
            result["provider"] = provider;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "cancel" || job.method == "provider_cancel") {
        if ((job.method == "cancel" && !count(0, 0)) ||
            (job.method == "provider_cancel" && (!count(1, 1) || !job.args[0].is_string()))) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Cancel arguments are invalid.");
        }
        std::string run_id;
        {
            std::lock_guard lock(adapter.mutex);
            if (job.method == "provider_cancel") {
                const std::string provider = job.args[0].get<std::string>();
                uint64_t newest = 0;
                for (const auto& [candidate, binding] : adapter.runs) {
                    if (binding.provider_id == provider && binding.sequence >= newest) {
                        newest = binding.sequence;
                        run_id = candidate;
                    }
                }
            } else {
                uint64_t newest = 0;
                for (const auto& [candidate, binding] : adapter.runs) {
                    if (binding.provider_id.empty() && binding.sequence >= newest) {
                        newest = binding.sequence;
                        run_id = candidate;
                    }
                }
                if (run_id.empty())
                    run_id = adapter.active_run_id;
            }
        }
        if (run_id.empty())
            return successful_completion(job, {{"ok", true}, {"cancelled", false}});
        RpcReply cancelled = call_backend(adapter, "run.cancel", {{"runId", run_id}});
        if (!cancelled.ok)
            return rpc_completion(job, std::move(cancelled));
        json result = cancelled.result.is_object() ? std::move(cancelled.result) : json::object();
        result["ok"] = true;
        result["cancelled"] = true;
        result["run_id"] = run_id;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "list_history") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_history takes no arguments.");
        RpcReply listed =
            call_backend(adapter, "conversation.list", {{"scope", "all"}, {"limit", 500}});
        if (!listed.ok)
            return rpc_completion(job, std::move(listed));
        json entries = array_from_result(listed.result, {"entries", "conversations", "items"});
        for (auto& entry : entries) {
            if (!entry.is_object())
                continue;
            if (entry.contains("savedAt"))
                entry["saved_at"] = entry["savedAt"];
            if (entry.contains("messageCount"))
                entry["message_count"] = entry["messageCount"];
        }
        return successful_completion(job, {{"entries", std::move(entries)}});
    }
    if (job.method == "load_history" || job.method == "delete_history") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "History id is required.");
        const std::string id = job.args[0].get<std::string>();
        RpcReply reply = call_backend(
            adapter, job.method == "load_history" ? "conversation.get" : "conversation.delete",
            {{"id", id}});
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        if (job.method == "load_history" && job_current(adapter, job))
            adapter.active_conversation_id = id;
        json result = reply.result.is_object() ? std::move(reply.result) : json::object();
        result["ok"] = true;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "export_chat") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "export_chat takes no arguments.");
        if (adapter.active_conversation_id.empty())
            return successful_completion(job, json::object());
        return rpc_completion(job, call_backend(adapter, "conversation.export",
                                                {{"id", adapter.active_conversation_id}}));
    }
    if (job.method == "count_conversation_tokens") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "count_conversation_tokens takes no arguments.");
        if (adapter.active_conversation_id.empty())
            return successful_completion(job, {{"tokens", 0}});
        RpcReply conversation =
            call_backend(adapter, "conversation.get", {{"id", adapter.active_conversation_id}});
        if (!conversation.ok)
            return rpc_completion(job, std::move(conversation));
        const auto messages = conversation.result.find("messages");
        const size_t bytes = messages != conversation.result.end() ? messages->dump().size() : 0;
        return successful_completion(job, {{"tokens", (bytes + 3) / 4}});
    }
    if (job.method == "execute_tool") {
        if (!count(1, 3) || !job.args[0].is_string() ||
            (job.args.size() > 1 && !job.args[1].is_string() && !job.args[1].is_object()) ||
            (job.args.size() > 2 && !job.args[2].is_boolean())) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "execute_tool arguments are invalid.");
        }
        json arguments = json::object();
        if (job.args.size() > 1 && job.args[1].is_object())
            arguments = job.args[1];
        else if (job.args.size() > 1 && job.args[1].is_string()) {
            const std::string& raw_arguments = job.args[1].get_ref<const std::string&>();
            arguments = json::parse(raw_arguments, nullptr, false);
            if (!arguments.is_object() || raw_arguments.size() > kMaximumRequestBytes ||
                !json_within_budget(arguments))
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Tool arguments are outside the structural budget.");
        }
        json params{{"name", job.args[0]}, {"arguments", std::move(arguments)}};
        if (job.args.size() > 2 && job.args[2].get<bool>())
            params["approval"] = "allow";
        return rpc_completion(job, call_backend(adapter, "tools.call", params));
    }
    if (job.method == "switch_provider") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Provider id is required.");
        RpcReply saved = save_settings(adapter, {{"active_chat_provider", job.args[0]}});
        if (!saved.ok)
            return rpc_completion(job, std::move(saved));
        return successful_completion(
            job, {{"ok", true}, {"available", true}, {"provider", job.args[0]}});
    }
    if (job.method == "provider_send") {
        if (!count(2, 3) || !job.args[0].is_string() || !job.args[1].is_string() ||
            (job.args.size() > 2 && !job.args[2].is_object())) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "provider_send arguments are invalid.");
        }
        const std::string provider = job.args[0].get<std::string>();
        json controls = job.args.size() > 2 ? job.args[2] : json::object();
        controls["provider"] = provider;
        const std::string text = job.args[1].get<std::string>();
        return start_chat_request(adapter, job, text, std::move(controls), text, provider);
    }
    if (job.method == "get_model_info" || job.method == "fetch_model_context") {
        if (!count(1, 3) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Model name is required.");
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        const auto models = settings.result.find("custom_models");
        const std::string name = job.args[0].get<std::string>();
        if (models != settings.result.end() && models->is_object() && models->contains(name)) {
            json result = (*models)[name];
            if (!result.is_object())
                result = json::object();
            result["name"] = name;
            return successful_completion(job, std::move(result));
        }
        return successful_completion(job, {{"name", name}, {"input", 0}, {"output", 0}});
    }
    if (job.method == "save_custom_model") {
        if (!count(6, 9) || !job.args[0].is_string() || !job.args[1].is_number_integer() ||
            !job.args[2].is_number_integer() || !job.args[3].is_boolean() ||
            !job.args[4].is_boolean() || !job.args[5].is_boolean() ||
            (job.args.size() > 6 && !job.args[6].is_boolean()) ||
            (job.args.size() > 7 && !job.args[7].is_string()) ||
            (job.args.size() > 8 && !job.args[8].is_string())) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Custom model arguments are invalid.");
        }
        const std::string model_name = job.args[0].get<std::string>();
        const int64_t input_tokens = job.args[1].get<int64_t>();
        const int64_t output_tokens = job.args[2].get<int64_t>();
        if (model_name.empty() || model_name.size() > 128 || input_tokens < 0 ||
            output_tokens < 0 || input_tokens > 1000000000 || output_tokens > 1000000000) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Custom model limits are invalid.");
        }
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        json models = settings.result.contains("custom_models") &&
                              settings.result["custom_models"].is_object()
                          ? settings.result["custom_models"]
                          : json::object();
        json model{{"input", job.args[1]},
                   {"output", job.args[2]},
                   {"tools", job.args[3]},
                   {"vision", job.args[4]},
                   {"thinking", job.args[5]}};
        if (job.args.size() > 6)
            model["streaming"] = job.args[6];
        if (job.args.size() > 7)
            model["provider"] = job.args[7];
        if (job.args.size() > 8)
            model["base_url"] = job.args[8];
        models[model_name] = std::move(model);
        RpcReply saved = save_settings(adapter, {{"custom_models", models}});
        return saved.ok ? successful_completion(job, {{"ok", true}})
                        : rpc_completion(job, std::move(saved));
    }
    if (job.method == "delete_custom_model") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Custom model name is required.");
        RpcReply settings = load_settings(adapter);
        if (!settings.ok)
            return rpc_completion(job, std::move(settings));
        json models = settings.result.contains("custom_models") &&
                              settings.result["custom_models"].is_object()
                          ? settings.result["custom_models"]
                          : json::object();
        models.erase(job.args[0].get<std::string>());
        RpcReply saved = save_settings(adapter, {{"custom_models", models}});
        return saved.ok ? successful_completion(job, {{"ok", true}})
                        : rpc_completion(job, std::move(saved));
    }
    *handled = false;
    return {};
}

NativeAdapterCompletion handle_agent_workflow_method(NativeAdapter& adapter, const AdapterJob& job,
                                                     bool* handled) {
    *handled = true;
    const auto count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
    if (job.method == "save_agent" || job.method == "save_workflow") {
        if (!count(1, 1) || !job.args[0].is_object())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Definition object is required.");
        json definition = job.args[0];
        std::string scope = "workspace";
        const auto scope_value = definition.find("_scope");
        if (scope_value != definition.end()) {
            if (!scope_value->is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Definition scope is invalid.");
            scope = scope_value->get<std::string>();
            definition.erase(scope_value);
        }
        RpcReply saved = call_backend(
            adapter, job.method == "save_agent" ? "agents.save_def" : "workflows.save_def",
            job.method == "save_agent" ? json{{"agent", definition}, {"scope", scope}}
                                       : json{{"workflow", definition}, {"scope", scope}});
        if (!saved.ok)
            return rpc_completion(job, std::move(saved));
        json result = saved.result.is_object() ? std::move(saved.result) : json::object();
        result["ok"] = true;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "delete_agent" || job.method == "delete_workflow") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Definition id is required.");
        return rpc_completion(job,
                              call_backend(adapter,
                                           job.method == "delete_agent" ? "agents.delete_def"
                                                                        : "workflows.delete_def",
                                           {{"id", job.args[0]}, {"scope", "workspace"}}));
    }
    if (job.method == "run_workflow" || job.method == "retry_workflow_step") {
        const bool retry = job.method == "retry_workflow_step";
        if ((!retry && (!count(2, 4) || !job.args[0].is_string() || !job.args[1].is_string() ||
                        (job.args.size() > 2 && !job.args[2].is_string()) ||
                        (job.args.size() > 3 && !job.args[3].is_object()))) ||
            (retry && (!count(4, 6) || !job.args[0].is_string() || !job.args[1].is_string() ||
                       !job.args[2].is_number_integer() || !job.args[3].is_object() ||
                       (job.args.size() > 4 && !job.args[4].is_string()) ||
                       (job.args.size() > 5 && !job.args[5].is_object())))) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workflow run arguments are invalid.");
        }
        const std::string workflow_id = job.args[0].get<std::string>();
        const std::string frontend_run_id =
            retry ? (job.args.size() > 4 ? job.args[4].get<std::string>() : std::string{})
                  : (job.args.size() > 2 ? job.args[2].get<std::string>() : std::string{});
        RpcReply started;
        if (retry) {
            std::string original_execution;
            {
                std::lock_guard lock(adapter.mutex);
                const auto found = adapter.workflow_execution_by_frontend.find(frontend_run_id);
                if (found != adapter.workflow_execution_by_frontend.end() &&
                    !found->second.active && found->second.workflow_id == workflow_id &&
                    found->second.document_token == job.document_token &&
                    found->second.document_generation == job.document_generation) {
                    original_execution = found->second.execution_id;
                }
            }
            if (original_execution.empty())
                return failed_completion(job, "SAO_WORKFLOW_NOT_FOUND",
                                         "Previous workflow execution is unavailable.");
            started = call_backend(adapter, "workflow.retry",
                                   {{"id", original_execution}, {"fromStep", job.args[2]}});
        } else {
            json params{{"id", workflow_id}, {"input", {{"input", job.args[1]}}}};
            const json metadata = job.args.size() > 3 ? job.args[3] : json::object();
            for (const auto key : {"provider", "model", "mode", "approval"}) {
                const auto value = metadata.find(key);
                if (value != metadata.end())
                    params[key] = *value;
            }
            started = call_backend(adapter, "workflows.run", params);
        }
        if (!started.ok)
            return rpc_completion(job, std::move(started));
        const std::string execution_id =
            result_identifier(started.result, {"executionId", "execution_id", "id"});
        if (execution_id.empty())
            return failed_completion(job, "SAO_BACKEND_PROTOCOL_ERROR",
                                     "Workflow run returned no execution id.");
        const std::string public_id = frontend_run_id.empty() ? execution_id : frontend_run_id;
        bool stale = false;
        {
            std::lock_guard lock(adapter.mutex);
            if (adapter.stopping || job.document_token != adapter.document_token ||
                job.document_generation != adapter.document_generation) {
                stale = true;
            } else {
                adapter.workflows[execution_id] = {job, execution_id, public_id, workflow_id,
                                                   json::object()};
                remember_workflow_binding_locked(
                    adapter, public_id,
                    {execution_id, workflow_id, job.document_token, job.document_generation, true});
                request_event_poll_locked(adapter, kEventPollLifetime);
            }
        }
        if (stale) {
            (void)call_backend(adapter, "workflows.cancel", {{"executionId", execution_id}});
            return failed_completion(job, "SAO_NAVIGATION_RESET", "Workflow document changed.");
        }
        return {};
    }
    if (job.method == "cancel_workflow" || job.method == "pause_workflow" ||
        job.method == "resume_workflow" || job.method == "confirm_workflow_step") {
        const bool confirm = job.method == "confirm_workflow_step";
        if ((!confirm && (!count(0, 1) || (!job.args.empty() && !job.args[0].is_string()))) ||
            (confirm && (!count(3, 3) || !job.args[0].is_string() ||
                         !job.args[1].is_number_integer() || !job.args[2].is_boolean()))) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workflow control arguments are invalid.");
        }
        const std::string frontend_id =
            job.args.empty() ? std::string{} : job.args[0].get<std::string>();
        std::vector<std::string> executions;
        {
            std::lock_guard lock(adapter.mutex);
            if (frontend_id.empty()) {
                for (const auto& [execution_id, pending] : adapter.workflows)
                    executions.push_back(execution_id);
            } else {
                const auto found = adapter.workflow_execution_by_frontend.find(frontend_id);
                if (found != adapter.workflow_execution_by_frontend.end() && found->second.active &&
                    found->second.document_token == job.document_token &&
                    found->second.document_generation == job.document_generation) {
                    executions.push_back(found->second.execution_id);
                }
            }
        }
        if (executions.empty())
            return successful_completion(job, {{"ok", true}, {"count", 0}});
        const std::string backend_method = job.method == "cancel_workflow"   ? "workflows.cancel"
                                           : job.method == "pause_workflow"  ? "workflows.pause"
                                           : job.method == "resume_workflow" ? "workflows.resume"
                                                                             : "workflows.confirm";
        for (const auto& execution_id : executions) {
            json params{{"executionId", execution_id}};
            if (confirm) {
                params["stepId"] = std::to_string(job.args[1].get<int64_t>());
                params["approved"] = job.args[2];
            }
            RpcReply controlled = call_backend(adapter, backend_method, params);
            if (!controlled.ok)
                return rpc_completion(job, std::move(controlled));
        }
        return successful_completion(
            job, {{"ok", true}, {"workflowRunId", frontend_id}, {"count", executions.size()}});
    }
    *handled = false;
    return {};
}

NativeAdapterCompletion handle_workspace_method(NativeAdapter& adapter, const AdapterJob& job,
                                                bool* handled) {
    *handled = true;
    const auto count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
    if (job.method == "list_workspace_tree") {
        if (!count(0, 1) || (!job.args.empty() && !job.args[0].is_string()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace tree path is invalid.");
        const std::string relative =
            job.args.empty() ? std::string{"."} : job.args[0].get<std::string>();
        std::filesystem::path directory;
        if (!resolve_workspace_path(adapter, relative.empty() ? "." : relative, false, directory))
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                     "Workspace path is outside the active root.");
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error)
            return failed_completion(job, "SAO_WORKSPACE_NOT_FOUND",
                                     "Workspace directory was not found.");
        json entries = json::array();
        for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
            if (error || entries.size() >= 500)
                break;
            const bool is_directory = item.is_directory(error);
            if (error)
                break;
            entries.push_back(
                {{"name", sao::ai_editor::native::wide_to_utf8(item.path().filename().native())},
                 {"path", relative_workspace_path(adapter, item.path())},
                 {"type", is_directory ? "directory" : "file"},
                 {"size", is_directory ? uintmax_t{0} : item.file_size(error)}});
            error.clear();
        }
        if (error)
            return failed_completion(job, "SAO_WORKSPACE_IO_FAILED",
                                     "Workspace directory enumeration failed.");
        std::sort(entries.begin(), entries.end(), [](const json& left, const json& right) {
            const bool left_directory = string_member_or(left, "type") == "directory";
            const bool right_directory = string_member_or(right, "type") == "directory";
            if (left_directory != right_directory)
                return left_directory;
            return string_member_or(left, "name") < string_member_or(right, "name");
        });
        return successful_completion(
            job, {{"root", sao::ai_editor::native::wide_to_utf8(adapter.workspace_root.native())},
                  {"root_name", sao::ai_editor::native::wide_to_utf8(
                                    adapter.workspace_root.filename().native())},
                  {"path", relative == "." ? "" : relative},
                  {"entries", std::move(entries)}});
    }
    if (job.method == "list_workspace_files") {
        if (!count(0, 2) || (!job.args.empty() && !job.args[0].is_string()) ||
            (job.args.size() > 1 && !job.args[1].is_number_integer()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace file query is invalid.");
        RpcReply workspace = ensure_workspace(adapter);
        if (!workspace.ok)
            return rpc_completion(job, std::move(workspace));
        std::string query = job.args.empty() ? std::string{} : job.args[0].get<std::string>();
        std::transform(query.begin(), query.end(), query.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const size_t requested =
            job.args.size() > 1
                ? static_cast<size_t>(std::max<int64_t>(1, job.args[1].get<int64_t>()))
                : 200;
        const size_t limit = std::min<size_t>(requested, 500);
        json files = json::array();
        bool truncated = false;
        std::error_code error;
        for (const auto& item : std::filesystem::recursive_directory_iterator(
                 adapter.workspace_root, std::filesystem::directory_options::skip_permission_denied,
                 error)) {
            if (error)
                break;
            if (!item.is_regular_file(error)) {
                error.clear();
                continue;
            }
            const std::string path = relative_workspace_path(adapter, item.path());
            std::string comparable = path;
            std::transform(
                comparable.begin(), comparable.end(), comparable.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (!query.empty() && comparable.find(query) == std::string::npos)
                continue;
            if (files.size() >= limit) {
                truncated = true;
                break;
            }
            files.push_back(
                {{"path", path},
                 {"name", sao::ai_editor::native::wide_to_utf8(item.path().filename().native())},
                 {"language", file_language(path)}});
        }
        return successful_completion(job, {{"files", std::move(files)}, {"truncated", truncated}});
    }
    if (job.method == "open_workspace_file" || job.method == "open_text_resource" ||
        job.method == "resolve_chat_resource") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Resource path is required.");
        std::string requested = job.args[0].get<std::string>();
        if (requested.rfind("untitled:", 0) == 0)
            return successful_completion(job, {{"uri", requested},
                                               {"name", requested},
                                               {"content", ""},
                                               {"language", "plaintext"}});
        requested = workspace_resource_path(std::move(requested));
        std::filesystem::path path;
        if (!resolve_workspace_path(adapter, requested, false, path))
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                     "Resource is outside the active workspace.");
        std::string content;
        const int32_t status = sao::ai_editor::native::read_text_file(
            path, static_cast<uint32_t>(kMaximumResponseBytes), content);
        if (status != SAO_AI_EDITOR_OK)
            return failed_completion(job, transport_error_code(status),
                                     "Workspace file read failed.");
        const std::string relative = relative_workspace_path(adapter, path);
        return successful_completion(
            job, {{"ok", true},
                  {"path", relative},
                  {"uri", workspace_file_uri(path)},
                  {"name", sao::ai_editor::native::wide_to_utf8(path.filename().native())},
                  {"content", std::move(content)},
                  {"language", file_language(relative)}});
    }
    if (job.method == "create_workspace_file" || job.method == "create_workspace_directory") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Workspace path is required.");
        std::filesystem::path path;
        if (!resolve_workspace_path(adapter, job.args[0].get_ref<const std::string&>(), true, path))
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                     "Workspace path is outside the active root.");
        std::error_code error;
        if (job.method == "create_workspace_directory") {
            if (!std::filesystem::create_directories(path, error) &&
                !std::filesystem::is_directory(path, error))
                return failed_completion(job, "SAO_WORKSPACE_IO_FAILED",
                                         "Workspace directory creation failed.");
        } else {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error || std::filesystem::exists(path, error))
                return failed_completion(job, "SAO_WORKSPACE_ALREADY_EXISTS",
                                         "Workspace file already exists.");
            const int32_t status = sao::ai_editor::native::write_text_atomic(path, {});
            if (status != SAO_AI_EDITOR_OK)
                return failed_completion(job, transport_error_code(status),
                                         "Workspace file creation failed.");
        }
        return successful_completion(
            job, {{"ok", true}, {"path", relative_workspace_path(adapter, path)}});
    }
    if (job.method == "rename_workspace_entry") {
        if (!count(2, 2) || !job.args[0].is_string() || !job.args[1].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace rename arguments are invalid.");
        std::filesystem::path source;
        if (!resolve_workspace_path(adapter, job.args[0].get_ref<const std::string&>(), false,
                                    source))
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY", "Workspace source is invalid.");
        const std::string name = job.args[1].get<std::string>();
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\") != std::string::npos)
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace entry name is invalid.");
        std::filesystem::path target;
        const std::string parent = relative_workspace_path(adapter, source.parent_path());
        const std::string target_text = parent.empty() ? name : parent + "\\" + name;
        if (!resolve_workspace_path(adapter, target_text, true, target))
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY", "Workspace target is invalid.");
        std::error_code error;
        std::filesystem::rename(source, target, error);
        if (error)
            return failed_completion(job, "SAO_WORKSPACE_IO_FAILED", "Workspace rename failed.");
        return successful_completion(job, {{"ok", true},
                                           {"oldPath", relative_workspace_path(adapter, source)},
                                           {"path", relative_workspace_path(adapter, target)}});
    }
    if (job.method == "delete_workspace_entry") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace delete path is invalid.");
        std::filesystem::path path;
        if (!resolve_workspace_path(adapter, job.args[0].get_ref<const std::string&>(), false,
                                    path) ||
            path == adapter.workspace_root)
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                     "Workspace root deletion is rejected.");
        std::error_code error;
        const uintmax_t removed = std::filesystem::remove_all(path, error);
        if (error || removed == 0)
            return failed_completion(job, "SAO_WORKSPACE_IO_FAILED", "Workspace delete failed.");
        return successful_completion(job, {{"ok", true}, {"removed", removed}});
    }
    if (job.method == "search_workspace_text") {
        if (!count(1, 2) || !job.args[0].is_string() ||
            (job.args.size() > 1 && !job.args[1].is_object()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace search query is invalid.");
        const std::string query = job.args[0].get<std::string>();
        const json options = job.args.size() > 1 ? job.args[1] : json::object();
        const bool use_regex = options.value("regex", false);
        const bool case_sensitive = options.value("caseSensitive", false);
        const bool whole_word = options.value("wholeWord", false);
        const int64_t requested_limit = options.value("maxResults", int64_t{500});
        if (query.empty() || query.size() > 4096 || requested_limit < 1 || requested_limit > 2000)
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace search options are invalid.");
        const uint32_t limit = static_cast<uint32_t>(requested_limit);
        json params{{"query", query},
                    {"path", "."},
                    {"limit", limit},
                    {"regex", use_regex},
                    {"caseSensitive", case_sensitive}};
        const auto pattern = options.find("pattern");
        if (pattern != options.end() && pattern->is_string())
            params["pattern"] = *pattern;
        RpcReply reply = call_backend(adapter, "vscode.workspace.textSearch", params);
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        std::string expression_text =
            use_regex ? query
                      : std::regex_replace(query, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
        if (whole_word)
            expression_text = "\\b(?:" + expression_text + ")\\b";
        std::regex expression;
        try {
            expression =
                std::regex(expression_text, case_sensitive ? std::regex_constants::ECMAScript
                                                           : std::regex_constants::ECMAScript |
                                                                 std::regex_constants::icase);
        } catch (const std::regex_error&) {
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace search expression is invalid.");
        }
        json normalized = json::array();
        std::unordered_set<std::string> files;
        for (const auto& row : array_from_result(reply.result, {"results", "items"})) {
            if (!row.is_object())
                continue;
            const std::string file = string_member_or(row, "file");
            const std::string line_text = string_member_or(row, "text");
            if (file.empty())
                continue;
            std::smatch match;
            if (!std::regex_search(line_text, match, expression))
                continue;
            const int64_t one_based_line = row.value("line", int64_t{1});
            const std::string prefix = line_text.substr(0, static_cast<size_t>(match.position()));
            const size_t column = sao::ai_editor::native::utf8_to_wide(prefix).size();
            const size_t match_length = sao::ai_editor::native::utf8_to_wide(match.str()).size();
            normalized.push_back({{"file", file},
                                  {"line", std::max<int64_t>(0, one_based_line - 1)},
                                  {"lineText", line_text},
                                  {"column", column},
                                  {"matchLength", std::max<size_t>(1, match_length)}});
            files.insert(file);
        }
        return successful_completion(
            job, {{"query", query},
                  {"results", std::move(normalized)},
                  {"fileCount", files.size()},
                  {"truncated", reply.result.value("total", size_t{0}) >= limit}});
    }
    if (job.method == "editor_surface_state" || job.method == "report_editor_options" ||
        job.method == "report_editor_selection" || job.method == "report_editor_visible_ranges") {
        if (!count(1, 4))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Editor state payload is required.");
        json state;
        if (job.method == "editor_surface_state") {
            state["surface"] = job.args[0];
            for (size_t index = 1; index < job.args.size(); ++index)
                state["context"].push_back(job.args[index]);
        } else if (job.method == "report_editor_options") {
            state["options"] = job.args[0];
            for (size_t index = 1; index < job.args.size(); ++index)
                state["context"].push_back(job.args[index]);
        } else if (job.method == "report_editor_selection") {
            if (!job.args[0].is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT", "Selection uri is required.");
            state["uri"] = workspace_resource_path(job.args[0].get<std::string>());
            if (job.args.size() > 1)
                state["text"] = job.args[1];
            if (job.args.size() > 2)
                state["language"] = job.args[2];
            if (job.args.size() > 3 && job.args[3].is_object())
                state["range"] = job.args[3];
        } else {
            if (!job.args[0].is_array())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Visible ranges must be an array.");
            state["ranges"] = job.args[0];
            for (size_t index = 1; index < job.args.size(); ++index)
                state["context"].push_back(job.args[index]);
        }
        adapter.editor_state[job.method] = std::move(state);
        return successful_completion(job, {{"ok", true}, {"state", adapter.editor_state}});
    }
    if (job.method == "workspace_file_decorations") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Decoration path is required.");
        return successful_completion(job, {{"path", job.args[0]}, {"decorations", json::array()}});
    }
    if (job.method == "list_breakpoints" || job.method == "toggle_breakpoint") {
        if (!count(0, 2) || (!job.args.empty() && !job.args[0].is_string()) ||
            (job.method == "toggle_breakpoint" &&
             (job.args.size() != 2 || !job.args[1].is_number_integer())))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Breakpoint arguments are invalid.");
        const std::string uri = job.args.empty() ? std::string{} : job.args[0].get<std::string>();
        auto& lines = adapter.breakpoints[uri];
        if (job.method == "toggle_breakpoint") {
            const int64_t line = job.args[1].get<int64_t>();
            const auto found = std::find(lines.begin(), lines.end(), line);
            if (found == lines.end())
                lines.push_back(line);
            else
                lines.erase(found);
        }
        json breakpoints = json::array();
        for (const int64_t line : lines)
            breakpoints.push_back({{"uri", uri}, {"line", line}, {"enabled", true}});
        return successful_completion(job, {{"breakpoints", std::move(breakpoints)}});
    }
    if (job.method == "list_editor_languages") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_editor_languages takes no arguments.");
        RpcReply reply = call_backend(adapter, "vscode.languages.getLanguages", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        return successful_completion(
            job, {{"languages", reply.result.is_array() ? reply.result : json::array()}});
    }
    if (job.method == "list_editor_themes") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_editor_themes takes no arguments.");
        return successful_completion(
            job,
            {{"themes", json::array({{{"id", "light"}, {"label", "SAO Light"}, {"kind", "light"}},
                                     {{"id", "dark"}, {"label", "SAO Dark"}, {"kind", "dark"}}})}});
    }
    if (job.method == "get_editor_theme" || job.method == "get_editor_icon_theme") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Theme id is required.");
        const std::string id = job.args[0].get<std::string>();
        return successful_completion(job, {{"id", id},
                                           {"label", id == "light" ? "SAO Light" : "SAO Dark"},
                                           {"kind", id == "light" ? "light" : "dark"},
                                           {"icons", json::object()}});
    }
    if (job.method == "apply_workspace_text_edits") {
        if (!count(1, 1) || !job.args[0].is_array() || job.args[0].size() > 1024)
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Workspace edits must be a bounded array.");
        std::map<std::string, json> grouped;
        json skipped = json::array();
        for (size_t index = 0; index < job.args[0].size(); ++index) {
            const json& edit = job.args[0][index];
            if (!edit.is_object()) {
                skipped.push_back({{"index", index}, {"reason", "Edit must be an object."}});
                continue;
            }
            std::string resource;
            for (const auto key : {"uri", "targetUri", "path"}) {
                const auto value = edit.find(key);
                if (value != edit.end() && value->is_string()) {
                    resource = workspace_resource_path(value->get<std::string>());
                    break;
                }
            }
            if (resource.empty()) {
                skipped.push_back({{"index", index}, {"reason", "Edit target is missing."}});
                continue;
            }
            grouped[resource].push_back(edit);
        }
        json applied = json::array();
        json errors = json::array();
        for (auto& [resource, edits] : grouped) {
            std::filesystem::path path;
            if (!resolve_workspace_path(adapter, resource, false, path)) {
                errors.push_back(
                    {{"path", resource}, {"error", "Workspace edit target is outside the root."}});
                continue;
            }
            std::string source;
            if (sao::ai_editor::native::read_text_file(path,
                                                       static_cast<uint32_t>(kMaximumResponseBytes),
                                                       source) != SAO_AI_EDITOR_OK) {
                errors.push_back(
                    {{"path", resource}, {"error", "Workspace edit target could not be read."}});
                continue;
            }
            std::wstring text = sao::ai_editor::native::utf8_to_wide(source);
            using TextEdit = std::tuple<size_t, size_t, std::wstring>;
            std::vector<TextEdit> operations;
            bool valid = true;
            for (const auto& edit : edits) {
                json range;
                const auto range_value = edit.find("range");
                if (range_value != edit.end())
                    range = *range_value;
                else if (edit.contains("position"))
                    range = {{"start", edit["position"]}, {"end", edit["position"]}};
                if (!range.is_object() || !range.contains("start") || !range.contains("end")) {
                    valid = false;
                    break;
                }
                size_t start = 0;
                size_t end = 0;
                if (!text_position_offset(text, range["start"], &start) ||
                    !text_position_offset(text, range["end"], &end)) {
                    valid = false;
                    break;
                }
                const auto replacement = edit.find("newText");
                const auto fallback = edit.find("text");
                const json* value = replacement != edit.end() ? &*replacement
                                    : fallback != edit.end()  ? &*fallback
                                                              : nullptr;
                if (value == nullptr || !value->is_string()) {
                    valid = false;
                    break;
                }
                operations.emplace_back(
                    std::min(start, end), std::max(start, end),
                    sao::ai_editor::native::utf8_to_wide(value->get_ref<const std::string&>()));
            }
            std::sort(operations.begin(), operations.end(),
                      [](const TextEdit& left, const TextEdit& right) {
                          return std::get<0>(left) > std::get<0>(right);
                      });
            for (size_t index = 1; valid && index < operations.size(); ++index)
                valid = std::get<1>(operations[index]) <= std::get<0>(operations[index - 1]);
            if (!valid) {
                errors.push_back({{"path", resource},
                                  {"error", "Workspace edit ranges are invalid or overlap."}});
                continue;
            }
            for (const auto& [start, end, replacement] : operations)
                text.replace(start, end - start, replacement);
            const std::string output = sao::ai_editor::native::wide_to_utf8(text);
            if (sao::ai_editor::native::write_text_atomic(path, output) != SAO_AI_EDITOR_OK) {
                errors.push_back(
                    {{"path", resource}, {"error", "Workspace edit target could not be saved."}});
                continue;
            }
            applied.push_back(
                {{"path", relative_workspace_path(adapter, path)}, {"edits", operations.size()}});
        }
        return successful_completion(job, {{"ok", errors.empty()},
                                           {"applied", std::move(applied)},
                                           {"skipped", std::move(skipped)},
                                           {"errors", std::move(errors)}});
    }
    if (job.method == "get_instructions" || job.method == "save_user_instructions" ||
        job.method == "save_instruction_file" || job.method == "delete_instruction_file") {
        RpcReply workspace = ensure_workspace(adapter);
        if (!workspace.ok)
            return rpc_completion(job, std::move(workspace));
        std::filesystem::path user_file;
        std::filesystem::path files_root;
        if (!resolve_workspace_path(adapter, ".sao/instructions.md", true, user_file) ||
            !resolve_workspace_path(adapter, ".sao/instructions", true, files_root)) {
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                     "Instruction storage is outside the workspace.");
        }
        if (job.method == "save_user_instructions") {
            if (!count(1, 1) || !job.args[0].is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Instruction text is required.");
            const int32_t status = sao::ai_editor::native::write_text_atomic(
                user_file, job.args[0].get_ref<const std::string&>());
            return status == SAO_AI_EDITOR_OK
                       ? successful_completion(job, {{"ok", true}})
                       : failed_completion(job, transport_error_code(status),
                                           "User instructions could not be saved.");
        }
        if (job.method == "save_instruction_file") {
            if (!count(2, 2) || !job.args[0].is_string() || !job.args[1].is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Instruction file arguments are invalid.");
            const std::string name = job.args[0].get<std::string>();
            if (name.empty() || name.size() > 128 ||
                name.find_first_of("/\\:") != std::string::npos)
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Instruction file name is invalid.");
            std::filesystem::path path;
            const std::string relative =
                ".sao/instructions/" + name + (name.ends_with(".md") ? "" : ".md");
            if (!resolve_workspace_path(adapter, relative, true, path))
                return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                         "Instruction file is outside the workspace.");
            const int32_t status = sao::ai_editor::native::write_text_atomic(
                path, job.args[1].get_ref<const std::string&>());
            return status == SAO_AI_EDITOR_OK
                       ? successful_completion(job, {{"ok", true}, {"name", name}})
                       : failed_completion(job, transport_error_code(status),
                                           "Instruction file could not be saved.");
        }
        if (job.method == "delete_instruction_file") {
            if (!count(1, 1) || !job.args[0].is_string())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Instruction file name is required.");
            const std::string name = job.args[0].get<std::string>();
            if (name.empty() || name.size() > 128 ||
                name.find_first_of("/\\:") != std::string::npos)
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Instruction file name is invalid.");
            std::filesystem::path path;
            const std::string relative =
                ".sao/instructions/" + name + (name.ends_with(".md") ? "" : ".md");
            if (!resolve_workspace_path(adapter, relative, false, path))
                return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                         "Instruction file is outside the workspace.");
            std::error_code error;
            if (!std::filesystem::remove(path, error) || error)
                return failed_completion(job, "SAO_WORKSPACE_IO_FAILED",
                                         "Instruction file could not be deleted.");
            return successful_completion(job, {{"ok", true}});
        }
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "get_instructions takes no arguments.");
        std::string user_text;
        (void)sao::ai_editor::native::read_text_file(user_file, 1024U * 1024U, user_text);
        json files = json::array();
        std::string combined = user_text;
        std::error_code error;
        if (std::filesystem::is_directory(files_root, error)) {
            for (const auto& item : std::filesystem::directory_iterator(files_root, error)) {
                if (error || files.size() >= 64)
                    break;
                if (!item.is_regular_file(error)) {
                    error.clear();
                    continue;
                }
                std::filesystem::path bounded;
                if (!resolve_workspace_path(adapter, relative_workspace_path(adapter, item.path()),
                                            false, bounded))
                    continue;
                std::string content;
                if (sao::ai_editor::native::read_text_file(bounded, 1024U * 1024U, content) !=
                    SAO_AI_EDITOR_OK)
                    continue;
                const std::string name =
                    sao::ai_editor::native::wide_to_utf8(bounded.filename().native());
                files.push_back({{"name", name}, {"content", content}});
                if (combined.size() + content.size() + 2 <= 4U * 1024U * 1024U)
                    combined.append("\n\n").append(content);
            }
        }
        return successful_completion(job, {{"user_instructions", std::move(user_text)},
                                           {"files", std::move(files)},
                                           {"combined_preview", std::move(combined)}});
    }
    if (job.method == "save_workspace_notebook") {
        if (!count(2, 2) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Notebook path and document are required.");
        std::filesystem::path path;
        if (!resolve_workspace_path(
                adapter, workspace_resource_path(job.args[0].get<std::string>()), true, path))
            return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                     "Notebook path is outside the workspace.");
        const std::string content =
            job.args[1].is_string() ? job.args[1].get<std::string>() : job.args[1].dump(2);
        const int32_t status = sao::ai_editor::native::write_text_atomic(path, content);
        return status == SAO_AI_EDITOR_OK
                   ? successful_completion(
                         job, {{"ok", true}, {"path", relative_workspace_path(adapter, path)}})
                   : failed_completion(job, transport_error_code(status),
                                       "Notebook could not be saved.");
    }
    *handled = false;
    return {};
}

NativeAdapterCompletion handle_runtime_alias_method(NativeAdapter& adapter, const AdapterJob& job,
                                                    bool* handled) {
    *handled = true;
    const auto count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
    if (job.method == "get_mcp_server_status") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP status takes no arguments.");
        const uint16_t port = sao_ai_editor_mcp_http_port();
        return successful_completion(job, {{"ok", true}, {"running", port != 0}, {"port", port}});
    }
    if (job.method == "list_mcp_servers") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP list takes no arguments.");
        RpcReply reply = call_backend(adapter, "mcp.list_servers", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        json servers = array_from_result(reply.result, {"servers", "items"});
        for (auto& server : servers) {
            if (!server.is_object())
                continue;
            const std::string name =
                string_member_or(server, "name", string_member_or(server, "id"));
            server["id"] = name;
            server["name"] = name;
            if (!server.contains("configured"))
                server["configured"] = true;
        }
        return successful_completion(job, {{"servers", std::move(servers)}, {"ok", true}});
    }
    if (job.method == "add_mcp_server") {
        if (!count(1, 1) || !job.args[0].is_object())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP server config is required.");
        json config = job.args[0];
        const std::string name = string_member_or(config, "name", string_member_or(config, "id"));
        if (name.empty())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP server id is required.");
        config["name"] = name;
        config.erase("id");
        config["confirmed"] = true;
        RpcReply reply = call_backend(adapter, "mcp.register_server", config);
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        adapter.mcp_configs[name] = config;
        json result = reply.result.is_object() ? std::move(reply.result) : json::object();
        result["ok"] = true;
        result["id"] = name;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "stop_mcp_server" && count(0, 0)) {
        const int status = sao_ai_editor_stop_mcp_server_streamable_http();
        return status == SAO_AI_EDITOR_OK
                   ? successful_completion(job, {{"ok", true}, {"running", false}})
                   : failed_completion(job, "SAO_MCP_STOP_FAILED",
                                       "HTTP MCP server could not be stopped.");
    }
    if (job.method == "start_mcp_server" && count(1, 1) && job.args[0].is_number_integer()) {
        const int64_t requested = job.args[0].get<int64_t>();
        if (requested < 0 || requested > 65535)
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP port is invalid.");
        const int status =
            sao_ai_editor_run_mcp_server_streamable_http(static_cast<uint16_t>(requested));
        const uint16_t port = sao_ai_editor_mcp_http_port();
        return status == SAO_AI_EDITOR_OK && port != 0
                   ? successful_completion(job, {{"ok", true}, {"running", true}, {"port", port}})
                   : failed_completion(job, "SAO_MCP_START_FAILED",
                                       "HTTP MCP server could not be started.");
    }
    if (job.method == "remove_mcp_server" || job.method == "stop_mcp_server") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP server id is required.");
        const std::string name = job.args[0].get<std::string>();
        RpcReply closed = call_backend(adapter, "mcp.close_server", {{"name", name}});
        if (!closed.ok)
            return rpc_completion(job, std::move(closed));
        if (job.method == "remove_mcp_server")
            adapter.mcp_configs.erase(name);
        return successful_completion(job, {{"ok", true}, {"id", name}});
    }
    if (job.method == "restart_mcp_server" || job.method == "start_mcp_server") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP server id is required.");
        const std::string name = job.args[0].get<std::string>();
        const auto configured = adapter.mcp_configs.find(name);
        if (configured == adapter.mcp_configs.end())
            return failed_completion(job, "SAO_MCP_CONFIG_UNAVAILABLE",
                                     "MCP server restart configuration is unavailable.");
        json config = configured->second;
        config["name"] = name;
        config.erase("id");
        config["confirmed"] = true;
        if (job.method == "restart_mcp_server")
            (void)call_backend(adapter, "mcp.close_server", {{"name", name}});
        RpcReply started = call_backend(adapter, "mcp.register_server", config);
        if (!started.ok)
            return rpc_completion(job, std::move(started));
        json result = started.result.is_object() ? std::move(started.result) : json::object();
        result["ok"] = true;
        result["id"] = name;
        return successful_completion(job, std::move(result));
    }
    if (job.method == "get_mcp_logs") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "MCP server id is required.");
        return successful_completion(job,
                                     {{"id", job.args[0]},
                                      {"logs", json::array()},
                                      {"message", "Runtime MCP log streaming is not enabled."}});
    }
    if (job.method == "list_auth_sessions") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Provider id is required.");
        RpcReply token = call_backend(adapter, "auth.load_token", {{"providerId", job.args[0]}});
        json sessions = json::array();
        if (token.ok)
            sessions.push_back({{"id", "native"}, {"provider", job.args[0]}, {"configured", true}});
        return successful_completion(job, {{"sessions", std::move(sessions)}});
    }
    if (job.method == "create_auth_session") {
        if (!count(2, 3) || !job.args[0].is_string() || !job.args[1].is_string() ||
            (job.args.size() > 2 && !job.args[2].is_string()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Auth session arguments are invalid.");
        json token{{"accessToken", job.args[1]}};
        if (job.args.size() > 2)
            token["label"] = job.args[2];
        RpcReply stored = call_backend(adapter, "auth.store_token",
                                       {{"providerId", job.args[0]}, {"token", std::move(token)}});
        if (!stored.ok)
            return rpc_completion(job, std::move(stored));
        return successful_completion(job, {{"ok", true}, {"id", "native"}});
    }
    if (job.method == "remove_auth_session") {
        if (!count(2, 2) || !job.args[0].is_string() || !job.args[1].is_string() ||
            job.args[1].get_ref<const std::string&>() != "native")
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Auth session arguments are invalid.");
        return rpc_completion(
            job, call_backend(adapter, "auth.revoke_token", {{"providerId", job.args[0]}}));
    }
    if (job.method == "execute_command") {
        if (!count(1, 64) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Command id is required.");
        json arguments = json::array();
        for (size_t index = 1; index < job.args.size(); ++index)
            arguments.push_back(job.args[index]);
        return rpc_completion(
            job, call_backend(adapter, "extensions.execute_command",
                              {{"command", job.args[0]}, {"arguments", std::move(arguments)}}));
    }
    if (job.method == "list_installed_extensions" ||
        job.method == "list_extension_runtime_surfaces" ||
        job.method == "list_extension_activity_bar_items" ||
        job.method == "list_extension_view_containers" ||
        job.method == "list_extension_container_views") {
        if (!count(0, 1))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Extension list arguments are invalid.");
        if (job.method != "list_installed_extensions") {
            return successful_completion(
                job, {{"available", false},
                      {"items", json::array()},
                      {"reason", "Extension contribution inventory is unavailable."}});
        }
        RpcReply reply = call_backend(adapter, "extensions.list", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        json items = array_from_result(
            reply.result, {"extensions", "items", "activityBarItems", "viewContainers", "views"});
        return successful_completion(
            job, {{job.method == "list_installed_extensions" ? "extensions" : "items",
                   std::move(items)}});
    }
    if (job.method == "uninstall_extension") {
        if (!count(1, 2) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Extension id is required.");
        return rpc_completion(
            job, call_backend(adapter, "extensions.unregister", {{"extensionId", job.args[0]}}));
    }
    if (job.method == "verify_installed_extension_activation") {
        if (!count(0, 1) || (!job.args.empty() && !job.args[0].is_boolean()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Activation verification argument is invalid.");
        return rpc_completion(job, call_backend(adapter, "extensions.snapshot", json::object()));
    }
    if (job.method == "get_extension_detail") {
        if (!count(2, 2) || !job.args[0].is_string() || !job.args[1].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Extension publisher/name are required.");
        RpcReply reply = call_backend(adapter, "extensions.list", json::object());
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        const std::string id =
            job.args[0].get<std::string>() + "." + job.args[1].get<std::string>();
        for (const auto& item : array_from_result(reply.result, {"extensions", "items"})) {
            if (item.is_object() &&
                (string_member_or(item, "id") == id ||
                 string_member_or(item, "name") == job.args[1].get<std::string>()))
                return successful_completion(job, item);
        }
        return failed_completion(job, "SAO_EXTENSION_NOT_FOUND", "Extension is not installed.");
    }
    if (job.method == "webview_panel_dispose") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "WebView panel id is required.");
        return rpc_completion(job, call_backend(adapter, "vscode.window.disposeWebviewPanel",
                                                {{"panelId", job.args[0]}}));
    }
    if (job.method == "webview_panel_view_state") {
        if (!count(1, 2) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "WebView panel id is required.");
        const json view_state =
            job.args.size() > 1 && job.args[1].is_object() ? job.args[1] : json::object();
        return rpc_completion(job,
                              call_backend(adapter, "vscode.window.setWebviewPanelViewState",
                                           {{"panelId", job.args[0]},
                                            {"active", view_state.value("active", false)},
                                            {"visible", view_state.value("visible", false)},
                                            {"viewColumn", view_state.value("viewColumn", 1)}}));
    }
    if (job.method == "resolve_extension_webview_view") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "WebView panel id is required.");
        return rpc_completion(job, call_backend(adapter, "vscode.window.revealWebviewPanel",
                                                {{"panelId", job.args[0]}}));
    }
    if (job.method == "webview_post_message") {
        if (!count(2, 3) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "WebView message arguments are invalid.");
        return rpc_completion(job,
                              call_backend(adapter, "vscode.webview.postMessage",
                                           {{"panelId", job.args[0]}, {"message", job.args[1]}}));
    }
    if (job.method == "webview_set_state") {
        if (!count(2, 2) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "WebView state arguments are invalid.");
        return rpc_completion(job,
                              call_backend(adapter, "vscode.window.setWebviewState",
                                           {{"panelId", job.args[0]}, {"state", job.args[1]}}));
    }
    if (job.method == "get_provider_webview") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Provider WebView request id is required.");
        RpcReply reply =
            call_backend(adapter, "vscode.window.getWebviewPanel", {{"panelId", job.args[0]}});
        if (!reply.ok)
            return rpc_completion(job, std::move(reply));
        const json options = reply.result.value("options", json::object());
        return successful_completion(
            job, {{"view_id", string_member_or(reply.result, "panelId")},
                  {"view_type", string_member_or(reply.result, "viewType")},
                  {"html", reply.result.value("html", std::string{})},
                  {"state", reply.result.value("state", json::object())},
                  {"source", "runtime"},
                  {"visible", reply.result.value("visible", false)},
                  {"retainContextWhenHidden", options.value("retainContextWhenHidden", false)}});
    }
    if (job.method == "set_extension_host_diagnostics") {
        if (!count(1, 1) || !job.args[0].is_boolean())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Diagnostics flag is invalid.");
        RpcReply saved =
            save_settings(adapter, {{"extensions", {{"diagnostics_enabled", job.args[0]}}}});
        return saved.ok ? successful_completion(job, {{"ok", true}})
                        : rpc_completion(job, std::move(saved));
    }
    if (job.method == "get_runtime_support_summary") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Runtime summary takes no arguments.");
        RpcReply runtime = call_backend(adapter, "runtime.initialize", json::object());
        RpcReply extensions = call_backend(adapter, "extensions.snapshot", json::object());
        RpcReply mcp = call_backend(adapter, "mcp.list_servers", json::object());
        return successful_completion(
            job, {{"ok", runtime.ok},
                  {"runtime", runtime.ok ? runtime.result : json::object()},
                  {"extensions", extensions.ok ? extensions.result : json::object()},
                  {"mcp", mcp.ok ? mcp.result : json::object()}});
    }
    if (job.method == "provider_cli_status" || job.method == "get_claude_proxy_status") {
        if (!count(0, 1))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Provider status arguments are invalid.");
        return successful_completion(job, {{"ok", true},
                                           {"running", false},
                                           {"available", false},
                                           {"reason", "External provider CLI is not configured."}});
    }
    if (job.method == "get_premium_guide") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Premium guide takes no arguments.");
        return successful_completion(
            job, {{"available", false}, {"message", "Premium guide is not configured."}});
    }
    *handled = false;
    return {};
}

NativeAdapterCompletion handle_process_method(NativeAdapter& adapter, const AdapterJob& job,
                                              bool* handled) {
    *handled = true;
    const auto count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
#if defined(_WIN32)
    if (job.method == "list_tasks" || job.method == "list_debug_configurations") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Process list takes no arguments.");
        RpcReply workspace = ensure_workspace(adapter);
        if (!workspace.ok)
            return rpc_completion(job, std::move(workspace));
        const bool debug = job.method == "list_debug_configurations";
        const auto path =
            adapter.workspace_root / L".vscode" / (debug ? L"launch.json" : L"tasks.json");
        std::string text;
        json definitions = json::array();
        if (sao::ai_editor::native::read_text_file(path, 1024U * 1024U, text) == SAO_AI_EDITOR_OK) {
            json document = json::parse(text, nullptr, false, true);
            if (document.is_object() && json_within_budget(document)) {
                const auto items = document.find(debug ? "configurations" : "tasks");
                if (items != document.end() && items->is_array() && items->size() <= 256) {
                    definitions = *items;
                }
            }
        }
        if (debug)
            adapter.debug_configurations = definitions;
        else
            adapter.task_definitions = definitions;
        json rows = json::array();
        for (size_t index = 0; index < definitions.size(); ++index) {
            const json& definition = definitions[index];
            if (!definition.is_object())
                continue;
            const std::string label =
                string_member_or(definition, debug ? "name" : "label",
                                 (debug ? "Debug " : "Task ") + std::to_string(index + 1));
            const std::string type = string_member_or(definition, "type");
            const std::string command = string_member_or(definition, debug ? "program" : "command",
                                                         string_member_or(definition, "process"));
            rows.push_back({{"index", index},
                            {"label", label},
                            {"type", type},
                            {"detail", type + (command.empty() ? "" : " · " + command)}});
        }
        return successful_completion(job, {{debug ? "configs" : "tasks", std::move(rows)}});
    }
    if (job.method == "run_task" || job.method == "start_debug") {
        if (!count(1, 1) || (!job.args[0].is_number_integer() && !job.args[0].is_string()))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Process definition index is invalid.");
        const bool debug = job.method == "start_debug";
        int64_t requested = -1;
        if (job.args[0].is_number_integer()) {
            requested = job.args[0].get<int64_t>();
        } else {
            const std::string& text = job.args[0].get_ref<const std::string&>();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), requested);
            if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                         "Process definition index is invalid.");
        }
        const json& definitions = debug ? adapter.debug_configurations : adapter.task_definitions;
        if (requested < 0 || static_cast<size_t>(requested) >= definitions.size() ||
            !definitions[static_cast<size_t>(requested)].is_object())
            return failed_completion(job, "SAO_PROCESS_NOT_FOUND",
                                     "Process definition was not found.");
        const json& definition = definitions[static_cast<size_t>(requested)];
        const std::string command = string_member_or(definition, debug ? "program" : "command",
                                                     string_member_or(definition, "process"));
        const std::string name =
            string_member_or(definition, debug ? "name" : "label", debug ? "Debug" : "Task");
        const auto args = definition.find("args");
        const json arguments = args != definition.end() && args->is_array() ? *args : json::array();
        std::filesystem::path cwd = adapter.workspace_root;
        const auto options = definition.find("options");
        if (options != definition.end() && options->is_object()) {
            const auto cwd_value = options->find("cwd");
            if (cwd_value != options->end() && cwd_value->is_string()) {
                std::string cwd_text = cwd_value->get<std::string>();
                if (!expand_workspace_variables(adapter, &cwd_text) ||
                    !resolve_workspace_path(adapter, cwd_text, false, cwd)) {
                    return failed_completion(job, "SAO_WORKSPACE_BOUNDARY",
                                             "Process cwd is outside the workspace.");
                }
            }
        }
        json result;
        std::string error;
        if (!launch_managed_process(adapter, name, debug ? "debug" : "task", command, arguments,
                                    !debug && string_member_or(definition, "type") == "shell", cwd,
                                    job.document_token, job.document_generation, result, error))
            return failed_completion(job, "SAO_PROCESS_START_FAILED", std::move(error));
        const std::string process_id =
            result_identifier(result, {debug ? "sessionId" : "executionId"});
        if (!job_current(adapter, job)) {
            const auto stale = adapter.processes.find(process_id);
            if (stale != adapter.processes.end()) {
                close_managed_process(*stale->second, true);
                adapter.processes.erase(stale);
            }
            return failed_completion(job, "SAO_NAVIGATION_RESET", "Process request is stale.");
        }
        {
            std::lock_guard lock(adapter.mutex);
            request_event_poll_locked(adapter, kEventPollLifetime);
        }
        return successful_completion(job, std::move(result));
    }
    if (job.method == "task_execution_status" || job.method == "debug_session_status" ||
        job.method == "cancel_task" || job.method == "stop_debug") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Process execution id is required.");
        const std::string id = job.args[0].get<std::string>();
        const auto found = adapter.processes.find(id);
        if (found == adapter.processes.end())
            return failed_completion(job, "SAO_PROCESS_NOT_FOUND",
                                     "Process execution was not found.");
        ManagedProcess& process = *found->second;
        if (job.method == "cancel_task" || job.method == "stop_debug") {
            if (process.running && process.job != nullptr)
                (void)TerminateJobObject(process.job, ERROR_CANCELLED);
            return successful_completion(job, {{"ok", true}});
        }
        return successful_completion(job, managed_process_snapshot(process));
    }
    if (job.method == "list_debug_sessions") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "list_debug_sessions takes no arguments.");
        json sessions = json::array();
        for (const auto& [id, process] : adapter.processes)
            if (process->kind == "debug")
                sessions.push_back(managed_process_snapshot(*process));
        return successful_completion(job, {{"sessions", std::move(sessions)}});
    }
    if (job.method == "evaluate_debug_console") {
        if (!count(1, 1) || !job.args[0].is_string())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Debug console input is required.");
        ManagedProcess* target = nullptr;
        for (auto& [id, process] : adapter.processes)
            if (process->kind == "debug" && process->running &&
                process->document_token == job.document_token &&
                process->document_generation == job.document_generation)
                target = process.get();
        if (target == nullptr || target->stdin_write == nullptr)
            return failed_completion(job, "SAO_DEBUG_SESSION_NOT_FOUND",
                                     "No running debug session accepts input.");
        std::string input = job.args[0].get<std::string>();
        if (input.size() > kMaximumConsoleInputBytes)
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Debug console input exceeds 64 KiB.");
        input.append("\r\n");
        {
            std::lock_guard lock(target->input_mutex);
            if (target->input_stopping || target->input_writer_done.load() ||
                target->input_queue.size() >= kMaximumConsoleQueue)
                return failed_completion(job, "SAO_DEBUG_INPUT_FAILED",
                                         "Debug console input queue is unavailable.");
            target->input_queue.push_back(std::move(input));
        }
        target->input_wake.notify_one();
        return successful_completion(
            job, {{"ok", true}, {"output", managed_process_snapshot(*target)["output"]}});
    }
    if (job.method == "get_diagnostics") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "get_diagnostics takes no arguments.");
        json diagnostics = json::array();
        for (const auto& [id, process] : adapter.processes) {
            if (!process->running && process->exit_code != 0)
                diagnostics.push_back({{"source", process->kind},
                                       {"severity", "error"},
                                       {"message", process->name + " exited with code " +
                                                       std::to_string(process->exit_code)},
                                       {"executionId", id}});
        }
        return successful_completion(job, {{"diagnostics", std::move(diagnostics)}});
    }
#else
    if (job.method == "list_tasks" || job.method == "run_task" ||
        job.method == "task_execution_status" || job.method == "cancel_task" ||
        job.method == "list_debug_configurations" || job.method == "start_debug" ||
        job.method == "list_debug_sessions" || job.method == "debug_session_status" ||
        job.method == "stop_debug" || job.method == "evaluate_debug_console" ||
        job.method == "get_diagnostics")
        return failed_completion(job, "SAO_CAPABILITY_MISSING",
                                 "Process execution requires Windows.");
#endif
    *handled = false;
    return {};
}

NativeAdapterCompletion handle_compatibility_method(NativeAdapter& adapter, const AdapterJob& job,
                                                    bool* handled) {
    *handled = true;
    const auto count = [&](size_t minimum, size_t maximum) {
        return job.args.is_array() && job.args.size() >= minimum && job.args.size() <= maximum;
    };
    if (job.method == "implement_plan") {
        if (!count(1, 2))
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Plan input is required.");
        std::string prompt;
        if (job.args[0].is_string())
            prompt = job.args[0].get<std::string>();
        else if (job.args[0].is_object())
            prompt = string_member_or(job.args[0], "prompt", string_member_or(job.args[0], "text"));
        if (prompt.empty())
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Plan input is empty.");
        return start_chat_request(adapter, job, prompt,
                                  job.args.size() > 1 && job.args[1].is_object() ? job.args[1]
                                                                                 : json::object(),
                                  prompt, {});
    }
    if (job.method == "confirm_tool") {
        if (!count(2, 2) || !job.args[0].is_string() || !job.args[1].is_boolean())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Tool confirmation arguments are invalid.");
        return successful_completion(
            job, {{"ok", false}, {"error", "Native tool confirmation has no pending call."}});
    }
    if (job.method == "list_command_palette_commands") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Command list takes no arguments.");
        RpcReply snapshot = call_backend(adapter, "extensions.snapshot", json::object());
        if (!snapshot.ok)
            return rpc_completion(job, std::move(snapshot));
        return successful_completion(
            job, {{"commands", array_from_result(snapshot.result, {"commands", "items"})}});
    }
    if (job.method == "list_editor_title_actions" || job.method == "list_webview_context_actions") {
        if (!count(0, 3))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Action list arguments are invalid.");
        return successful_completion(job, {{"actions", json::array()}});
    }
    if (job.method == "editor_language_provider") {
        if (!count(1, 1) || !job.args[0].is_object())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Language provider payload is invalid.");
        return successful_completion(job,
                                     {{"requestId", job.args[0].value("requestId", json(nullptr))},
                                      {"items", json::array()},
                                      {"providerErrors", json::array()},
                                      {"skipped", true}});
    }
    if (job.method == "list_extension_settings") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Extension settings take no arguments.");
        return successful_completion(
            job, {{"configurations", json::array()}, {"languageDefaults", json::array()}});
    }
    if (job.method == "set_extension_setting" || job.method == "reset_extension_setting" ||
        job.method == "set_extension_language_setting" ||
        job.method == "reset_extension_language_setting") {
        if (!count(2, 5))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Extension setting arguments are invalid.");
        return successful_completion(
            job, {{"ok", false},
                  {"available", false},
                  {"error", "No native extension configuration contribution is active."}});
    }
    if (job.method == "list_scm_providers") {
        if (!count(0, 0))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "SCM provider list takes no arguments.");
        return successful_completion(job, {{"providers", json::array()}});
    }
    if (job.method == "get_scm_quick_diff_baseline" ||
        job.method == "request_scm_quick_diff_original_resource" ||
        job.method == "request_scm_history") {
        if (!count(1, 3))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "SCM request arguments are invalid.");
        return successful_completion(
            job, {{"available", false}, {"reason", "No native SCM provider is active."}});
    }
    if (job.method == "validate_scm_input") {
        if (!count(2, 3))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "SCM input arguments are invalid.");
        return successful_completion(job, {{"validation", nullptr}});
    }
    if (job.method == "set_scm_input_value" || job.method == "accept_scm_input") {
        if (!count(2, 2))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "SCM input arguments are invalid.");
        return successful_completion(
            job,
            {{"ok", false}, {"available", false}, {"error", "No native SCM provider is active."}});
    }
    if (job.method == "notebook_controllers") {
        if (!count(0, 1))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Notebook controller arguments are invalid.");
        return successful_completion(job, {{"controllers", json::array()}});
    }
    if (job.method == "notebook_cell_status_bar_items") {
        if (!count(4, 5))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Notebook status arguments are invalid.");
        return successful_completion(job, {{"items", json::array()}});
    }
    if (job.method == "select_notebook_controller" || job.method == "execute_notebook_controller") {
        if (!count(5, 7))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Notebook controller request is invalid.");
        return successful_completion(
            job,
            {{"ok", false}, {"available", false}, {"error", "No notebook controller is active."}});
    }
    if (job.method == "search_extensions") {
        if (!count(0, 2))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Extension search arguments are invalid.");
        RpcReply listed = call_backend(adapter, "extensions.list", json::object());
        if (!listed.ok)
            return rpc_completion(job, std::move(listed));
        const std::string query = !job.args.empty() && job.args[0].is_string()
                                      ? job.args[0].get<std::string>()
                                      : std::string{};
        json matches = json::array();
        for (const auto& extension : array_from_result(listed.result, {"extensions", "items"})) {
            if (!extension.is_object())
                continue;
            const std::string searchable =
                string_member_or(extension, "id") + " " + string_member_or(extension, "name");
            if (query.empty() || searchable.find(query) != std::string::npos)
                matches.push_back(extension);
        }
        return successful_completion(job, {{"extensions", std::move(matches)},
                                           {"source", "installed"},
                                           {"marketplaceAvailable", false}});
    }
    if (job.method == "get_extension_install_preflight" || job.method == "install_extension" ||
        job.method == "install_extension_from_dir_dialog" || job.method == "install_node_runtime") {
        return successful_completion(
            job, {{"ok", false},
                  {"available", false},
                  {"error", "Native extension package installation is not configured."}});
    }
    if (job.method == "start_claude_proxy" || job.method == "stop_claude_proxy" ||
        job.method == "launch_provider_cli" || job.method == "stop_provider_cli") {
        return successful_completion(job,
                                     {{"ok", false},
                                      {"available", false},
                                      {"error", "External provider process is not configured."}});
    }
    if (job.method == "test_connection") {
        if (!count(1, 1) || !job.args[0].is_object())
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Connection settings are invalid.");
        RpcReply models = call_backend(adapter, "models.list", job.args[0]);
        return models.ok ? successful_completion(job, {{"ok", true}, {"models", models.result}})
                         : rpc_completion(job, std::move(models));
    }
    if (job.method == "save_feedback") {
        if (!count(1, 2))
            return failed_completion(job, "SAO_INVALID_ARGUMENT", "Feedback payload is required.");
        return rpc_completion(job, call_backend(adapter, "sao.host.log",
                                                {{"kind", "feedback"}, {"payload", job.args}}));
    }
    if (job.method == "assistant_native_response_fixture" ||
        job.method == "assistant_response_part_action") {
        return successful_completion(
            job, {{"ok", false},
                  {"available", false},
                  {"error", "Assistant fixture actions are disabled in production."}});
    }
    if (job.method == "extension_quick_input_action" ||
        job.method == "extension_window_message_action" ||
        job.method == "execute_extension_tree_item_action" ||
        job.method == "run_extension_task_type" || job.method == "start_extension_debugger_type" ||
        job.method == "save_extension_custom_editor" ||
        job.method == "save_extension_custom_editor_as" ||
        job.method == "revert_extension_custom_editor" ||
        job.method == "undo_extension_custom_editor" ||
        job.method == "redo_extension_custom_editor") {
        json arguments = job.args;
        return rpc_completion(
            job, call_backend(adapter, "extensions.execute_command",
                              {{"command", job.method}, {"arguments", std::move(arguments)}}));
    }
    if (job.method == "load_extension_tree_children" ||
        job.method == "set_extension_tree_item_expanded" ||
        job.method == "set_extension_tree_item_checkbox_state" ||
        job.method == "drop_extension_tree_items" || job.method == "select_extension_tree_item" ||
        job.method == "set_extension_activity_view_visibility") {
        if (!count(1, 5))
            return failed_completion(job, "SAO_INVALID_ARGUMENT",
                                     "Extension tree request is invalid.");
        return successful_completion(job,
                                     {{"ok", true}, {"items", json::array()}, {"applied", true}});
    }
    *handled = false;
    return {};
}

NativeAdapterCompletion execute_job(NativeAdapter& adapter, const AdapterJob& job) {
    bool handled = false;
    NativeAdapterCompletion completion = handle_startup_method(adapter, job, &handled);
    if (handled)
        return completion;
    completion = handle_chat_method(adapter, job, &handled);
    if (handled)
        return completion;
    completion = handle_agent_workflow_method(adapter, job, &handled);
    if (handled)
        return completion;
    completion = handle_workspace_method(adapter, job, &handled);
    if (handled)
        return completion;
    completion = handle_runtime_alias_method(adapter, job, &handled);
    if (handled)
        return completion;
    completion = handle_process_method(adapter, job, &handled);
    if (handled)
        return completion;
    completion = handle_compatibility_method(adapter, job, &handled);
    if (handled)
        return completion;
    return failed_completion(job, "SAO_METHOD_UNAVAILABLE",
                             "Native workbench method is not available: " + job.method);
}

void push_event_locked(NativeAdapter& adapter, NativeAdapterEvent event) {
    const auto terminal = [](const NativeAdapterEvent& candidate) {
        return candidate.name == "stream_end" || candidate.name == "provider_stream_end" ||
               candidate.name == "idle" || candidate.name == "provider_idle" ||
               candidate.name == "error" || candidate.name == "provider_error" ||
               candidate.name == "extension_task_lifecycle" ||
               candidate.name == "extension_debug_session";
    };
    if (adapter.events.size() >= kMaximumEvents) {
        const auto removable =
            std::find_if(adapter.events.begin(), adapter.events.end(),
                         [&](const NativeAdapterEvent& candidate) { return !terminal(candidate); });
        if (removable != adapter.events.end()) {
            adapter.events.erase(removable);
        } else if (!terminal(event)) {
            ++adapter.dropped_events;
            return;
        } else {
            adapter.events.pop_front();
        }
        ++adapter.dropped_events;
    }
    adapter.events.push_back(std::move(event));
}

void translate_event_locked(NativeAdapter& adapter, const json& notification,
                            const std::string& document_token, uint64_t document_generation) {
    if (!notification.is_object() || !valid_string_member(notification, "method") ||
        notification["method"] != "sao.event") {
        return;
    }
    const auto params = notification.find("params");
    if (params == notification.end() || !params->is_object() ||
        !valid_string_member(*params, "event")) {
        return;
    }
    const auto run_value = params->find("runId");
    std::string run_id;
    std::string provider_id;
    PendingWorkflow* workflow = nullptr;
    if (run_value != params->end()) {
        if (!run_value->is_string())
            return;
        run_id = run_value->get<std::string>();
        const auto binding = adapter.runs.find(run_id);
        if (binding != adapter.runs.end()) {
            if (binding->second.document_token != document_token ||
                binding->second.document_generation != document_generation) {
                return;
            }
            provider_id = binding->second.provider_id;
        } else {
            const auto pending = adapter.workflows.find(run_id);
            if (pending == adapter.workflows.end() ||
                pending->second.job.document_token != document_token ||
                pending->second.job.document_generation != document_generation) {
                return;
            }
            workflow = &pending->second;
        }
    }
    const std::string native_name = (*params)["event"].get<std::string>();
    json payload = params->contains("payload") ? (*params)["payload"] : json::object();
    if (!payload.is_object())
        payload = {{"value", std::move(payload)}};
    if (!run_id.empty())
        payload["run_id"] = run_id;
    if (!provider_id.empty())
        payload["provider"] = provider_id;
    if (workflow != nullptr) {
        payload["workflowRunId"] = workflow->frontend_run_id;
        payload["workflowId"] = workflow->workflow_id;
    }
    const auto emit = [&](std::string name, json value) {
        push_event_locked(adapter, {document_token, std::move(name), std::move(value)});
    };
    if (workflow != nullptr && native_name == "workflow.progress") {
        if (payload.contains("stepIndex"))
            payload["step"] = payload["stepIndex"];
        if (payload.contains("totalSteps"))
            payload["total"] = payload["totalSteps"];
        if (payload.contains("outputVar"))
            payload["output_var"] = payload["outputVar"];
        const std::string status = string_member_or(payload, "status");
        if (status == "step_started")
            payload["status"] = "running";
        else if (status == "step_completed" || status == "step_skipped")
            payload["status"] = "done";
        else if (status == "step_failed")
            payload["status"] = "error";
        emit("workflow_step", payload);
    } else if (workflow != nullptr && native_name == "workflow.step_delta") {
        if (payload.contains("stepIndex"))
            payload["step"] = payload["stepIndex"];
        payload["status"] = "running";
        emit("workflow_step", payload);
    } else if (native_name == "chat.delta" || native_name == "agent.delta") {
        emit(provider_id.empty() ? "stream_delta" : "provider_stream_delta", payload);
    } else if (native_name == "run.completed") {
        emit(provider_id.empty() ? "stream_end" : "provider_stream_end", payload);
        emit(provider_id.empty() ? "idle" : "provider_idle", payload);
    } else if (native_name == "run.cancelled" || native_name == "run.stale") {
        payload["cancelled"] = true;
        emit(provider_id.empty() ? "stream_end" : "provider_stream_end", payload);
        emit(provider_id.empty() ? "idle" : "provider_idle", payload);
    } else if (native_name == "run.failed") {
        if (!payload.contains("error") && payload.contains("message"))
            payload["error"] = payload["message"];
        emit(provider_id.empty() ? "error" : "provider_error", payload);
        emit(provider_id.empty() ? "idle" : "provider_idle", payload);
    } else if (native_name == "chat.retry") {
        emit(provider_id.empty() ? "tool_progress" : "provider_tool_progress", payload);
    } else if (native_name == "chat.metrics") {
        emit(provider_id.empty() ? "token_warning" : "provider_token_warning", payload);
    } else if (native_name != "run.started") {
        emit(native_name, payload);
    }
    if (!run_id.empty() && (native_name == "run.completed" || native_name == "run.cancelled" ||
                            native_name == "run.failed" || native_name == "run.stale")) {
        adapter.runs.erase(run_id);
        if (adapter.active_run_id == run_id)
            adapter.active_run_id.clear();
    }
}

void poll_runs(NativeAdapter& adapter, const std::string& document_token,
               uint64_t document_generation) {
    std::vector<std::string> run_ids;
    {
        std::lock_guard lock(adapter.mutex);
        if (adapter.stopping || document_token != adapter.document_token ||
            document_generation != adapter.document_generation) {
            return;
        }
        for (const auto& [run_id, binding] : adapter.runs)
            run_ids.push_back(run_id);
        std::sort(run_ids.begin(), run_ids.end());
        if (!run_ids.empty()) {
            auto selected =
                std::upper_bound(run_ids.begin(), run_ids.end(), adapter.run_poll_cursor);
            if (selected == run_ids.end())
                selected = run_ids.begin();
            adapter.run_poll_cursor = *selected;
            run_ids = {*selected};
        }
    }
    for (const auto& run_id : run_ids) {
        RpcReply reply =
            call_backend(adapter, "run.status", {{"runId", run_id}}, kStatusPollTimeoutMs);
        std::lock_guard lock(adapter.mutex);
        if (adapter.stopping || document_token != adapter.document_token ||
            document_generation != adapter.document_generation) {
            return;
        }
        const auto found = adapter.runs.find(run_id);
        if (found == adapter.runs.end())
            continue;
        RunBinding& binding = found->second;
        if (!reply.ok || !reply.result.is_object()) {
            ++binding.status_failures;
            if (binding.status_failures == kMaximumEventPollFailures) {
                push_event_locked(
                    adapter, {document_token,
                              "run_status_warning",
                              {{"run_id", run_id},
                               {"provider", binding.provider_id},
                               {"error", reply.error_message.empty() ? "Run status request failed."
                                                                     : reply.error_message}}});
            }
            continue;
        }
        binding.status_failures = 0;
        const std::string status = string_member_or(reply.result, "status", "running");
        if (status != "completed" && status != "cancelled" && status != "failed" &&
            status != "stale") {
            continue;
        }
        json payload = reply.result.contains("result") ? reply.result["result"] : json::object();
        if (!payload.is_object())
            payload = {{"result", std::move(payload)}};
        payload["run_id"] = run_id;
        if (!binding.provider_id.empty())
            payload["provider"] = binding.provider_id;
        const bool provider = !binding.provider_id.empty();
        if (status == "failed") {
            if (!payload.contains("error"))
                payload["error"] = string_member_or(reply.result, "error", "Run failed.");
            push_event_locked(adapter,
                              {document_token, provider ? "provider_error" : "error", payload});
        } else {
            payload["cancelled"] = status != "completed";
            push_event_locked(adapter, {document_token,
                                        provider ? "provider_stream_end" : "stream_end", payload});
        }
        push_event_locked(adapter, {document_token, provider ? "provider_idle" : "idle", payload});
        adapter.runs.erase(found);
        if (adapter.active_run_id == run_id)
            adapter.active_run_id.clear();
    }
}

void poll_workflows(NativeAdapter& adapter, const std::string& document_token,
                    uint64_t document_generation) {
    std::vector<std::string> execution_ids;
    {
        std::lock_guard lock(adapter.mutex);
        if (adapter.stopping || document_token != adapter.document_token ||
            document_generation != adapter.document_generation) {
            return;
        }
        for (const auto& [execution_id, pending] : adapter.workflows)
            execution_ids.push_back(execution_id);
        std::sort(execution_ids.begin(), execution_ids.end());
        if (!execution_ids.empty()) {
            auto selected = std::upper_bound(execution_ids.begin(), execution_ids.end(),
                                             adapter.workflow_poll_cursor);
            if (selected == execution_ids.end())
                selected = execution_ids.begin();
            adapter.workflow_poll_cursor = *selected;
            execution_ids = {*selected};
        }
    }
    for (const auto& execution_id : execution_ids) {
        RpcReply reply = call_backend(adapter, "workflows.status", {{"executionId", execution_id}},
                                      kStatusPollTimeoutMs);
        std::lock_guard lock(adapter.mutex);
        if (adapter.stopping || document_token != adapter.document_token ||
            document_generation != adapter.document_generation) {
            return;
        }
        const auto found = adapter.workflows.find(execution_id);
        if (found == adapter.workflows.end())
            continue;
        PendingWorkflow& pending = found->second;
        if (!reply.ok || !reply.result.is_object()) {
            ++pending.status_failures;
            if (pending.status_failures == kMaximumEventPollFailures) {
                push_event_locked(adapter, {document_token,
                                            "workflow_status_warning",
                                            {{"workflowRunId", pending.frontend_run_id},
                                             {"workflowId", pending.workflow_id},
                                             {"error", reply.error_message.empty()
                                                           ? "Workflow status request failed."
                                                           : reply.error_message}}});
            }
            continue;
        }
        pending.status_failures = 0;
        const std::string status = string_member_or(reply.result, "status", "running");
        const size_t current_step = reply.result.value("currentStep", size_t{0});
        const size_t previous_step = pending.last_snapshot.is_object()
                                         ? pending.last_snapshot.value("currentStep", size_t{0})
                                         : std::numeric_limits<size_t>::max();
        const bool snapshot_changed = previous_step != current_step ||
                                      string_member_or(pending.last_snapshot, "status") != status;
        if (snapshot_changed) {
            json event = reply.result;
            event["workflowRunId"] = pending.frontend_run_id;
            event["workflowId"] = pending.workflow_id;
            event["step"] = current_step;
            event["total"] = reply.result.value("totalSteps", size_t{0});
            push_event_locked(adapter, {document_token, "workflow_step", std::move(event)});
        }
        pending.last_snapshot = reply.result;
        if (status != "completed" && status != "failed" && status != "cancelled")
            continue;
        json result = reply.result;
        result["workflowRunId"] = pending.frontend_run_id;
        result["workflow"] = pending.workflow_id;
        result["cancelled"] = status == "cancelled";
        if (status == "failed" && string_member_or(result, "error").empty())
            result["error"] = "Workflow failed.";
        json steps = json::array();
        const auto step_results = result.find("stepResults");
        if (step_results != result.end() && step_results->is_array()) {
            for (const auto& raw_step : *step_results) {
                if (!raw_step.is_object())
                    continue;
                json step = raw_step;
                if (step.contains("stepIndex"))
                    step["step"] = step["stepIndex"];
                if (step.contains("outputVar"))
                    step["output_var"] = step["outputVar"];
                if (step.contains("content")) {
                    step["output"] = step["content"];
                    if (step["content"].is_string() &&
                        !step["content"].get_ref<const std::string&>().empty()) {
                        result["final_output"] = step["content"];
                    }
                }
                steps.push_back(std::move(step));
            }
        }
        result["steps"] = std::move(steps);
        const auto started = result.find("startedAt");
        const auto completed = result.find("completedAt");
        if (started != result.end() && completed != result.end() && started->is_number_integer() &&
            completed->is_number_integer()) {
            result["workflowDurationMs"] =
                std::max<int64_t>(0, completed->get<int64_t>() - started->get<int64_t>());
        }
        if (adapter.completions.size() >= kMaximumCompletions)
            continue;
        adapter.completions.push_back(successful_completion(pending.job, std::move(result)));
        remember_workflow_binding_locked(adapter, pending.frontend_run_id,
                                         {execution_id, pending.workflow_id,
                                          pending.job.document_token,
                                          pending.job.document_generation, false});
        adapter.workflows.erase(found);
    }
}

void process_event_poll(NativeAdapter& adapter, std::string document_token,
                        uint64_t document_generation, bool discard) {
    bool has_more = false;
    bool valid_batch = false;
    size_t drained_count = 0;
    size_t upstream_dropped = 0;
    for (size_t batch = 0; batch < (discard ? 16U : 1U); ++batch) {
        RpcReply reply =
            call_backend(adapter, "events.drain", {{"limit", 64}}, kStatusPollTimeoutMs);
        if (!reply.ok || !reply.result.is_object())
            break;
        const auto drained = reply.result.find("events");
        if (drained == reply.result.end() || !drained->is_array())
            break;
        valid_batch = true;
        drained_count += drained->size();
        upstream_dropped += reply.result.value("dropped", size_t{0});
        const auto more = reply.result.find("hasMore");
        has_more = more != reply.result.end() && more->is_boolean() && more->get<bool>();
        if (!discard) {
            std::lock_guard lock(adapter.mutex);
            if (adapter.stopping || document_token != adapter.document_token ||
                document_generation != adapter.document_generation) {
                return;
            }
            for (const auto& event : *drained)
                translate_event_locked(adapter, event, document_token, document_generation);
        }
        if (!has_more)
            break;
    }
    std::lock_guard lock(adapter.mutex);
    if (adapter.stopping || document_token != adapter.document_token ||
        document_generation != adapter.document_generation) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!valid_batch) {
        ++adapter.consecutive_event_poll_failures;
        adapter.event_reset_empty_passes = 0;
        if (adapter.consecutive_event_poll_failures >= kMaximumEventPollFailures) {
            adapter.worker_failed = true;
            adapter.event_reset_pending = false;
            adapter.event_polling = false;
            adapter.wake.notify_all();
            return;
        }
        const size_t shift = std::min<size_t>(adapter.consecutive_event_poll_failures, 5);
        adapter.next_event_poll = now + kEventPollInterval * (size_t{1} << shift);
        return;
    }
    adapter.consecutive_event_poll_failures = 0;
    if (!discard)
        adapter.dropped_events += upstream_dropped;
    if (discard) {
        if (!has_more && drained_count == 0)
            ++adapter.event_reset_empty_passes;
        else
            adapter.event_reset_empty_passes = 0;
        adapter.event_reset_pending = has_more || adapter.event_reset_empty_passes < 2;
        if (!adapter.event_reset_pending) {
            adapter.event_reset_empty_passes = 0;
            adapter.event_polling = now < adapter.event_poll_deadline || !adapter.runs.empty() ||
                                    !adapter.workflows.empty();
        }
    }
    adapter.next_event_poll = now + kEventPollInterval;
}

void worker_main(NativeAdapter* adapter) noexcept {
    try {
        for (;;) {
            AdapterJob job;
            bool have_job = false;
            bool event_task = false;
            bool discard_events = false;
            std::string event_document;
            uint64_t event_generation = 0;
            std::string orphan_id;
            bool orphan_workflow = false;
#if defined(_WIN32)
            bool reset_processes = false;
            std::string process_document;
            uint64_t process_generation = 0;
#endif
            {
                std::unique_lock lock(adapter->mutex);
                const auto ready = [&] {
                    const bool event_due =
                        std::chrono::steady_clock::now() >= adapter->next_event_poll;
                    return adapter->stopping || !adapter->orphan_run_ids.empty() ||
                           !adapter->orphan_workflow_ids.empty() ||
#if defined(_WIN32)
                           adapter->process_reset_pending ||
#endif
                           (adapter->event_reset_pending && event_due) || !adapter->jobs.empty() ||
                           (adapter->event_polling && event_due);
                };
                if ((adapter->event_polling || adapter->event_reset_pending) &&
                    adapter->jobs.empty()) {
                    adapter->wake.wait_until(lock, adapter->next_event_poll, ready);
                } else {
                    adapter->wake.wait(lock, ready);
                }
                if ((!adapter->orphan_run_ids.empty() || !adapter->orphan_workflow_ids.empty()) &&
                    adapter->orphan_cleanup_deadline.time_since_epoch().count() != 0 &&
                    std::chrono::steady_clock::now() >= adapter->orphan_cleanup_deadline) {
                    adapter->orphan_run_ids.clear();
                    adapter->orphan_workflow_ids.clear();
                }
                if (adapter->stopping && adapter->orphan_run_ids.empty() &&
                    adapter->orphan_workflow_ids.empty())
                    break;
#if defined(_WIN32)
                if (adapter->process_reset_pending) {
                    adapter->process_reset_pending = false;
                    reset_processes = true;
                    process_document = adapter->document_token;
                    process_generation = adapter->document_generation;
                    adapter->consecutive_jobs = 0;
                } else
#endif
                    if (!adapter->orphan_run_ids.empty()) {
                    orphan_id = std::move(adapter->orphan_run_ids.front());
                    adapter->orphan_run_ids.pop_front();
                    adapter->consecutive_jobs = 0;
                } else if (!adapter->orphan_workflow_ids.empty()) {
                    orphan_id = std::move(adapter->orphan_workflow_ids.front());
                    adapter->orphan_workflow_ids.pop_front();
                    orphan_workflow = true;
                    adapter->consecutive_jobs = 0;
                } else if (adapter->event_reset_pending &&
                           std::chrono::steady_clock::now() >= adapter->next_event_poll) {
                    event_task = true;
                    discard_events = true;
                    event_document = adapter->document_token;
                    event_generation = adapter->document_generation;
                    adapter->consecutive_jobs = 0;
                } else if (adapter->event_polling &&
                           std::chrono::steady_clock::now() >= adapter->next_event_poll &&
                           (adapter->jobs.empty() ||
                            adapter->consecutive_jobs >= kMaximumConsecutiveJobs)) {
                    if (std::chrono::steady_clock::now() >= adapter->event_poll_deadline &&
                        adapter->runs.empty() && adapter->workflows.empty()) {
                        adapter->event_polling = false;
                    } else {
                        event_task = true;
                        event_document = adapter->document_token;
                        event_generation = adapter->document_generation;
                        adapter->consecutive_jobs = 0;
                    }
                } else if (!adapter->jobs.empty()) {
                    job = std::move(adapter->jobs.front());
                    adapter->jobs.pop_front();
                    ++adapter->in_flight;
                    ++adapter->consecutive_jobs;
                    have_job = true;
                }
            }
#if defined(_WIN32)
            if (reset_processes) {
                close_stale_managed_processes(*adapter, process_document, process_generation);
                continue;
            }
#endif
            if (!orphan_id.empty()) {
                (void)call_backend(*adapter, orphan_workflow ? "workflows.cancel" : "run.cancel",
                                   orphan_workflow ? json{{"executionId", orphan_id}}
                                                   : json{{"runId", orphan_id}},
                                   kOrphanCancelTimeoutMs);
                continue;
            }
            if (event_task) {
                process_event_poll(*adapter, event_document, event_generation, discard_events);
                if (!discard_events) {
                    poll_runs(*adapter, event_document, event_generation);
                    poll_workflows(*adapter, event_document, event_generation);
                }
#if defined(_WIN32)
                if (!discard_events)
                    poll_managed_processes(*adapter, event_document, event_generation);
#endif
                continue;
            }
            if (!have_job)
                continue;
            {
                std::lock_guard lock(adapter->mutex);
                if (adapter->stopping || job.document_token != adapter->document_token ||
                    job.document_generation != adapter->document_generation) {
                    --adapter->in_flight;
                    continue;
                }
            }
            NativeAdapterCompletion completion;
            try {
                completion = execute_job(*adapter, job);
            } catch (...) {
                completion = failed_completion(job, "SAO_ADAPTER_FAILURE",
                                               "Native workbench adapter request failed.");
            }
            std::unique_lock lock(adapter->mutex);
            --adapter->in_flight;
            adapter->wake.wait(lock, [&] {
                return adapter->stopping || job.document_token != adapter->document_token ||
                       job.document_generation != adapter->document_generation ||
                       adapter->completions.size() < kMaximumCompletions;
            });
            if (!adapter->stopping && job.document_token == adapter->document_token &&
                job.document_generation == adapter->document_generation &&
                !completion.request_id.empty()) {
                adapter->completions.push_back(std::move(completion));
            }
            adapter->wake.notify_all();
        }
    } catch (...) {
        std::lock_guard lock(adapter->mutex);
        adapter->worker_failed = true;
        while (!adapter->jobs.empty() && adapter->completions.size() < kMaximumCompletions) {
            AdapterJob job = std::move(adapter->jobs.front());
            adapter->jobs.pop_front();
            if (job.document_token == adapter->document_token &&
                job.document_generation == adapter->document_generation) {
                adapter->completions.push_back(failed_completion(
                    job, "SAO_ADAPTER_FAILURE", "Native workbench adapter stopped."));
            }
        }
    }
#if defined(_WIN32)
    close_managed_processes(*adapter);
#endif
    {
        std::lock_guard lock(adapter->mutex);
        adapter->worker_exited = true;
    }
    adapter->wake.notify_all();
}

} // namespace

sao_status_t native_adapter_create(sao_ai_editor_launcher_t launcher,
                                   NativeAdapter** out_adapter) noexcept {
    if (out_adapter == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_adapter = nullptr;
    try {
        auto adapter = std::unique_ptr<NativeAdapter>(new (std::nothrow) NativeAdapter{});
        if (!adapter)
            return SAO_STATUS_ERR_UNKNOWN;
        adapter->launcher = launcher;
        adapter->owner_thread = std::this_thread::get_id();
        adapter->worker = std::thread(worker_main, adapter.get());
        *out_adapter = adapter.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t native_adapter_set_document(NativeAdapter* adapter,
                                         std::string_view document_token) noexcept {
    if (adapter == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!adapter_on_owner(*adapter))
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (document_token.size() > 128)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(adapter->mutex);
        if (adapter->stopping)
            return SAO_STATUS_ERR_CANCELLED;
        if (document_token == adapter->document_token)
            return SAO_STATUS_OK;
        for (const auto& [run_id, binding] : adapter->runs)
            enqueue_unique(adapter->orphan_run_ids, run_id);
        for (const auto& [execution_id, pending] : adapter->workflows)
            enqueue_unique(adapter->orphan_workflow_ids, execution_id);
        adapter->orphan_cleanup_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        ++adapter->document_generation;
        if (adapter->document_generation == 0)
            ++adapter->document_generation;
        adapter->document_token.assign(document_token);
        adapter->jobs.clear();
        adapter->completions.clear();
        adapter->events.clear();
        adapter->runs.clear();
        adapter->workflows.clear();
        adapter->workflow_execution_by_frontend.clear();
        adapter->workflow_history_order.clear();
        adapter->active_run_id.clear();
        adapter->run_poll_cursor.clear();
        adapter->workflow_poll_cursor.clear();
#if defined(_WIN32)
        adapter->process_reset_pending = true;
#endif
        adapter->consecutive_jobs = 0;
        adapter->consecutive_event_poll_failures = 0;
        adapter->event_reset_empty_passes = 0;
        adapter->dropped_events = 0;
        adapter->event_reset_pending = !document_token.empty() && adapter->launcher != nullptr;
        adapter->event_polling = false;
        adapter->next_event_poll = std::chrono::steady_clock::now();
        adapter->event_poll_deadline = std::chrono::steady_clock::now() + kEventPollLifetime;
        adapter->wake.notify_all();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t native_adapter_submit(NativeAdapter* adapter, std::string_view document_token,
                                   std::string_view request_id, std::string_view method,
                                   const json& args) noexcept {
    if (adapter == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!adapter_on_owner(*adapter))
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (document_token.empty() || document_token.size() > 128 || request_id.empty() ||
        request_id.size() > 128 || method.empty() || method.size() > 128 || !args.is_array() ||
        args.size() > 64) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        if (!json_within_budget(args) || args.dump().size() > kMaximumRequestBytes)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        std::lock_guard lock(adapter->mutex);
        if (adapter->stopping || adapter->worker_exited || adapter->worker_failed)
            return SAO_STATUS_ERR_CANCELLED;
        if (document_token != adapter->document_token)
            return SAO_STATUS_ERR_ACCESS_DENIED;
        if (adapter->jobs.size() >= kMaximumQueue ||
            adapter->jobs.size() + adapter->in_flight + adapter->completions.size() +
                    adapter->workflows.size() + adapter->runs.size() >=
                kMaximumCompletions) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        adapter->jobs.push_back({std::string(document_token), std::string(request_id),
                                 std::string(method), args, adapter->document_generation});
        request_event_poll_locked(*adapter, kRequestEventPollLifetime);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t native_adapter_drain(NativeAdapter* adapter,
                                  std::vector<NativeAdapterCompletion>* completions,
                                  std::vector<NativeAdapterEvent>* events) noexcept {
    if (adapter == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!adapter_on_owner(*adapter))
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (completions == nullptr || events == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(adapter->mutex);
        if (adapter->worker_failed)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        completions->clear();
        events->clear();
        completions->reserve(adapter->completions.size());
        events->reserve(adapter->events.size() + (adapter->dropped_events != 0 ? 1U : 0U));
        if (adapter->dropped_events != 0) {
            events->push_back({adapter->document_token,
                               "event_stream_reset",
                               {{"dropped", adapter->dropped_events}, {"reconcile", true}}});
            adapter->dropped_events = 0;
        }
        while (!adapter->completions.empty()) {
            completions->push_back(std::move(adapter->completions.front()));
            adapter->completions.pop_front();
        }
        while (!adapter->events.empty()) {
            events->push_back(std::move(adapter->events.front()));
            adapter->events.pop_front();
        }
        adapter->wake.notify_all();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t native_adapter_try_destroy(NativeAdapter* adapter) noexcept {
    if (adapter == nullptr)
        return SAO_STATUS_OK;
    if (!adapter_on_owner(*adapter))
        return SAO_STATUS_ERR_ACCESS_DENIED;
    try {
        {
            std::lock_guard lock(adapter->mutex);
            if (!adapter->stopping) {
                for (const auto& [run_id, binding] : adapter->runs)
                    enqueue_unique(adapter->orphan_run_ids, run_id);
                for (const auto& [execution_id, pending] : adapter->workflows)
                    enqueue_unique(adapter->orphan_workflow_ids, execution_id);
                adapter->orphan_cleanup_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                adapter->stopping = true;
                adapter->jobs.clear();
                adapter->completions.clear();
                adapter->events.clear();
                adapter->runs.clear();
                adapter->workflows.clear();
                adapter->workflow_execution_by_frontend.clear();
                adapter->workflow_history_order.clear();
                adapter->event_reset_pending = false;
                adapter->event_polling = false;
            }
            adapter->wake.notify_all();
            if (!adapter->worker_exited)
                return SAO_STATUS_ERR_CANCELLED;
        }
        if (adapter->worker.joinable())
            adapter->worker.join();
        delete adapter;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::ai_editor::workbench
