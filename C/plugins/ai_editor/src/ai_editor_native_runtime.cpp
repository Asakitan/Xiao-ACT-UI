#include "sao/ai_editor/ai_editor_native.h"

#include "chat_provider_router.h"
#include "native_runtime_internal.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace sao::ai_editor::native {
namespace {

std::string status_message(int32_t status) {
    switch (status) {
        case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL:
            return "buffer too small";
        case SAO_AI_EDITOR_ERR_HANDLE_INVALID:
            return "invalid handle";
        case SAO_AI_EDITOR_ERR_TIMEOUT:
            return "timeout";
        case SAO_AI_EDITOR_ERR_NOT_FOUND:
            return "not found";
        case SAO_AI_EDITOR_ERR_PERMISSION_DENIED:
            return "permission denied";
        case SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED:
            return "confirmation required";
        case SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION:
            return "workspace boundary violation";
        case SAO_AI_EDITOR_ERR_CANCELLED:
            return "cancelled";
        case SAO_AI_EDITOR_ERR_HTTP:
            return "HTTP request failed";
        case SAO_AI_EDITOR_ERR_PROTOCOL:
            return "protocol error";
        case SAO_AI_EDITOR_ERR_BUSY:
            return "busy";
        default:
            return "operation failed";
    }
}

int rpc_code(int32_t status) {
    switch (status) {
        case SAO_AI_EDITOR_ERR_INVALID_ARGUMENT:
            return -32602;
        case SAO_AI_EDITOR_ERR_NOT_FOUND:
            return -32004;
        case SAO_AI_EDITOR_ERR_PERMISSION_DENIED:
            return -32003;
        case SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED:
            return -32002;
        case SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION:
            return -32001;
        case SAO_AI_EDITOR_ERR_CANCELLED:
            return -32800;
        default:
            return -32000;
    }
}

std::string new_run_id() {
    static std::atomic<uint64_t> sequence{0};
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return "run-" + std::to_string(ticks) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string environment_value(std::string_view name) {
    if (name.empty() || !valid_utf8(name)) {
        return {};
    }
    const std::wstring wide_name = utf8_to_wide(name);
    const DWORD required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (required == 0 || required > 64U * 1024U) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        wide_name.c_str(), value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) {
        return {};
    }
    value.resize(written);
    return wide_to_utf8(value);
}

std::string mode_of(const Json& params) {
    std::string mode = params.value("mode", "agent");
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return mode;
}

bool supported_mode(std::string_view mode) {
    return mode == "agent" || mode == "ask" || mode == "plan";
}

Json registry_summary(const Json& registry, std::string_view kind) {
    return Json{{"kind", kind},
                {"items", registry.is_array() ? registry : Json::array()},
                {"total", registry.is_array() ? registry.size() : 0U}};
}

bool parse_scope_key(std::string_view key,
                     std::string& scope,
                     std::string& plugin_id) {
    plugin_id.clear();
    if (key == "system" || key == "workspace") {
        scope = key;
        return true;
    }
    constexpr std::string_view prefix = "plugin:";
    if (key.starts_with(prefix)) {
        plugin_id = std::string(key.substr(prefix.size()));
        if (valid_simple_id(plugin_id)) {
            scope = "plugin";
            return true;
        }
    }
    return false;
}

}  // namespace

NativeRuntime::NativeRuntime(RuntimeOptions options)
        : options_(std::move(options)),
            conversations_(scopes_),
      tools_(scopes_, options.maximum_file_bytes,
             options.maximum_search_results),
      maximum_event_queue_(std::clamp(options.maximum_event_queue, 8U, 4096U)) {}

NativeRuntime::~NativeRuntime() {
    std::vector<std::shared_ptr<RunState>> runs;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& [id, run] : runs_) {
            (void)id;
            runs.push_back(run);
            run->cancellation->cancel();
        }
    }
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        stopping_ = true;
    }
    event_ready_.notify_all();
    for (const auto& run : runs) {
        if (run->worker.joinable()) {
            run->worker.join();
        }
    }
}

int32_t NativeRuntime::initialize() {
    const int32_t status = scopes_.initialize(
        options_.workspace_root, options_.system_root,
        options_.plugin_roots_json);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    secrets_ = std::make_unique<SecretStore>(scopes_.secret_vault_path());
    SaoAiEditorMcpClient* mcp_raw = nullptr;
    if (sao_ai_editor_mcp_client_create(&mcp_raw) != SAO_AI_EDITOR_OK) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    mcp_client_.reset(mcp_raw);
    // Forward every MCP notification (tools/list_changed, prompts/list_changed,
    // resources/list_changed, resources/updated, notifications/message, ...)
    // straight into the runtime event queue so the UI can observe changes as
    // sao.event / mcp.notification without having to poll the MCP APIs.
    sao_ai_editor_mcp_client_set_notification_forwarder(
        mcp_client_.get(), this, &NativeRuntime::mcp_notification_trampoline);
    auth_flow_ = std::make_unique<AuthDeviceFlow>(secrets_.get());
    extension_host_ = std::make_unique<ExtensionHost>(*this);
    return SAO_AI_EDITOR_OK;
}

void SAO_AI_EDITOR_CALL NativeRuntime::mcp_notification_trampoline(
    void* user, const char* json_utf8, uint32_t json_len) {
    if (user == nullptr || json_utf8 == nullptr || json_len == 0) {
        return;
    }
    try {
        auto* self = static_cast<NativeRuntime*>(user);
        Json envelope = Json::parse(json_utf8, json_utf8 + json_len, nullptr,
                                    false);
        if (envelope.is_discarded() || !envelope.is_object()) {
            return;
        }
        const std::string server_name = envelope.value("server", std::string{});
        Json notification = envelope.value("notification", Json::object());
        std::string method = notification.value("method", std::string{});
        if (method.empty()) {
            return;
        }
        Json payload{{"server", server_name},
                     {"method", std::move(method)},
                     {"params", notification.value("params", Json::object())}};
        self->emit("mcp.notification", payload);
    } catch (...) {
        // Never propagate exceptions across the C API boundary.
    }
}

Json NativeRuntime::permission_policy(std::string_view mode) {
    if (!supported_mode(mode)) {
        return Json();
    }
    const bool ask = mode == "ask";
    const bool plan = mode == "plan";
    return Json{{"mode", mode},
                {"read", "allowed"},
                {"write", ask ? "disabled" : plan ? "confirm" : "allowed"},
                {"execute", ask ? "disabled" : plan ? "confirm" : "allowed"}};
}

void NativeRuntime::emit(std::string_view event_name,
                         const Json& payload,
                         std::string_view run_id) {
    Json params{{"event", event_name}, {"payload", payload}};
    if (!run_id.empty()) {
        params["runId"] = run_id;
    }
    Json notification{{"jsonrpc", "2.0"},
                      {"method", "sao.event"},
                      {"params", std::move(params)},
                      {"sao", {{"protocolVersion",
                                SAO_AI_EDITOR_PROTOCOL_VERSION}}}};
    std::string text = dump_json(notification);
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (stopping_) {
            return;
        }
        if (events_.size() >= maximum_event_queue_) {
            events_.pop_front();
        }
        events_.push_back(std::move(text));
    }
    event_ready_.notify_one();
}

int32_t NativeRuntime::next_event(uint32_t timeout_ms,
                                  std::string& event_json) {
    std::unique_lock<std::mutex> lock(event_mutex_);
    const auto ready = [&] { return stopping_ || !events_.empty(); };
    if (timeout_ms == 0) {
        if (!ready()) {
            return SAO_AI_EDITOR_ERR_TIMEOUT;
        }
    } else if (!event_ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      ready)) {
        return SAO_AI_EDITOR_ERR_TIMEOUT;
    }
    if (events_.empty()) {
        return SAO_AI_EDITOR_ERR_CANCELLED;
    }
    event_json = std::move(events_.front());
    events_.pop_front();
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch(const Json& request, Json& response) {
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" ||
        !request.contains("method") || !request["method"].is_string()) {
        response = rpc_error(nullptr, -32600, "invalid JSON-RPC request");
        return SAO_AI_EDITOR_OK;
    }
    const Json id = request.value("id", Json(nullptr));
    const Json params = request.value("params", Json::object());
    if (!params.is_object()) {
        response = rpc_error(id, -32602, "params must be an object");
        return SAO_AI_EDITOR_OK;
    }
    Json result;
    const int32_t status = invoke(request["method"].get_ref<const std::string&>(),
                                  params, result);
    if (status == SAO_AI_EDITOR_ERR_NOT_FOUND &&
        request["method"].get_ref<const std::string&>().find('.') !=
            std::string::npos) {
        response = rpc_error(id, -32601, "method or item not found",
                             Json{{"status", status}});
    } else if (status != SAO_AI_EDITOR_OK) {
        response = rpc_error(id, rpc_code(status), status_message(status),
                             Json{{"status", status}, {"details", result}});
    } else {
        response = rpc_result(id, std::move(result));
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::invoke(std::string_view method,
                              const Json& params,
                              Json& result) {
    if (method == "runtime.initialize") {
        Json config;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            int32_t status = scopes_.load_merged_config(config);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        result = Json{{"nativeAbiVersion", SAO_AI_EDITOR_NATIVE_ABI_VERSION},
                      {"protocolVersion", SAO_AI_EDITOR_PROTOCOL_VERSION},
                      {"workspaceRoot", wide_to_utf8(scopes_.workspace_root().native())},
                      {"scopes", scopes_.describe_scopes()},
                      {"config", std::move(config)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "scopes.list") {
        result = scopes_.describe_scopes();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "config.load") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        if (params.contains("scope")) {
            if (!params["scope"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            std::string scope;
            std::string plugin_id;
            if (!parse_scope_key(params["scope"].get<std::string>(), scope,
                                 plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            return scopes_.load_scope_config(scope, plugin_id, result);
        }
        return scopes_.load_merged_config(result);
    }
    if (method == "config.save") {
        if (!params.contains("scope") || !params["scope"].is_string() ||
            !params.contains("config") || !params["config"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(params["scope"].get<std::string>(), scope,
                             plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status = scopes_.save_scope_config(
            scope, plugin_id, params["config"]);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"scope", params["scope"]}};
        }
        return status;
    }
    if (method == "permission.get") {
        const std::string mode = mode_of(params);
        result = permission_policy(mode);
        return result.is_null() ? SAO_AI_EDITOR_ERR_INVALID_ARGUMENT
                                : SAO_AI_EDITOR_OK;
    }
    if (method == "conversation.create") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.create(params.value("title", "Untitled"),
                                     params.value("model", ""),
                                     params.value("scope", "workspace"), result);
    }
    if (method == "conversation.append") {
        if (!params.contains("id") || !params["id"].is_string() ||
            !params.contains("message")) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.append(params["id"].get<std::string>(),
                                     params["message"], result);
    }
    if (method == "conversation.get") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.get(params["id"].get<std::string>(), result);
    }
    if (method == "conversation.list") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.list(params.value("scope", "all"),
                                   params.value("limit", 100U), result);
    }
    if (method == "conversation.search") {
        if (!params.contains("query") || !params["query"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.search(
            params["query"].get<std::string>(),
            params.value("scope", "all"),
            params.value("limit", 20U),
            result);
    }
    if (method == "conversation.delete") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.remove(params["id"].get<std::string>(), result);
    }
    if (method == "conversation.pin" || method == "conversation.unpin" ||
        method == "conversation.set_pinned") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Method-aware pinned resolution:
        //   * conversation.pin        → pinned defaults to true (payload may
        //                               still force pinned:false)
        //   * conversation.unpin      → always pinned:false; ignore payload
        //   * conversation.set_pinned → payload must supply pinned:bool
        bool pinned = false;
        if (method == "conversation.unpin") {
            pinned = false;
        } else if (method == "conversation.pin") {
            pinned = params.value("pinned", true);
        } else {
            if (!params.contains("pinned") ||
                !params["pinned"].is_boolean()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            pinned = params["pinned"].get<bool>();
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.set_pinned(params["id"].get<std::string>(),
                                          pinned, result);
    }
    if (method == "conversation.branch") {
        if (!params.contains("sourceId") ||
            !params["sourceId"].is_string() ||
            !params.contains("messageIndex") ||
            !params["messageIndex"].is_number_integer()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string source_id = params["sourceId"].get<std::string>();
        const int64_t message_index = params["messageIndex"].get<int64_t>();
        const std::string title = params.value("title", std::string{});
        const std::string scope = params.value("scope",
                                                std::string{"workspace"});
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.branch(source_id, message_index, title, scope,
                                      result);
    }
    if (method == "conversation.export") {
        const bool has_id = params.contains("id");
        const bool has_scope = params.contains("scope");
        if (has_id == has_scope) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (has_id) {
            if (!params["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            Json conversation;
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = conversations_.get(
                params["id"].get<std::string>(), conversation);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            const int64_t now = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::system_clock::now()
                                        .time_since_epoch())
                                    .count();
            result = Json{{"format", "sao-conversation/1"},
                          {"conversation", std::move(conversation)},
                          {"exportedAt", now}};
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.export_all(
            params["scope"].get<std::string>(), result);
    }
    if (method == "conversation.import") {
        if (!params.contains("payload") || !params["payload"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string target_scope =
            params.value("scope", std::string{"workspace"});
        if (target_scope != "workspace" && target_scope != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool overwrite = params.value("overwrite", false);
        const Json& payload = params["payload"];
        const std::string format = payload.value("format", std::string{});
        std::vector<Json> incoming;
        if (format == "sao-conversation/1") {
            if (!payload.contains("conversation") ||
                !payload["conversation"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(payload["conversation"]);
        } else if (format == "sao-conversations/1") {
            if (!payload.contains("conversations") ||
                !payload["conversations"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : payload["conversations"]) {
                if (!item.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                incoming.push_back(item);
            }
        } else {
            result = Json{{"message", "unknown export format"},
                          {"format", format}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (incoming.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        // Validate ids and detect conflicts up front so overwrite=false can
        // reject atomically without a partial import.
        Json conflicts = Json::array();
        for (const auto& conversation : incoming) {
            const std::string candidate_id =
                conversation.value("id", std::string{});
            if (candidate_id.empty()) {
                continue;
            }
            if (!valid_simple_id(candidate_id)) {
                result = Json{{"message", "invalid conversation id"},
                              {"id", candidate_id}};
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            Json existing_doc;
            const int32_t get_status = conversations_.get(
                candidate_id, existing_doc);
            if (get_status == SAO_AI_EDITOR_OK) {
                conflicts.push_back(candidate_id);
            }
        }
        if (!conflicts.empty() && !overwrite) {
            result = Json{{"imported", 0},
                          {"conflicts", std::move(conflicts)},
                          {"assignedIds", Json::array()}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json assigned_ids = Json::array();
        size_t imported = 0;
        for (const auto& conversation : incoming) {
            std::string assigned_id;
            const int32_t import_status = conversations_.import_conversation(
                conversation, target_scope, overwrite, assigned_id);
            if (import_status != SAO_AI_EDITOR_OK) {
                result = Json{{"imported", imported},
                              {"conflicts", std::move(conflicts)},
                              {"assignedIds", std::move(assigned_ids)}};
                return import_status;
            }
            assigned_ids.push_back(assigned_id);
            ++imported;
        }
        result = Json{{"imported", imported},
                      {"conflicts", std::move(conflicts)},
                      {"assignedIds", std::move(assigned_ids)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.export") {
        const bool has_id = params.contains("id");
        const bool has_scope = params.contains("scope");
        if (has_id == has_scope) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        if (has_id) {
            if (!params["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            WorkflowDefinition definition;
            if (!workflow_registry_.get(params["id"].get<std::string>(),
                                        definition)) {
                return SAO_AI_EDITOR_ERR_NOT_FOUND;
            }
            const int64_t now = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::system_clock::now()
                                        .time_since_epoch())
                                    .count();
            result = Json{{"format", "sao-workflow/1"},
                          {"workflow", definition.to_json()},
                          {"exportedAt", now}};
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return workflow_registry_.export_all(
            params["scope"].get<std::string>(), result);
    }
    if (method == "workflow.import") {
        if (!params.contains("payload") || !params["payload"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string scope_key =
            params.value("scope", std::string{"workspace"});
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(scope_key, scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool overwrite = params.value("overwrite", false);
        const Json& payload = params["payload"];
        const std::string format = payload.value("format", std::string{});
        std::vector<Json> incoming;
        if (format == "sao-workflow/1") {
            if (!payload.contains("workflow") ||
                !payload["workflow"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(payload["workflow"]);
        } else if (format == "sao-workflows/1") {
            if (!payload.contains("workflows") ||
                !payload["workflows"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : payload["workflows"]) {
                if (!item.is_object()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                incoming.push_back(item);
            }
        } else {
            result = Json{{"message", "unknown export format"},
                          {"format", format}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (incoming.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        workflow_registry_.reload(scopes_);
        // Two-pass conflict detection so overwrite=false rejects atomically
        // instead of half-importing a batch.  Built-ins are always fatal —
        // even with overwrite=true — because letting a user replace a
        // built-in id would either shadow it after reload or masquerade a
        // user workflow as compile-time.
        Json conflicts = Json::array();
        for (const auto& workflow : incoming) {
            const std::string candidate_id =
                workflow.value("id", std::string{});
            if (candidate_id.empty() || !valid_simple_id(candidate_id)) {
                result = Json{{"message", "invalid workflow id"},
                              {"id", candidate_id}};
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (workflow_registry_.is_builtin(candidate_id)) {
                result = Json{{"imported", 0},
                              {"conflicts", Json::array({candidate_id})},
                              {"assignedIds", Json::array()},
                              {"message", "cannot overwrite builtin"}};
                return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
            }
            WorkflowDefinition existing;
            if (workflow_registry_.get(candidate_id, existing) &&
                !existing.builtin) {
                conflicts.push_back(candidate_id);
            }
        }
        if (!conflicts.empty() && !overwrite) {
            result = Json{{"imported", 0},
                          {"conflicts", std::move(conflicts)},
                          {"assignedIds", Json::array()}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json assigned_ids = Json::array();
        size_t imported = 0;
        for (const auto& workflow : incoming) {
            std::string assigned_id;
            const int32_t import_status = workflow_registry_.import_workflow(
                workflow, scope, plugin_id, overwrite, scopes_, assigned_id);
            if (import_status != SAO_AI_EDITOR_OK) {
                result = Json{{"imported", imported},
                              {"conflicts", std::move(conflicts)},
                              {"assignedIds", std::move(assigned_ids)}};
                return import_status;
            }
            assigned_ids.push_back(assigned_id);
            ++imported;
        }
        result = Json{{"imported", imported},
                      {"conflicts", std::move(conflicts)},
                      {"assignedIds", std::move(assigned_ids)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.list" || method == "workflows.list" ||
        method == "providers.list") {
        const std::string kind(method.substr(0, method.find('.')));
        Json registry;
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status =
            scopes_.load_registry(kind, Json::array(), registry);
        if (status == SAO_AI_EDITOR_OK) {
            result = registry_summary(registry, kind);
        }
        return status;
    }
    if (method == "agents.save" || method == "workflows.save") {
        const std::string kind(method.substr(0, method.find('.')));
        if (!params.contains("scope") || !params["scope"].is_string() ||
            !params.contains("item") || !params["item"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string item_id = params["item"].value("id", "");
        std::string scope;
        std::string plugin_id;
        if (!valid_simple_id(item_id) ||
            !parse_scope_key(params["scope"].get<std::string>(), scope,
                             plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status = scopes_.save_registry_item(
            kind, scope, plugin_id, item_id, params["item"]);
        if (status == SAO_AI_EDITOR_OK) {
            result = params["item"];
            result["scope"] = params["scope"];
        }
        return status;
    }
    if (method == "providers.configure") {
        if (!params.contains("scope") || !params["scope"].is_string() ||
            !params.contains("provider") || !params["provider"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json provider = params["provider"];
        const std::string id = provider.value("id", "");
        if (!valid_simple_id(id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (provider.contains("apiKey")) {
            if (!provider["apiKey"].is_string() || secrets_ == nullptr) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const std::string secret_key = "provider/" + id + "/apiKey";
            const std::string secret_value =
                provider["apiKey"].get<std::string>();
            const int32_t protected_status =
                secret_value.empty() ? secrets_->erase(secret_key)
                                     : secrets_->set(secret_key, secret_value);
            if (protected_status != SAO_AI_EDITOR_OK &&
                protected_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return protected_status;
            }
            provider.erase("apiKey");
        }
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(params["scope"].get<std::string>(), scope,
                             plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        const int32_t status = scopes_.save_registry_item(
            "providers", scope, plugin_id, id, provider);
        if (status == SAO_AI_EDITOR_OK) {
            result = provider;
            result["scope"] = params["scope"];
        }
        return status;
    }
    if (method == "models.list") {
        Json provider;
        std::string secret;
        const int32_t status = resolve_provider(params, provider, secret);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        Json models = provider.value("models", Json::array());
        if (!models.is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        result = Json{{"providerId", provider.value("id", "inline")},
                      {"models", std::move(models)}};
        result["total"] = result["models"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.list") {
        const std::string mode = mode_of(params);
        if (!supported_mode(mode)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        result = Json{{"mode", mode}, {"tools", tools_.describe(mode)}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.call") {
        if (!params.contains("name") || !params["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string mode = mode_of(params);
        if (!supported_mode(mode)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json arguments = params.value("arguments", Json::object());
        std::lock_guard<std::mutex> lock(store_mutex_);
        return tools_.execute(mode, params["name"].get<std::string>(),
                              arguments, result);
    }
    if (method == "chat.run") {
        // Render an optional `promptId` + `promptArguments` into the
        // messages array before delegating to start_chat.  Doing it here
        // (rather than inside start_chat) keeps chat.run_with_mcp free to
        // apply the prompt exactly once via its own path.
        Json forwarded = params;
        const int32_t prompt_status = apply_prompt_source(forwarded);
        if (prompt_status != SAO_AI_EDITOR_OK) {
            return prompt_status;
        }
        forwarded.erase("promptId");
        forwarded.erase("promptArguments");
        return start_chat(forwarded, result);
    }
    if (method == "chat.run_with_mcp") {
        return start_chat_with_mcp(params, result);
    }
    if (method == "run.cancel") {
        return cancel_run(params, result);
    }
    if (method == "run.status") {
        return run_status(params, result);
    }
    if (method.starts_with("mcp.")) {
        return dispatch_mcp(method, params, result);
    }
    if (method.starts_with("workflows.")) {
        return dispatch_workflow(method, params, result);
    }
    if (method.starts_with("auth.")) {
        return dispatch_auth(method, params, result);
    }
    if (method.starts_with("extensions.")) {
        return dispatch_extension(method, params, result);
    }
    if (method == "agents.list_defs" || method == "agents.get_def" ||
        method == "agents.save_def" || method == "agents.delete_def" ||
        method == "agents.invoke") {
        return dispatch_agent(method, params, result);
    }
    if (method == "agents.invoke_with_mcp") {
        return agent_invoke_with_mcp(params, result);
    }
    if (method == "prompts.list_defs" || method == "prompts.get_def" ||
        method == "prompts.save_def" || method == "prompts.delete_def" ||
        method == "prompts.render") {
        return dispatch_prompt(method, params, result);
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_agent(std::string_view method,
                                      const Json& params, Json& result) {
    if (method == "agents.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        Json items = Json::array();
        for (const auto& agent : agent_registry_.list()) {
            items.push_back(agent.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.get_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        AgentDefinition agent;
        if (!agent_registry_.get(params["id"].get<std::string>(), agent)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = agent.to_json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.save_def") {
        if (!params.contains("agent") || !params["agent"].is_object() ||
            !params.contains("scope") || !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        AgentDefinition agent =
            AgentDefinition::from_json(params["agent"]);
        const std::string scope_key = params["scope"].get<std::string>();
        std::string scope;
        std::string plugin_id;
        if (scope_key == "system" || scope_key == "workspace") {
            scope = scope_key;
        } else if (scope_key.rfind("plugin:", 0) == 0) {
            scope = "plugin";
            plugin_id = scope_key.substr(7);
            if (!valid_simple_id(plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = agent_registry_.save(agent, scopes_, scope,
                                                     plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = agent.to_json();
        result["scope"] = scope_key;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.delete_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = agent_registry_.remove(
            params["id"].get<std::string>(), scopes_,
            params.value("scope", "workspace"), std::string{});
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"id", params["id"]}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "agents.invoke") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string message = params.value("message", std::string{});
        if (message.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        AgentDefinition agent;
        if (!agent_registry_.get(params["id"].get<std::string>(), agent)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        // conversationId is authoritative when provided: we load the stored
        // history from disk and ignore the `history` param.  If it's absent
        // and createConversation=true, we lazily create a fresh workspace
        // conversation and return its id so the caller can keep appending.
        std::string conversation_id = params.value("conversationId",
                                                    std::string{});
        const bool create_conversation = params.value("createConversation",
                                                       false);
        Json history_from_store = Json::array();
        if (!conversation_id.empty()) {
            Json conversation_doc;
            int32_t get_status = SAO_AI_EDITOR_OK;
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                get_status = conversations_.get(conversation_id,
                                                 conversation_doc);
            }
            if (get_status != SAO_AI_EDITOR_OK) {
                return get_status;
            }
            history_from_store = conversation_doc.value("messages",
                                                         Json::array());
        } else if (create_conversation) {
            Json created;
            const std::string title = params.value("conversationTitle",
                                                    agent.name);
            const std::string convo_model = params.value(
                "model", agent.model);
            int32_t create_status = SAO_AI_EDITOR_OK;
            {
                std::lock_guard<std::mutex> guard(store_mutex_);
                create_status = conversations_.create(
                    title, convo_model, "workspace", created);
            }
            if (create_status != SAO_AI_EDITOR_OK) {
                return create_status;
            }
            conversation_id = created.value("id", std::string{});
        }
        // When a conversationId is in play, the persisted history overrides
        // any caller-supplied `history` array.  Without one, we fall back to
        // the legacy single-shot behaviour that still respects `history`.
        const Json history = !conversation_id.empty()
            ? history_from_store
            : params.value("history", Json::array());
        Json messages =
            agent_registry_.build_chat_messages(agent, message, history);
        const std::string effective_model = params.value(
            "model", agent.model);
        Json chat_params = params;
        chat_params.erase("id");
        chat_params.erase("message");
        chat_params.erase("history");
        chat_params.erase("conversationId");
        chat_params.erase("createConversation");
        chat_params.erase("conversationTitle");
        chat_params["messages"] = std::move(messages);
        if (!effective_model.empty()) {
            chat_params["model"] = effective_model;
        }
        // Prompt library integration: an agents.invoke_with_prompt caller
        // passes promptId (+ optional promptArguments) alongside the agent
        // id.  Render before start_chat sees the payload so the library
        // prompt lands as a proper message.  Strip the fields afterwards so
        // downstream code can't double-apply them.
        const int32_t prompt_status = apply_prompt_source(chat_params);
        if (prompt_status != SAO_AI_EDITOR_OK) {
            return prompt_status;
        }
        chat_params.erase("promptId");
        chat_params.erase("promptArguments");
        const uint32_t timeout_ms = params.value("timeoutMs", 60'000U);
        const bool stream_requested = params.value("stream", false);
        std::string content;
        std::function<void(const Json&)> on_delta;
        if (stream_requested) {
            const std::string agent_id = agent.id;
            on_delta = [this, agent_id](const Json& event) {
                Json payload{{"agentId", agent_id}, {"event", event}};
                if (event.value("type", "") == "delta" &&
                    event.contains("content") &&
                    event["content"].is_string()) {
                    payload["content"] = event["content"];
                }
                emit("agent.delta", payload);
            };
        }
        const int32_t status = run_chat_sync(chat_params, timeout_ms, content,
                                             std::move(on_delta));
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        // Persist both the user turn and the assistant reply so the next
        // agents.invoke on this conversation sees the full transcript.
        if (!conversation_id.empty()) {
            Json append_result;
            std::lock_guard<std::mutex> guard(store_mutex_);
            conversations_.append(
                conversation_id,
                Json{{"role", "user"}, {"content", message}},
                append_result);
            conversations_.append(
                conversation_id,
                Json{{"role", "assistant"}, {"content", content}},
                append_result);
        }
        result = Json{{"agentId", agent.id},
                      {"agentName", agent.name},
                      {"content", std::move(content)},
                      {"model", effective_model}};
        if (!conversation_id.empty()) {
            result["conversationId"] = conversation_id;
        }
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_prompt(std::string_view method,
                                        const Json& params, Json& result) {
    if (method == "prompts.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        Json items = Json::array();
        for (const auto& prompt : prompt_registry_.list()) {
            items.push_back(prompt.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.get_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        PromptDefinition prompt;
        if (!prompt_registry_.get(params["id"].get<std::string>(), prompt)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = prompt.to_json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.save_def") {
        if (!params.contains("prompt") || !params["prompt"].is_object() ||
            !params.contains("scope") || !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        PromptDefinition prompt =
            PromptDefinition::from_json(params["prompt"]);
        const std::string scope_key = params["scope"].get<std::string>();
        std::string scope;
        std::string plugin_id;
        if (scope_key == "system" || scope_key == "workspace") {
            scope = scope_key;
        } else if (scope_key.rfind("plugin:", 0) == 0) {
            scope = "plugin";
            plugin_id = scope_key.substr(7);
            if (!valid_simple_id(plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = prompt_registry_.save(prompt, scopes_, scope,
                                                      plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = prompt.to_json();
        result["scope"] = scope_key;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.delete_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = prompt_registry_.remove(
            params["id"].get<std::string>(), scopes_,
            params.value("scope", "workspace"), std::string{});
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"id", params["id"]}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.render") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        PromptDefinition prompt;
        if (!prompt_registry_.get(params["id"].get<std::string>(), prompt)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        const Json arguments = params.value("arguments", Json::object());
        if (!arguments.is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        result = Json{{"id", prompt.id},
                      {"content", prompt.render(arguments)}};
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::apply_prompt_source(Json& params) {
    if (!params.is_object() || !params.contains("promptId")) {
        return SAO_AI_EDITOR_OK;
    }
    const Json& id_field = params["promptId"];
    // Absent / null / empty-string promptId is a no-op — callers may send
    // the field unconditionally.  The runtime still strips it from the
    // forwarded params in the caller.
    if (!id_field.is_string() || id_field.get<std::string>().empty()) {
        return SAO_AI_EDITOR_OK;
    }
    const std::string id = id_field.get<std::string>();
    Json arguments = Json::object();
    if (params.contains("promptArguments")) {
        if (!params["promptArguments"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        arguments = params["promptArguments"];
    }
    {
        std::lock_guard<std::mutex> guard(store_mutex_);
        prompt_registry_.reload(scopes_);
    }
    PromptDefinition prompt;
    if (!prompt_registry_.get(id, prompt)) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const std::string rendered = prompt.render(arguments);
    // Match apply_system_prompt_source: prepend as system when the caller
    // has no system message yet; otherwise queue as a user turn so the
    // existing system message stays authoritative.
    const bool caller_has_system =
        params.contains("messages") && params["messages"].is_array() &&
        !params["messages"].empty() &&
        params["messages"][0].is_object() &&
        params["messages"][0].value("role", "") == "system";
    const std::string role = caller_has_system ? "user" : "system";
    Json prepended = Json::array();
    prepended.push_back(Json{{"role", role}, {"content", rendered}});
    Json existing = params.value("messages", Json::array());
    if (!existing.is_array()) {
        existing = Json::array();
    }
    for (const auto& message : existing) {
        prepended.push_back(message);
    }
    params["messages"] = std::move(prepended);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_extension(std::string_view method,
                                          const Json& params, Json& result) {
    if (extension_host_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (method == "extensions.configure_host") {
        return extension_host_->configure(params);
    }
    if (method == "extensions.list") {
        return extension_host_->list_extensions(result);
    }
    if (method == "extensions.register") {
        return extension_host_->register_extension(params, result);
    }
    if (method == "extensions.unregister") {
        if (!params.contains("extensionId") ||
            !params["extensionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return extension_host_->unregister_extension(
            params["extensionId"].get<std::string>(), result);
    }
    if (method == "extensions.activate") {
        if (!params.contains("extensionId") ||
            !params["extensionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return extension_host_->activate(
            params["extensionId"].get<std::string>(),
            params.value("timeoutMs", 15000U), result);
    }
    if (method == "extensions.deactivate") {
        if (!params.contains("extensionId") ||
            !params["extensionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return extension_host_->deactivate(
            params["extensionId"].get<std::string>(), result);
    }
    if (method == "extensions.execute_command") {
        if (!params.contains("command") || !params["command"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return extension_host_->execute_command(
            params["command"].get<std::string>(),
            params.value("arguments", Json::array()),
            params.value("timeoutMs", 15000U), result);
    }
    if (method == "extensions.snapshot") {
        result = extension_host_->snapshot();
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_extension_call(std::string_view method,
                                               const Json& params,
                                               Json& result) {
    // vscode.workspace.* — file surface goes through the built-in tools
    // registry so writes still respect the workspace boundary + permission
    // model.
    if (method == "vscode.workspace.readTextDocument") {
        const Json arguments{{"path", params.value("path", std::string{})},
                              {"startLine", params.value("startLine", 0)},
                              {"endLine", params.value("endLine", 0)}};
        return tools_.execute("agent", "readFile", arguments, result);
    }
    if (method == "vscode.workspace.writeTextDocument") {
        Json arguments = params;
        arguments["confirmed"] = true;
        return tools_.execute("agent", "editFile", arguments, result);
    }
    if (method == "vscode.workspace.findFiles") {
        const Json arguments{
            {"path", params.value("path", std::string{"."})},
            {"pattern", params.value("pattern", std::string{"*"})},
            {"recursive", params.value("recursive", true)},
            {"limit", params.value("limit", 200)}};
        return tools_.execute("agent", "listFiles", arguments, result);
    }
    if (method == "vscode.workspace.textSearch") {
        const Json arguments{
            {"query", params.value("query", std::string{})},
            {"path", params.value("path", std::string{"."})},
            {"pattern", params.value("pattern", std::string{"*"})},
            {"regex", params.value("regex", false)},
            {"caseSensitive", params.value("caseSensitive", false)},
            {"limit", params.value("limit", 200)}};
        return tools_.execute("agent", "searchFiles", arguments, result);
    }
    if (method == "vscode.workspace.workspaceFolders") {
        result = Json::array({Json{{"index", 0},
                                    {"name", "workspace"},
                                    {"uri", "file:///" +
                                             wide_to_utf8(
                                                 scopes_.workspace_root().native())}}});
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.workspace.getConfiguration") {
        Json merged;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = scopes_.load_merged_config(merged);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        const std::string section = params.value("section", std::string{});
        if (section.empty()) {
            result = std::move(merged);
        } else {
            const auto found = merged.find(section);
            result = found == merged.end() ? Json::object() : *found;
        }
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.workspace.updateConfiguration") {
        if (!params.contains("section") || !params["section"].is_string() ||
            !params.contains("value")) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        Json existing;
        (void)scopes_.load_scope_config("workspace", "", existing);
        existing[params["section"].get<std::string>()] = params["value"];
        return scopes_.save_scope_config("workspace", "", existing);
    }
    // vscode.window.* — surface messages/inputs as native events so the
    // host UI can render them.
    if (method == "vscode.window.showInformationMessage" ||
        method == "vscode.window.showWarningMessage" ||
        method == "vscode.window.showErrorMessage") {
        const std::string_view kind = method.substr(
            std::string_view("vscode.window.show").size());
        emit(std::string("vscode.window.") + std::string(kind),
             Json{{"message", params.value("message", std::string{})},
                  {"actions", params.value("actions", Json::array())}});
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.showQuickPick" ||
        method == "vscode.window.showInputBox") {
        emit(std::string("vscode.window.") + std::string(method.substr(
                 std::string_view("vscode.window.").size())),
             params);
        // No UI attached — fail closed with null.
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.createOutputChannel") {
        result = Json{{"channelId",
                       params.value("name", std::string{"default"})}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.appendOutput") {
        emit("vscode.window.output",
             Json{{"channelId", params.value("channelId", std::string{})},
                  {"text", params.value("text", std::string{})}});
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    // vscode.commands.executeCommand loops back through the extension host
    // if a Node-side command was registered.
    if (method == "vscode.commands.executeCommand") {
        if (extension_host_ == nullptr) {
            return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
        }
        return extension_host_->execute_command(
            params.value("command", std::string{}),
            params.value("arguments", Json::array()),
            params.value("timeoutMs", 15000U), result);
    }
    // vscode.languages.* — static shim identifying registered languages.
    if (method == "vscode.languages.getLanguages") {
        result = Json::array({"plaintext", "json", "javascript", "typescript",
                              "python", "cpp", "csharp", "go", "rust",
                              "markdown", "html", "css"});
        return SAO_AI_EDITOR_OK;
    }
    // sao.host.* — direct pass-through to native runtime methods so the
    // extension shim can lean on SAO's own JSON-RPC surface without going
    // through the parent process.
    if (method == "sao.host.dispatch") {
        Json request{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method",
                      params.value("method", std::string{})},
                     {"params", params.value("params", Json::object())}};
        Json response;
        const int32_t status = dispatch(request, response);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "dispatch failed"}};
            return status;
        }
        if (response.contains("error")) {
            result = response["error"];
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        result = response.value("result", Json::object());
        return SAO_AI_EDITOR_OK;
    }
    if (method == "sao.host.log") {
        emit("host.log", params);
        result = Json(nullptr);
        return SAO_AI_EDITOR_OK;
    }
    result = Json{{"message", "unsupported extension method"}};
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::dispatch_auth(std::string_view method,
                                     const Json& params, Json& result) {
    if (auth_flow_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    if (method == "auth.begin_device_flow") {
        DeviceFlowState state;
        const int32_t status = auth_flow_->begin(params, state);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"flowId", state.flow_id},
                      {"providerId", state.provider_id},
                      {"userCode", state.user_code},
                      {"verificationUri", state.verification_uri},
                      {"verificationUriComplete",
                       state.verification_uri_complete},
                      {"intervalSeconds", state.interval_seconds},
                      {"expiresAtUnixMs", state.expires_at_unix_ms},
                      {"status", state.status}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "auth.poll_device_flow") {
        if (!params.contains("flowId") || !params["flowId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->poll(params["flowId"].get<std::string>(), result);
    }
    if (method == "auth.status_device_flow") {
        if (!params.contains("flowId") || !params["flowId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->status(params["flowId"].get<std::string>(),
                                  result);
    }
    if (method == "auth.cancel_device_flow") {
        if (!params.contains("flowId") || !params["flowId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->cancel(params["flowId"].get<std::string>(),
                                  result);
    }
    if (method == "auth.store_token") {
        if (!params.contains("providerId") ||
            !params["providerId"].is_string() ||
            !params.contains("token") || !params["token"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->store_token(
            params["providerId"].get<std::string>(), params["token"], result);
    }
    if (method == "auth.load_token") {
        if (!params.contains("providerId") ||
            !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->load_token(
            params["providerId"].get<std::string>(), result);
    }
    if (method == "auth.revoke_token") {
        if (!params.contains("providerId") ||
            !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->revoke(params["providerId"].get<std::string>(),
                                  result);
    }
    if (method == "auth.refresh_token") {
        if (!params.contains("providerId") ||
            !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        return auth_flow_->refresh(params["providerId"].get<std::string>(),
                                   result);
    }
    if (method == "auth.get_access_token") {
        if (!params.contains("providerId") ||
            !params["providerId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int64_t leeway = params.value(
            "expiryLeewaySeconds", int64_t{60});
        return auth_flow_->get_access_token(
            params["providerId"].get<std::string>(), leeway, result);
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::run_chat_sync(const Json& params, uint32_t timeout_ms,
                                     std::string& out_content,
                                     std::function<void(const Json&)> on_delta) {
    Json provider;
    std::string api_key;
    int32_t status = resolve_provider(params, provider, api_key);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string model =
        params.value("model", provider.value("model", std::string{}));
    if (model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json messages = params.value("messages", Json::array());
    if (!messages.is_array() || messages.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Force stream=true whenever the caller supplied an on_delta callback so
    // upper layers (workflow / agents.invoke) can observe intermediate tokens
    // without having to explicitly set stream in params.
    const bool stream = params.value("stream", false) ||
                        static_cast<bool>(on_delta);
    Json body{{"model", model},
              {"messages", std::move(messages)},
              {"stream", stream}};
    for (const std::string_view field : {"temperature", "max_tokens", "tools",
                                          "tool_choice", "response_format"}) {
        if (params.contains(field)) {
            body[std::string(field)] = params[field];
        }
    }
    Json provider_with_key = provider;
    if (!api_key.empty()) {
        provider_with_key["apiKey"] = api_key;
    }
    ProviderRoute route = normalise_provider(provider_with_key, model);
    if (route.endpoint.empty() && route.type == "openai") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    ProviderRequest provider_request;
    const int32_t build_status =
        build_provider_request(route, body, provider_request);
    if (build_status != SAO_AI_EDITOR_OK) {
        return build_status;
    }
    HttpChatRequest request{};
    request.endpoint = provider_request.endpoint;
    request.api_key = route.api_key;
    request.authorization = provider_request.authorization;
    request.extra_headers = provider_request.extra_headers;
    request.request_json = provider_request.body_json;
    request.provider_type = route.type;
    request.timeout_ms = timeout_ms;
    request.stream = stream;
    ChatCancellation cancellation;
    std::string streamed;
    Json transport_result;
    const int32_t chat_status = perform_openai_chat(
        request, cancellation,
        [&](const Json& event) {
            if (event.value("type", "") == "delta" &&
                event.contains("content") && event["content"].is_string()) {
                streamed += event["content"].get<std::string>();
                if (on_delta) {
                    on_delta(event);
                }
            } else if (on_delta) {
                // Forward non-delta events (message_delta, tool_delta, done,
                // etc.) so the caller can observe stream completion / usage.
                on_delta(event);
            }
        },
        transport_result);
    if (chat_status != SAO_AI_EDITOR_OK) {
        return chat_status;
    }
    out_content = streamed.empty()
        ? transport_result.value("content", std::string{})
        : std::move(streamed);
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_workflow(std::string_view method,
                                          const Json& params, Json& result) {
    if (method == "workflows.reload") {
        std::lock_guard<std::mutex> guard(store_mutex_);
        workflow_registry_.reload(scopes_);
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        Json items = Json::array();
        for (const auto& definition : workflow_registry_.list()) {
            items.push_back(definition.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.get_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(params["id"].get<std::string>(),
                                    definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = definition.to_json();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.save_def") {
        if (!params.contains("workflow") || !params["workflow"].is_object() ||
            !params.contains("scope") || !params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WorkflowDefinition definition =
            WorkflowDefinition::from_json(params["workflow"]);
        const std::string scope_key = params["scope"].get<std::string>();
        std::string scope;
        std::string plugin_id;
        if (scope_key == "system" || scope_key == "workspace") {
            scope = scope_key;
        } else if (scope_key.rfind("plugin:", 0) == 0) {
            scope = "plugin";
            plugin_id = scope_key.substr(7);
            if (!valid_simple_id(plugin_id)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        } else {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = workflow_registry_.save(definition, scopes_,
                                                        scope, plugin_id);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = definition.to_json();
        result["scope"] = scope_key;
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.delete_def") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> guard(store_mutex_);
        const int32_t status = workflow_registry_.remove(
            params["id"].get<std::string>(), scopes_,
            params.value("scope", "workspace"), std::string{});
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"id", params["id"]}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.run") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(params["id"].get<std::string>(),
                                    definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        Json provider;
        std::string api_key;
        int32_t status = resolve_provider(params, provider, api_key);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        if (!api_key.empty()) {
            provider["apiKey"] = api_key;
        }
        const std::string model =
            params.value("model", provider.value("model", std::string{}));
        if (model.empty() || provider.value("endpoint", "").empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const uint32_t chat_timeout = params.value("timeoutMs", 60'000U);
        Json input = params.contains("input") ? params["input"]
                                              : params.value("inputs",
                                                              Json::object());
        static std::atomic<uint64_t> execution_counter{0};
        auto execution = std::make_shared<WorkflowExecution>(
            "wf-" + std::to_string(GetTickCount64()) + "-" +
                std::to_string(execution_counter.fetch_add(
                    1, std::memory_order_relaxed)),
            std::move(definition), std::move(input));
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            workflow_executions_[execution->id()] = execution;
        }
        execution->start(*this, std::move(provider), model, chat_timeout);
        result = Json{{"executionId", execution->id()},
                      {"status", "running"}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.status") {
        if (!params.contains("executionId") ||
            !params["executionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::shared_ptr<WorkflowExecution> execution;
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            const auto found = workflow_executions_.find(
                params["executionId"].get<std::string>());
            if (found != workflow_executions_.end()) {
                execution = found->second;
            }
        }
        if (!execution) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = execution->snapshot();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.pause" || method == "workflows.resume" ||
        method == "workflows.cancel" || method == "workflows.confirm") {
        if (!params.contains("executionId") ||
            !params["executionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::shared_ptr<WorkflowExecution> execution;
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            const auto found = workflow_executions_.find(
                params["executionId"].get<std::string>());
            if (found != workflow_executions_.end()) {
                execution = found->second;
            }
        }
        if (!execution) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (method == "workflows.pause") {
            execution->request_pause();
            result = Json{{"ok", true}};
            return SAO_AI_EDITOR_OK;
        }
        if (method == "workflows.resume") {
            execution->request_resume(
                params.value("humanInput", std::string{}));
            result = Json{{"ok", true}};
            return SAO_AI_EDITOR_OK;
        }
        if (method == "workflows.cancel") {
            execution->request_cancel();
            result = Json{{"ok", true}};
            return SAO_AI_EDITOR_OK;
        }
        // confirm
        const int32_t confirm_status = execution->confirm(
            params.value("stepId", std::string{}),
            params.value("approved", false),
            params.value("note", std::string{}));
        if (confirm_status != SAO_AI_EDITOR_OK) {
            return confirm_status;
        }
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflows.gc") {
        // Reap completed executions to bound memory.
        std::lock_guard<std::mutex> guard(workflow_mutex_);
        Json removed = Json::array();
        for (auto iterator = workflow_executions_.begin();
             iterator != workflow_executions_.end();) {
            const Json snapshot = iterator->second->snapshot();
            const std::string status = snapshot.value("status", "");
            if (status == "completed" || status == "cancelled" ||
                status == "failed") {
                iterator->second->join();
                removed.push_back(iterator->first);
                iterator = workflow_executions_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        result = Json{{"removed", std::move(removed)}};
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::resolve_provider(const Json& params,
                                        Json& provider,
                                        std::string& api_key) {
    if (params.contains("provider")) {
        if (!params["provider"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        provider = params["provider"];
        if (provider.contains("apiKey") && provider["apiKey"].is_string()) {
            api_key = provider["apiKey"].get<std::string>();
        }
    } else {
        const std::string provider_id = params.value("providerId", "");
        if (!valid_simple_id(provider_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        Json registry;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t status = scopes_.load_registry(
                "providers", Json::array(), registry);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        for (const auto& item : registry) {
            if (item.is_object() && item.value("id", "") == provider_id) {
                provider = item;
                break;
            }
        }
        if (provider.is_null()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (secrets_ != nullptr) {
            std::string secret_value;
            const int32_t secret_status = secrets_->get(
                "provider/" + provider_id + "/apiKey", secret_value);
            if (secret_status == SAO_AI_EDITOR_OK) {
                api_key = std::move(secret_value);
            } else if (secret_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return secret_status;
            }
        }
    }
    if (api_key.empty() && provider.contains("apiKeyEnv") &&
        provider["apiKeyEnv"].is_string()) {
        api_key = environment_value(provider["apiKeyEnv"].get<std::string>());
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::start_chat(const Json& params, Json& result) {
    Json provider;
    std::string api_key;
    int32_t status = resolve_provider(params, provider, api_key);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    const std::string endpoint = provider.value("endpoint", "");
    const std::string model = params.value("model", provider.value("model", ""));
    if (endpoint.empty() || model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json messages = params.value("messages", Json::array());
    const std::string conversation_id = params.value("conversationId", "");
    if (messages.empty() && !conversation_id.empty()) {
        Json conversation;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            status = conversations_.get(conversation_id, conversation);
        }
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        messages = conversation.value("messages", Json::array());
    }
    if (!messages.is_array() || messages.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const bool stream = params.value("stream", true);
    Json body{{"model", model}, {"messages", std::move(messages)},
              {"stream", stream}};
    for (const std::string_view field : {"temperature", "max_tokens", "tools",
                                         "tool_choice", "response_format"}) {
        if (params.contains(field)) {
            body[std::string(field)] = params[field];
        }
    }
    Json provider_with_key = provider;
    if (!api_key.empty()) {
        provider_with_key["apiKey"] = api_key;
    }
    ProviderRoute route = normalise_provider(provider_with_key, model);
    if (route.endpoint.empty()) {
        route.endpoint = endpoint;
    }
    ProviderRequest provider_request;
    const int32_t build_status =
        build_provider_request(route, body, provider_request);
    if (build_status != SAO_AI_EDITOR_OK) {
        return build_status;
    }
    HttpChatRequest request{};
    request.endpoint = provider_request.endpoint;
    request.api_key = route.api_key;
    request.authorization = provider_request.authorization;
    request.extra_headers = provider_request.extra_headers;
    request.request_json = provider_request.body_json;
    request.provider_type = route.type;
    request.timeout_ms = params.value("timeoutMs", 60'000U);
    request.stream = stream;
    auto run = std::make_shared<RunState>();
    run->id = new_run_id();
    run->cancellation = std::make_shared<ChatCancellation>();
    std::shared_ptr<RunState> stale_run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++generation_;
        run->generation = generation_;
        if (!active_run_id_.empty()) {
            const auto active = runs_.find(active_run_id_);
            if (active != runs_.end() && active->second->status == "running") {
                stale_run = active->second;
                stale_run->status = "stale";
            }
        }
        active_run_id_ = run->id;
        runs_[run->id] = run;
    }
    if (stale_run) {
        stale_run->cancellation->cancel();
        emit("run.stale", Json{{"reason", "superseded"}}, stale_run->id);
    }
    try {
        run->worker = std::thread(&NativeRuntime::execute_chat, this, run,
                                  std::move(request), conversation_id);
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        runs_.erase(run->id);
        if (active_run_id_ == run->id) {
            active_run_id_.clear();
        }
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
    result = Json{{"accepted", true}, {"runId", run->id}, {"status", "running"}};
    emit("run.started", Json{{"model", model}, {"stream", stream}}, run->id);
    return SAO_AI_EDITOR_OK;
}

bool NativeRuntime::is_current(const std::shared_ptr<RunState>& run) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return run->generation == generation_ && run->status == "running";
}

void NativeRuntime::execute_chat(const std::shared_ptr<RunState>& run,
                                 HttpChatRequest request,
                                 std::string conversation_id) {
    try {
    Json transport_result;
    std::string streamed_content;
    const int32_t status = perform_openai_chat(
        request, *run->cancellation,
        [&](const Json& event) {
            if (!is_current(run)) {
                return;
            }
            if (event.value("type", "") == "delta" &&
                event.contains("content") && event["content"].is_string()) {
                streamed_content += event["content"].get<std::string>();
            }
            emit("chat.delta", event, run->id);
        },
        transport_result);

    std::string final_state;
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current = run->generation == generation_ && run->status == "running";
        if (run->status == "stale") {
            final_state = "stale";
        } else if (status == SAO_AI_EDITOR_ERR_CANCELLED ||
                   run->cancellation->cancelled()) {
            final_state = "cancelled";
            run->status = final_state;
        } else if (status == SAO_AI_EDITOR_OK) {
            final_state = "completed";
            run->status = final_state;
            run->result = transport_result;
            if (!streamed_content.empty()) {
                run->result["content"] = streamed_content;
            }
        } else {
            final_state = "failed";
            run->status = final_state;
            run->result = transport_result;
            run->error = status_message(status);
        }
        if (active_run_id_ == run->id) {
            active_run_id_.clear();
        }
    }
    if (!current && final_state == "stale") {
        return;
    }
    if (final_state == "completed" && !conversation_id.empty()) {
        Json appended;
        const std::string content = !streamed_content.empty()
            ? streamed_content
            : transport_result.value("content", "");
        if (!content.empty()) {
            std::lock_guard<std::mutex> lock(store_mutex_);
            conversations_.append(
                conversation_id,
                Json{{"role", "assistant"}, {"content", content}}, appended);
        }
    }
    if (final_state == "completed") {
        // Surface latency/token-throughput separately from the completion
        // payload so callers that only care about metrics (dashboards,
        // logging) can subscribe to chat.metrics without pulling the full
        // run.completed body.  Missing metrics (e.g. transport short-circuit)
        // simply skip the emit — never blocks run.completed.
        if (run->result.is_object() && run->result.contains("metrics") &&
            run->result["metrics"].is_object()) {
            Json metrics_payload = run->result["metrics"];
            metrics_payload["runId"] = run->id;
            emit("chat.metrics", metrics_payload, run->id);
        }
        emit("run.completed", run->result, run->id);
    } else if (final_state == "cancelled") {
        emit("run.cancelled", Json::object(), run->id);
    } else if (final_state == "failed") {
        emit("run.failed",
             Json{{"status", status}, {"message", run->error},
                  {"details", run->result}},
             run->id);
    }
    } catch (...) {
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stale = run->status == "stale";
            if (!stale) {
                run->status = "failed";
                run->error = "worker exception";
            }
            if (active_run_id_ == run->id) {
                active_run_id_.clear();
            }
        }
        if (!stale) {
            try {
                emit("run.failed",
                     Json{{"status", SAO_AI_EDITOR_ERR_PROTOCOL},
                          {"message", "worker exception"}},
                     run->id);
            } catch (...) {
            }
        }
    }
}

int32_t NativeRuntime::cancel_run(const Json& params, Json& result) {
    const std::string id = params.value("runId", "");
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::shared_ptr<RunState> run;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto found = runs_.find(id);
        if (found == runs_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        run = found->second;
        if (run->status == "running") {
            run->status = "cancelling";
        }
    }
    run->cancellation->cancel();
    result = Json{{"runId", id}, {"cancelRequested", true}};
    return SAO_AI_EDITOR_OK;
}

namespace {

int32_t collect_mcp_output(int32_t query_status,
                           uint32_t required,
                           std::function<int32_t(char*, uint32_t, uint32_t*)>
                               drain,
                           Json& result) {
    if (query_status == SAO_AI_EDITOR_OK && required == 0) {
        result = Json::object();
        return SAO_AI_EDITOR_OK;
    }
    if (query_status != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return query_status;
    }
    std::string buffer(static_cast<size_t>(required) + 1U, '\0');
    uint32_t written = 0;
    const int32_t drain_status = drain(buffer.data(),
                                       static_cast<uint32_t>(buffer.size()),
                                       &written);
    if (drain_status != SAO_AI_EDITOR_OK) {
        return drain_status;
    }
    result = Json::parse(buffer.data(), buffer.data() + written, nullptr, false);
    if (result.is_discarded()) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
    return SAO_AI_EDITOR_OK;
}

}  // namespace

int32_t NativeRuntime::dispatch_mcp(std::string_view method, const Json& params,
                                    Json& result) {
    if (mcp_client_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    auto* raw = mcp_client_.get();
    if (method == "mcp.register_server") {
        const std::string config = dump_json(params);
        const int32_t status = sao_ai_editor_mcp_client_register(
            raw, config.data(), static_cast<uint32_t>(config.size()));
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"name", params.value("name", "")}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.list_servers") {
        uint32_t required = 0;
        const int32_t query = sao_ai_editor_mcp_client_list_servers(
            raw, nullptr, 0, &required);
        Json items;
        const int32_t status = collect_mcp_output(
            query, required,
            [raw](char* output, uint32_t capacity, uint32_t* out_length) {
                return sao_ai_editor_mcp_client_list_servers(raw, output,
                                                             capacity,
                                                             out_length);
            },
            items);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"items", items.is_array() ? items : Json::array()}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.list_tools" || method == "mcp.list_prompts" ||
        method == "mcp.list_resources") {
        auto invoker = method == "mcp.list_tools"
            ? &sao_ai_editor_mcp_client_list_tools
            : (method == "mcp.list_prompts"
                   ? &sao_ai_editor_mcp_client_list_prompts
                   : &sao_ai_editor_mcp_client_list_resources);
        uint32_t required = 0;
        const int32_t query = invoker(raw, nullptr, 0, &required);
        Json items;
        const int32_t status = collect_mcp_output(
            query, required,
            [raw, invoker](char* output, uint32_t capacity,
                           uint32_t* out_length) {
                return invoker(raw, output, capacity, out_length);
            },
            items);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"items", items.is_array() ? items : Json::array()}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.call_tool" || method == "mcp.read_resource" ||
        method == "mcp.get_prompt") {
        const std::string request = dump_json(params);
        auto invoker = method == "mcp.call_tool"
            ? &sao_ai_editor_mcp_client_call_tool
            : (method == "mcp.read_resource"
                   ? &sao_ai_editor_mcp_client_read_resource
                   : &sao_ai_editor_mcp_client_get_prompt);
        uint32_t required = 0;
        int32_t query =
            invoker(raw, request.data(),
                    static_cast<uint32_t>(request.size()), nullptr, 0,
                    &required);
        if (query == SAO_AI_EDITOR_OK && required == 0) {
            result = Json::object();
            return SAO_AI_EDITOR_OK;
        }
        if (query != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
            return query;
        }
        std::string buffer(static_cast<size_t>(required) + 1U, '\0');
        uint32_t written = 0;
        const int32_t drain_status =
            invoker(raw, nullptr, 0, buffer.data(),
                    static_cast<uint32_t>(buffer.size()), &written);
        if (drain_status != SAO_AI_EDITOR_OK) {
            return drain_status;
        }
        result = Json::parse(buffer.data(), buffer.data() + written, nullptr,
                             false);
        if (result.is_discarded()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.close_server") {
        const std::string name = params.value("name", "");
        const int32_t status = sao_ai_editor_mcp_client_close(
            raw, name.empty() ? nullptr : name.c_str());
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        result = Json{{"ok", true}, {"name", name}};
        return SAO_AI_EDITOR_OK;
    }
    return SAO_AI_EDITOR_ERR_NOT_FOUND;
}

int32_t NativeRuntime::collect_mcp_openai_tools(const Json& mcp_server_filter,
                                                Json& out_tools) {
    out_tools = Json::array();
    if (mcp_client_ == nullptr) {
        // No MCP registry — treat as "no MCP tools available"; callers may
        // still layer extraTools on top.
        return SAO_AI_EDITOR_OK;
    }
    // Build a normalized filter set.  Empty filter (missing / null / empty
    // array) means "include tools from every registered server".
    std::vector<std::string> filter;
    bool filter_enabled = false;
    if (mcp_server_filter.is_array()) {
        for (const auto& entry : mcp_server_filter) {
            if (entry.is_string()) {
                const std::string name = entry.get<std::string>();
                if (!name.empty()) {
                    filter.push_back(name);
                    filter_enabled = true;
                }
            }
        }
    }
    Json tools_response;
    const int32_t status = dispatch_mcp("mcp.list_tools", Json::object(),
                                        tools_response);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    if (!tools_response.is_object() || !tools_response.contains("items") ||
        !tools_response["items"].is_array()) {
        return SAO_AI_EDITOR_OK;
    }
    for (const auto& item : tools_response["items"]) {
        if (!item.is_object()) {
            continue;
        }
        const std::string server = item.value("server", std::string{});
        const std::string tool_name = item.value("name", std::string{});
        if (server.empty() || tool_name.empty()) {
            continue;
        }
        if (filter_enabled) {
            if (std::find(filter.begin(), filter.end(), server) ==
                filter.end()) {
                continue;
            }
        }
        Json function_body{{"name", "mcp__" + server + "__" + tool_name}};
        if (item.contains("description") &&
            item["description"].is_string()) {
            function_body["description"] = item["description"];
        }
        Json parameters;
        if (item.contains("inputSchema") && item["inputSchema"].is_object()) {
            parameters = item["inputSchema"];
        } else {
            parameters = Json{{"type", "object"},
                              {"properties", Json::object()}};
        }
        function_body["parameters"] = std::move(parameters);
        out_tools.push_back(Json{{"type", "function"},
                                 {"function", std::move(function_body)}});
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::apply_system_prompt_source(Json& params) {
    if (!params.is_object() || !params.contains("systemPromptSource")) {
        return SAO_AI_EDITOR_OK;
    }
    const Json source = params["systemPromptSource"];
    // Absent / null / empty-object systemPromptSource is a no-op — callers may
    // send the field unconditionally.  The runtime still strips it from the
    // forwarded params in the caller.
    if (!source.is_object() || source.empty()) {
        return SAO_AI_EDITOR_OK;
    }
    if (!source.contains("server") || !source["server"].is_string() ||
        !source.contains("name") || !source["name"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    if (mcp_client_ == nullptr) {
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    Json request{{"server", source["server"]}, {"name", source["name"]}};
    if (source.contains("arguments") && source["arguments"].is_object()) {
        request["arguments"] = source["arguments"];
    } else {
        request["arguments"] = Json::object();
    }
    if (source.contains("timeoutMs") &&
        source["timeoutMs"].is_number_integer()) {
        request["timeoutMs"] = source["timeoutMs"];
    }
    const std::string request_json = dump_json(request);
    auto* raw = mcp_client_.get();
    uint32_t required = 0;
    int32_t query = sao_ai_editor_mcp_client_get_prompt(
        raw, request_json.data(),
        static_cast<uint32_t>(request_json.size()), nullptr, 0, &required);
    Json prompt_result;
    if (query == SAO_AI_EDITOR_OK && required == 0) {
        prompt_result = Json::object();
    } else if (query != SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL) {
        return query;
    } else {
        std::string buffer(static_cast<size_t>(required) + 1U, '\0');
        uint32_t written = 0;
        const int32_t drain_status = sao_ai_editor_mcp_client_get_prompt(
            raw, nullptr, 0, buffer.data(),
            static_cast<uint32_t>(buffer.size()), &written);
        if (drain_status != SAO_AI_EDITOR_OK) {
            return drain_status;
        }
        prompt_result = Json::parse(buffer.data(), buffer.data() + written,
                                    nullptr, false);
        if (prompt_result.is_discarded() || !prompt_result.is_object()) {
            return SAO_AI_EDITOR_ERR_PROTOCOL;
        }
    }
    // Normalize the MCP prompts/get shape into OpenAI-flavour chat messages.
    // MCP content is either a plain string, a single {type:"text",text:...}
    // object, or an array of such objects; downstream chat.run expects
    // {"role","content"} with content as a string.
    Json prepended = Json::array();
    bool prompt_has_system = false;
    if (prompt_result.contains("messages") &&
        prompt_result["messages"].is_array()) {
        for (const auto& message : prompt_result["messages"]) {
            if (message.is_object() &&
                message.value("role", std::string{}) == "system") {
                prompt_has_system = true;
                break;
            }
        }
    }
    const bool caller_has_system =
        params.contains("messages") && params["messages"].is_array() &&
        !params["messages"].empty() &&
        params["messages"][0].is_object() &&
        params["messages"][0].value("role", "") == "system";
    // Only surface the prompt description as a system header when neither
    // the prompt messages nor the caller already provide one — otherwise
    // it duplicates the intent of the existing system message.
    if (!caller_has_system && !prompt_has_system &&
        prompt_result.contains("description") &&
        prompt_result["description"].is_string()) {
        const std::string description =
            prompt_result["description"].get<std::string>();
        if (!description.empty()) {
            prepended.push_back(Json{{"role", "system"},
                                     {"content", description}});
        }
    }
    if (prompt_result.contains("messages") &&
        prompt_result["messages"].is_array()) {
        for (const auto& message : prompt_result["messages"]) {
            if (!message.is_object()) {
                continue;
            }
            const std::string role = message.value("role", std::string{});
            if (role.empty()) {
                continue;
            }
            std::string text;
            const auto& content = message.contains("content")
                                      ? message["content"]
                                      : Json{};
            if (content.is_string()) {
                text = content.get<std::string>();
            } else if (content.is_object() &&
                       content.value("type", "") == "text" &&
                       content.contains("text") &&
                       content["text"].is_string()) {
                text = content["text"].get<std::string>();
            } else if (content.is_array()) {
                for (const auto& part : content) {
                    if (part.is_object() &&
                        part.value("type", "") == "text" &&
                        part.contains("text") && part["text"].is_string()) {
                        if (!text.empty()) {
                            text.push_back('\n');
                        }
                        text.append(part["text"].get<std::string>());
                    }
                }
            }
            if (text.empty()) {
                continue;
            }
            prepended.push_back(Json{{"role", role}, {"content", text}});
        }
    }
    if (!prepended.empty()) {
        Json existing = params.value("messages", Json::array());
        if (!existing.is_array()) {
            existing = Json::array();
        }
        for (const auto& message : existing) {
            prepended.push_back(message);
        }
        params["messages"] = std::move(prepended);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::start_chat_with_mcp(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json mcp_tools;
    const int32_t collect_status = collect_mcp_openai_tools(
        params.value("mcpServers", Json{}), mcp_tools);
    if (collect_status != SAO_AI_EDITOR_OK) {
        return collect_status;
    }
    // Build combined tools list: MCP tools first, then user-provided
    // extraTools.  Empty combined list -> do not set params.tools so the
    // request body stays identical to chat.run without tools.
    Json combined_tools = std::move(mcp_tools);
    if (params.contains("extraTools") && params["extraTools"].is_array()) {
        for (const auto& tool : params["extraTools"]) {
            combined_tools.push_back(tool);
        }
    }
    Json forwarded = params;
    // Render systemPromptSource (MCP prompts/get) into forwarded.messages
    // before stripping the source field.  On error we surface the failure
    // instead of silently dropping the source.
    const int32_t prompt_status = apply_system_prompt_source(forwarded);
    if (prompt_status != SAO_AI_EDITOR_OK) {
        return prompt_status;
    }
    // Also honour prompt-library references so callers can compose an MCP
    // prompt source with a stored template in the same request.
    const int32_t library_status = apply_prompt_source(forwarded);
    if (library_status != SAO_AI_EDITOR_OK) {
        return library_status;
    }
    forwarded.erase("mcpServers");
    forwarded.erase("extraTools");
    forwarded.erase("systemPromptSource");
    forwarded.erase("promptId");
    forwarded.erase("promptArguments");
    if (!combined_tools.empty()) {
        forwarded["tools"] = std::move(combined_tools);
    }
    return start_chat(forwarded, result);
}

int32_t NativeRuntime::agent_invoke_with_mcp(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json mcp_tools;
    const int32_t collect_status = collect_mcp_openai_tools(
        params.value("mcpServers", Json{}), mcp_tools);
    if (collect_status != SAO_AI_EDITOR_OK) {
        return collect_status;
    }
    Json combined_tools = std::move(mcp_tools);
    if (params.contains("extraTools") && params["extraTools"].is_array()) {
        for (const auto& tool : params["extraTools"]) {
            combined_tools.push_back(tool);
        }
    }
    Json forwarded = params;
    const int32_t prompt_status = apply_system_prompt_source(forwarded);
    if (prompt_status != SAO_AI_EDITOR_OK) {
        return prompt_status;
    }
    forwarded.erase("mcpServers");
    forwarded.erase("extraTools");
    forwarded.erase("systemPromptSource");
    if (!combined_tools.empty()) {
        forwarded["tools"] = std::move(combined_tools);
    }
    return dispatch_agent("agents.invoke", forwarded, result);
}

int32_t NativeRuntime::run_status(const Json& params, Json& result) {
    const std::string id = params.value("runId", "");
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto found = runs_.find(id);
    if (found == runs_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    const auto& run = found->second;
    result = Json{{"runId", id},
                  {"status", run->status},
                  {"result", run->result},
                  {"error", run->error}};
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native

struct SaoAiEditorRuntime {
    std::unique_ptr<sao::ai_editor::native::NativeRuntime> implementation;
    std::mutex dispatch_mutex;
    std::mutex event_mutex;
    std::string pending_dispatch;
    std::string pending_event;
};

namespace {

int32_t parse_runtime_options(const SaoAiEditorRuntimeConfig& config,
                              sao::ai_editor::native::RuntimeOptions& options) {
    using namespace sao::ai_editor::native;
    if (config.struct_size < sizeof(SaoAiEditorRuntimeConfig) ||
        config.workspace_root_utf8 == nullptr ||
        !valid_utf8(config.workspace_root_utf8)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    options.workspace_root = config.workspace_root_utf8;
    if (config.system_root_utf8 != nullptr && config.system_root_utf8[0] != '\0') {
        if (!valid_utf8(config.system_root_utf8)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        options.system_root = config.system_root_utf8;
    } else {
        std::wstring home;
        DWORD required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
        if (required == 0) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        home.resize(required);
        required = GetEnvironmentVariableW(L"USERPROFILE", home.data(), required);
        if (required == 0 || required >= home.size()) {
            return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
        }
        home.resize(required);
        options.system_root = wide_to_utf8(
            (std::filesystem::path(home) / L".sao").native());
    }
    if (config.plugin_roots_json_utf8 != nullptr &&
        config.plugin_roots_json_utf8[0] != '\0') {
        if (!valid_utf8(config.plugin_roots_json_utf8)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json plugins = Json::parse(config.plugin_roots_json_utf8);
        if (!plugins.is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        for (size_t index = 0; index < plugins.size(); ++index) {
            std::string id;
            std::string path;
            if (plugins[index].is_string()) {
                id = "plugin-" + std::to_string(index + 1);
                path = plugins[index].get<std::string>();
            } else if (plugins[index].is_object() &&
                       plugins[index].contains("path") &&
                       plugins[index]["path"].is_string()) {
                id = plugins[index].value(
                    "id", "plugin-" + std::to_string(index + 1));
                path = plugins[index]["path"].get<std::string>();
            } else {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (!valid_simple_id(id) || path.empty() || !valid_utf8(path)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        }
        options.plugin_roots_json = plugins.dump();
    }
    options.maximum_file_bytes = config.max_file_bytes;
    options.maximum_search_results = config.max_search_results;
    return SAO_AI_EDITOR_OK;
}

}  // namespace

extern "C" SAO_AI_EDITOR_API uint32_t SAO_AI_EDITOR_CALL
sao_ai_editor_native_abi_version(void) {
    try {
        return SAO_AI_EDITOR_NATIVE_ABI_VERSION;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_create(const SaoAiEditorRuntimeConfig* config,
                             sao_ai_editor_runtime_t* out_handle) {
    try {
        if (config == nullptr || out_handle == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        *out_handle = nullptr;
        sao::ai_editor::native::RuntimeOptions options;
        const int32_t parsed = parse_runtime_options(*config, options);
        if (parsed != SAO_AI_EDITOR_OK) {
            return parsed;
        }
        auto handle = std::make_unique<SaoAiEditorRuntime>();
        handle->implementation =
            std::make_unique<sao::ai_editor::native::NativeRuntime>(
                std::move(options));
        const int32_t status = handle->implementation->initialize();
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        *out_handle = handle.release();
        return SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_dispatch(sao_ai_editor_runtime_t handle,
                               const void* request_json,
                               uint32_t request_len,
                               char* response_out,
                               uint32_t response_cap,
                               uint32_t* out_len) {
    try {
        if (handle == nullptr || handle->implementation == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if ((request_json == nullptr && request_len != 0) || out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(handle->dispatch_mutex);
        if (handle->pending_dispatch.empty()) {
            const auto input = std::string_view(
                static_cast<const char*>(request_json), request_len);
            if (input.empty() || input.size() >
                                     sao::ai_editor::native::kMaximumJsonBytes ||
                !sao::ai_editor::native::valid_utf8(input)) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            sao::ai_editor::native::Json request =
                sao::ai_editor::native::Json::parse(input);
            sao::ai_editor::native::Json response;
            const int32_t status =
                handle->implementation->dispatch(request, response);
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
            handle->pending_dispatch =
                sao::ai_editor::native::dump_json(response);
        } else if (request_len != 0) {
            return SAO_AI_EDITOR_ERR_BUSY;
        }
        const int32_t status = sao::ai_editor::native::copy_text_to_caller(
            handle->pending_dispatch, response_out, response_cap, out_len);
        if (status == SAO_AI_EDITOR_OK) {
            handle->pending_dispatch.clear();
        }
        return status;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_next_event(sao_ai_editor_runtime_t handle,
                                 char* event_out,
                                 uint32_t event_cap,
                                 uint32_t* out_len) {
    try {
        if (handle == nullptr || handle->implementation == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(handle->event_mutex);
        if (handle->pending_event.empty()) {
            const int32_t status =
                handle->implementation->next_event(0, handle->pending_event);
            if (status == SAO_AI_EDITOR_ERR_TIMEOUT) {
                *out_len = 0;
                if (event_out != nullptr && event_cap > 0) {
                    event_out[0] = '\0';
                }
                return SAO_AI_EDITOR_OK;
            }
            if (status != SAO_AI_EDITOR_OK) {
                return status;
            }
        }
        const int32_t status = sao::ai_editor::native::copy_text_to_caller(
            handle->pending_event, event_out, event_cap, out_len);
        if (status == SAO_AI_EDITOR_OK) {
            handle->pending_event.clear();
        }
        return status;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_cancel(sao_ai_editor_runtime_t handle,
                             const char* run_id_utf8) {
    try {
        if (handle == nullptr || handle->implementation == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (run_id_utf8 == nullptr ||
            !sao::ai_editor::native::valid_utf8(run_id_utf8)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        sao::ai_editor::native::Json request{
            {"jsonrpc", "2.0"}, {"id", 0}, {"method", "run.cancel"},
            {"params", {{"runId", run_id_utf8}}}};
        sao::ai_editor::native::Json response;
        const int32_t status =
            handle->implementation->dispatch(request, response);
        if (status != SAO_AI_EDITOR_OK) {
            return status;
        }
        return response.contains("error") ? SAO_AI_EDITOR_ERR_NOT_FOUND
                                          : SAO_AI_EDITOR_OK;
    } catch (...) {
        return SAO_AI_EDITOR_ERR_PROTOCOL;
    }
}

extern "C" SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL
sao_ai_editor_runtime_destroy(sao_ai_editor_runtime_t handle) {
    try {
        delete handle;
    } catch (...) {
    }
}
