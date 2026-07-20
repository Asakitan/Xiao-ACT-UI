#include "sao/ai_editor/ai_editor_native.h"

#include "chat_provider_router.h"
#include "native_runtime_internal.h"
#include "sha256_helper.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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

// Locate the on-disk workflow history directory for a scope.  Anchored to
// ScopeStore::history_root("<scope>") so we stay in sync with the
// chat_history layout convention — the sibling directory naming means new
// scope roots (system_root_ / workspace_scope_root_) get picked up
// automatically without exposing those private paths.
std::filesystem::path workflow_history_root(const ScopeStore& scopes,
                                             std::string_view scope) {
    const std::filesystem::path chat_history = scopes.history_root(scope);
    if (chat_history.empty()) {
        return {};
    }
    return chat_history.parent_path() / L"workflow_history";
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

// Convert a WebviewPanelState into the JSON shape the Node-side extension
// shim + tests consume.  Kept here rather than on the struct because the
// registry header intentionally has no nlohmann::json dependency beyond
// the opaque `Json extras` bag.
Json panel_state_to_json(const WebviewPanelState& state) {
    return Json{{"panelId", state.panel_id},
                {"viewType", state.view_type},
                {"title", state.title},
                {"visible", state.visible},
                {"disposed", state.disposed},
                {"htmlLength",
                 static_cast<int64_t>(state.html.size())},
                {"createdMs", state.created_ms},
                {"lastRevealMs", state.last_reveal_ms},
                {"lastPostMs", state.last_post_ms},
                {"messageSeq",
                 static_cast<int64_t>(state.message_seq)},
                {"options",
                 Json{{"enableScripts", state.options.enable_scripts},
                      {"retainContextWhenHidden",
                       state.options.retain_context_when_hidden},
                      {"viewColumn", state.options.view_column},
                      {"extras", state.options.extras}}},
                {"initialState", state.initial_state}};
}

}  // namespace

NativeRuntime::NativeRuntime(RuntimeOptions options)
        : options_(std::move(options)),
            conversations_(scopes_),
      tools_(scopes_, options.maximum_file_bytes,
             options.maximum_search_results),
      maximum_event_queue_(std::clamp(options.maximum_event_queue, 8U, 4096U)) {
    // Register the three built-in tool-result compressors + hand the registry
    // pointer to the tool registry.  Filters are shared_ptrs so the registry
    // can snapshot the vector without holding its mutex on every dispatch;
    // set_filter_registry takes a raw pointer because the tool registry lives
    // strictly within this runtime and never outlives it.  Order matters only
    // for the fired-id list — the LLM sees them in the order they compressed.
    filter_registry_.register_filter(make_list_files_folder_filter());
    filter_registry_.register_filter(make_search_files_collapse_filter());
    filter_registry_.register_filter(make_read_file_truncate_filter());
    tools_.set_filter_registry(&filter_registry_);
}

NativeRuntime::~NativeRuntime() {
    set_webview_post_message_handler({});
    extension_host_.reset();

    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        stopping_ = true;
    }
    event_ready_.notify_all();

    std::vector<std::shared_ptr<WorkflowExecution>> workflows;
    {
        std::lock_guard<std::mutex> lock(workflow_mutex_);
        workflows.reserve(workflow_executions_.size());
        for (const auto& [id, execution] : workflow_executions_) {
            (void)id;
            workflows.push_back(execution);
        }
    }
    for (const auto& execution : workflows) {
        execution->request_cancel();
    }

    std::vector<std::shared_ptr<RunState>> runs;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        runs.reserve(runs_.size());
        for (const auto& [id, run] : runs_) {
            (void)id;
            runs.push_back(run);
            run->cancellation->cancel();
        }
    }

    if (mcp_client_ != nullptr) {
        (void)sao_ai_editor_mcp_client_set_notification_forwarder(
            mcp_client_.get(), nullptr, nullptr);
        (void)sao_ai_editor_mcp_client_close(mcp_client_.get(), nullptr);
    }

    for (const auto& execution : workflows) {
        execution->join();
    }
    for (const auto& run : runs) {
        if (run->worker.joinable()) {
            run->worker.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(workflow_mutex_);
        workflow_executions_.clear();
    }
    mcp_client_.reset();
}

void NativeRuntime::set_webview_post_message_handler(
    WebviewPostMessageHandler handler) {
    std::lock_guard<std::mutex> guard(webview_bridge_mutex_);
    webview_post_message_handler_ = std::move(handler);
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
    if (method == "conversation.stats") {
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.stats(params.value("scope", "all"), result);
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
    if (method == "conversation.tag" || method == "conversation.untag") {
        // Both endpoints funnel into set_tags(add, remove) so we can keep the
        // "add and subtract in one call" primitive on the store while still
        // exposing the two verbs the UI wants.  Payload requires id + tags[]
        // for either method; anything else falls through to INVALID_ARGUMENT.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (!params.contains("tags") || !params["tags"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> incoming;
        incoming.reserve(params["tags"].size());
        for (const auto& tag : params["tags"]) {
            if (!tag.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(tag.get<std::string>());
        }
        const std::vector<std::string> add_tags =
            method == "conversation.tag" ? incoming
                                         : std::vector<std::string>{};
        const std::vector<std::string> remove_tags =
            method == "conversation.untag" ? incoming
                                           : std::vector<std::string>{};
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.set_tags(params["id"].get<std::string>(),
                                        add_tags, remove_tags, result);
    }
    if (method == "conversation.find_by_tag") {
        if (!params.contains("tags") || !params["tags"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> query_tags;
        query_tags.reserve(params["tags"].size());
        for (const auto& tag : params["tags"]) {
            if (!tag.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            query_tags.push_back(tag.get<std::string>());
        }
        const std::string scope = params.value("scope", std::string{"all"});
        const uint32_t limit = params.value("limit", 50U);
        const bool match_all = params.value("matchAll", false);
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.find_by_tag(query_tags, scope, limit,
                                            match_all, result);
    }
    if (method == "conversation.list_tags") {
        const std::string scope = params.value("scope", std::string{"all"});
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.list_tags_stats(scope, result);
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
    if (method == "conversation.merge") {
        if (!params.contains("sourceIds") ||
            !params["sourceIds"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> source_ids;
        source_ids.reserve(params["sourceIds"].size());
        for (const auto& item : params["sourceIds"]) {
            if (!item.is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            source_ids.push_back(item.get<std::string>());
        }
        const std::string title = params.value("title", std::string{});
        const std::string scope = params.value("scope",
                                                std::string{"workspace"});
        // "explicit" (default) preserves the caller's sourceIds order;
        // "savedAt" sorts by each source's savedAt ascending before concat.
        const std::string order_by = params.value("orderBy",
                                                    std::string{"explicit"});
        if (order_by != "explicit" && order_by != "savedAt") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json separator = params.contains("separator")
                                    ? params["separator"]
                                    : Json(nullptr);
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.merge(source_ids, title, scope, separator,
                                     order_by == "savedAt", result);
    }
    if (method == "conversation.split") {
        if (!params.contains("sourceId") ||
            !params["sourceId"].is_string() ||
            !params.contains("messageIndex") ||
            !params["messageIndex"].is_number_integer()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const int64_t message_index_signed =
            params["messageIndex"].get<int64_t>();
        if (message_index_signed < 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string source_id = params["sourceId"].get<std::string>();
        const std::string scope = params.value("scope",
                                                std::string{"workspace"});
        const bool keep_original = params.value("keepOriginal", false);
        // titles is optional; when present it must be a 2-element string
        // array [before, after].  Missing / partial entries fall back to
        // the store's default naming.
        std::string title_before;
        std::string title_after;
        if (params.contains("titles")) {
            if (!params["titles"].is_array() ||
                params["titles"].size() > 2) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const auto& titles = params["titles"];
            if (titles.size() >= 1 && titles[0].is_string()) {
                title_before = titles[0].get<std::string>();
            } else if (titles.size() >= 1 && !titles[0].is_null()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (titles.size() >= 2 && titles[1].is_string()) {
                title_after = titles[1].get<std::string>();
            } else if (titles.size() >= 2 && !titles[1].is_null()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.split(source_id,
                                     static_cast<size_t>(message_index_signed),
                                     title_before, title_after, scope,
                                     keep_original, result);
    }
    if (method == "conversation.compact") {
        // Fold the head of a long transcript into a single summary produced
        // by an LLM.  Params:
        //   id           - required conversation id.
        //   keepLast     - number of tail messages to keep intact (default 6).
        //   provider     - LLM provider config forwarded verbatim to
        //                  run_chat_sync (same shape as chat.run).
        //   model        - model id (defaults resolved by the provider).
        //   summaryPrompt (optional) - system message driving the summary;
        //                  a fixed default ships when omitted so callers can
        //                  cheaply "just compact this thing" without
        //                  configuring prompts.
        //   compactStrategy - "replace" (default) drops the early tail and
        //                     substitutes one system summary + keepLast
        //                     recent; "prepend" retains everything and only
        //                     glues the summary at the head.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string conversation_id =
            params["id"].get<std::string>();
        // Accept both integer and floating-point keepLast so JS callers
        // whose JSON serialisers turn `6` into `6.0` still hit the happy
        // path.  Signed compare catches negatives before the size_t cast.
        int64_t keep_last_signed = 6;
        if (params.contains("keepLast")) {
            if (!params["keepLast"].is_number()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            keep_last_signed = params["keepLast"].get<int64_t>();
        }
        if (keep_last_signed <= 0) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const size_t keep_last =
            static_cast<size_t>(keep_last_signed);
        const std::string strategy =
            params.value("compactStrategy", std::string{"replace"});
        if (strategy != "replace" && strategy != "prepend") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Load a snapshot of the conversation up front.  We deliberately
        // release store_mutex_ before the LLM call because run_chat_sync
        // performs a blocking HTTP round-trip and holding the store lock
        // through it would starve every other conversation.* method.
        Json conversation;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            const int32_t get_status =
                conversations_.get(conversation_id, conversation);
            if (get_status != SAO_AI_EDITOR_OK) {
                return get_status;
            }
        }
        if (!conversation.contains("messages") ||
            !conversation["messages"].is_array()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const Json& all_messages = conversation["messages"];
        const size_t original_count = all_messages.size();
        // Nothing to summarise - short-circuit the LLM entirely.  We still
        // route through compact() so the response shape is identical to a
        // "did work" call; the store's noop path emits summary:"" and the
        // savedAt is left untouched.
        if (original_count <= keep_last) {
            std::lock_guard<std::mutex> lock(store_mutex_);
            return conversations_.compact(conversation_id, keep_last,
                                          std::string_view{},
                                          strategy, result);
        }
        const size_t drop_count = original_count - keep_last;
        // Roll the early messages into a single "transcript" string.  Using
        // one user turn (rather than replaying the historic roles) keeps
        // the summary prompt cheap and avoids accidentally letting the
        // model treat old assistant text as authoritative context.
        std::string transcript;
        for (size_t index = 0; index < drop_count; ++index) {
            const auto& message = all_messages[index];
            if (!message.is_object()) {
                continue;
            }
            const std::string role =
                message.value("role", std::string{"user"});
            std::string content;
            if (message.contains("content") &&
                message["content"].is_string()) {
                content = message["content"].get<std::string>();
            } else if (message.contains("content")) {
                // Non-string content (arrays / objects for tool calls) -
                // dump as JSON so the model still sees something rather
                // than a silent empty line.
                content = message["content"].dump();
            }
            transcript += role;
            transcript += ": ";
            transcript += content;
            transcript += "\n";
        }
        const std::string default_summary_prompt =
            "Please summarize the following conversation history "
            "concisely. Preserve important facts, decisions, and open "
            "questions. Output only the summary paragraph, no preamble.";
        const std::string summary_prompt = params.value(
            "summaryPrompt", default_summary_prompt);
        Json summary_messages = Json::array();
        summary_messages.push_back(Json{{"role", "system"},
                                         {"content", summary_prompt}});
        summary_messages.push_back(Json{{"role", "user"},
                                         {"content", transcript}});
        Json chat_params = Json::object();
        if (params.contains("provider")) {
            chat_params["provider"] = params["provider"];
        }
        if (params.contains("model") && params["model"].is_string()) {
            chat_params["model"] = params["model"];
        }
        chat_params["messages"] = std::move(summary_messages);
        // Forward the standard timeout/retry knobs so callers can crank
        // both down when they know the summary should be quick.
        for (const std::string_view field :
             {"temperature", "max_tokens", "retry"}) {
            const std::string key(field);
            if (params.contains(field)) {
                chat_params[key] = params[field];
            }
        }
        const uint32_t timeout_ms = params.value("timeoutMs", 60'000U);
        std::string summary_text;
        const int32_t chat_status =
            run_chat_sync(chat_params, timeout_ms, summary_text);
        if (chat_status != SAO_AI_EDITOR_OK) {
            // Conversation on disk is untouched - the LLM call happened
            // *before* the store rewrite, so callers can safely retry.
            return chat_status;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.compact(conversation_id, keep_last,
                                      summary_text, strategy, result);
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
            // Envelope integrity: hash the payload minus the sha256 field
            // itself.  Import will strip and recompute, so the digest must
            // be stamped last so it covers every stable field above.
            result["sha256"] = sha256_envelope_hex(result);
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        // conversations_.export_all() stamps its own sha256 — no post-hash
        // needed here.
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
        // Envelope integrity: if the payload carries a `sha256` field, verify
        // it against a recomputation over the payload with the field stripped.
        // Missing digest is permitted for backward compatibility with pre-
        // checksum exports (R4 shipped without it).
        if (payload.contains("sha256") && payload["sha256"].is_string()) {
            const std::string claimed =
                payload["sha256"].get<std::string>();
            const std::string recomputed = sha256_envelope_hex(payload);
            if (claimed.empty() || recomputed.empty() ||
                claimed != recomputed) {
                result = Json{{"message", "envelope sha256 mismatch"},
                              {"expected", claimed},
                              {"actual", recomputed}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
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
            // Envelope integrity — see conversation.export for the invariant.
            result["sha256"] = sha256_envelope_hex(result);
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // workflow_registry_.export_all() stamps its own sha256.
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
        // Envelope integrity guard — see conversation.import for the invariant.
        // Missing digest is tolerated for backward compatibility with R6-era
        // exports; a mismatched digest short-circuits with a protocol error
        // so tampered payloads cannot poison the registry.
        if (payload.contains("sha256") && payload["sha256"].is_string()) {
            const std::string claimed =
                payload["sha256"].get<std::string>();
            const std::string recomputed = sha256_envelope_hex(payload);
            if (claimed.empty() || recomputed.empty() ||
                claimed != recomputed) {
                result = Json{{"message", "envelope sha256 mismatch"},
                              {"expected", claimed},
                              {"actual", recomputed}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
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
    if (method == "workflow.list_executions") {
        const std::string scope_key = params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" &&
            scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        uint32_t limit = params.value("limit", 50U);
        limit = std::clamp<uint32_t>(limit, 1U, 500U);
        std::string workflow_filter;
        if (params.contains("workflowId")) {
            if (!params["workflowId"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            workflow_filter = params["workflowId"].get<std::string>();
        }
        // Collect from both scopes when scope=="all"; individual scope
        // requests only search the matching root.  Missing directories are
        // silently treated as empty by enumerate_history.
        std::vector<std::string_view> selected;
        if (scope_key == "all") {
            selected = {"workspace", "system"};
        } else {
            selected = {std::string_view(scope_key)};
        }
        std::vector<Json> combined;
        for (const auto scope_view : selected) {
            const std::filesystem::path root =
                workflow_history_root(scopes_, scope_view);
            std::vector<Json> summaries;
            const int32_t enumerate_status =
                WorkflowExecution::enumerate_history(root, workflow_filter,
                                                      summaries);
            if (enumerate_status != SAO_AI_EDITOR_OK) {
                return enumerate_status;
            }
            for (auto& summary : summaries) {
                summary["scope"] = std::string(scope_view);
                combined.push_back(std::move(summary));
            }
        }
        std::sort(combined.begin(), combined.end(),
                  [](const Json& left, const Json& right) {
                      return left.value("completedAt", int64_t{0}) >
                             right.value("completedAt", int64_t{0});
                  });
        Json items = Json::array();
        for (size_t index = 0;
             index < combined.size() && index < static_cast<size_t>(limit);
             ++index) {
            items.push_back(std::move(combined[index]));
        }
        result = Json{{"items", std::move(items)},
                      {"total", combined.size()}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.get_execution") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string execution_id = params["id"].get<std::string>();
        if (execution_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Try both roots (workspace first, then system).  Callers can pass
        // an explicit scope to skip the second lookup, but the default
        // scans everywhere so the API mirrors conversation.get semantics.
        const std::string scope_key =
            params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" &&
            scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::filesystem::path> candidates;
        if (scope_key == "workspace" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "workspace"));
        }
        if (scope_key == "system" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "system"));
        }
        for (const auto& root : candidates) {
            Json record;
            const int32_t status = WorkflowExecution::load_history_record(
                root, execution_id, record);
            if (status == SAO_AI_EDITOR_OK) {
                result = std::move(record);
                return SAO_AI_EDITOR_OK;
            }
            if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return status;
            }
        }
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (method == "workflow.delete_execution") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string execution_id = params["id"].get<std::string>();
        if (execution_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string scope_key =
            params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" &&
            scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::filesystem::path> candidates;
        if (scope_key == "workspace" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "workspace"));
        }
        if (scope_key == "system" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "system"));
        }
        bool deleted = false;
        for (const auto& root : candidates) {
            const int32_t status = WorkflowExecution::delete_history_record(
                root, execution_id);
            if (status == SAO_AI_EDITOR_OK) {
                deleted = true;
                continue;
            }
            if (status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return status;
            }
        }
        if (!deleted) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        result = Json{{"ok", true}, {"id", execution_id}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.skip_step" ||
        method == "workflow.replace_variable" ||
        method == "workflow.snapshot_variables") {
        // Runtime workflow-control surface — each op resolves the target
        // execution from workflow_executions_, then delegates to the
        // matching WorkflowExecution primitive.  A single validation
        // block handles the shared "executionId" arg so the individual
        // branches stay focussed on their unique params.  Lives here in
        // invoke() (not dispatch_workflow) because the "workflow." (no s)
        // methods route through the top-level dispatcher — the plural
        // "workflows." prefix is what feeds dispatch_workflow.
        if (!params.contains("executionId") ||
            !params["executionId"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string execution_id =
            params["executionId"].get<std::string>();
        if (execution_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::shared_ptr<WorkflowExecution> execution;
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            const auto found = workflow_executions_.find(execution_id);
            if (found != workflow_executions_.end()) {
                execution = found->second;
            }
        }
        if (!execution) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (method == "workflow.skip_step") {
            const std::string reason =
                params.value("reason", std::string{});
            const int32_t skip_status = execution->request_skip(reason);
            if (skip_status != SAO_AI_EDITOR_OK) {
                return skip_status;
            }
            result = Json{{"ok", true}, {"executionId", execution_id}};
            if (!reason.empty()) {
                result["reason"] = reason;
            }
            return SAO_AI_EDITOR_OK;
        }
        if (method == "workflow.replace_variable") {
            if (!params.contains("name") || !params["name"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (!params.contains("value") || !params["value"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const std::string name = params["name"].get<std::string>();
            const std::string value = params["value"].get<std::string>();
            const int32_t set_status = execution->set_variable(name, value);
            if (set_status != SAO_AI_EDITOR_OK) {
                return set_status;
            }
            result = Json{{"ok", true},
                          {"name", name},
                          {"value", value},
                          {"executionId", execution_id}};
            return SAO_AI_EDITOR_OK;
        }
        // workflow.snapshot_variables
        result = Json{{"executionId", execution_id},
                      {"variables", execution->variables_snapshot()}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.retry") {
        // Resume a previously-failed execution starting at the failed
        // step (or an explicit `fromStep` override).  Original variables
        // from the persisted record are preserved by default so downstream
        // steps see the successful outputs of earlier ones.
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string original_id = params["id"].get<std::string>();
        if (original_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string scope_key =
            params.value("scope", std::string{"all"});
        if (scope_key != "all" && scope_key != "workspace" &&
            scope_key != "system") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Locate the persisted record.  Mirror the workspace-first scan
        // pattern that workflow.get_execution uses so the two APIs stay
        // consistent for callers who don't pin a scope.
        std::vector<std::filesystem::path> candidates;
        if (scope_key == "workspace" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "workspace"));
        }
        if (scope_key == "system" || scope_key == "all") {
            candidates.push_back(workflow_history_root(scopes_, "system"));
        }
        Json record;
        bool found_record = false;
        for (const auto& root : candidates) {
            const int32_t load_status = WorkflowExecution::load_history_record(
                root, original_id, record);
            if (load_status == SAO_AI_EDITOR_OK) {
                found_record = true;
                break;
            }
            if (load_status != SAO_AI_EDITOR_ERR_NOT_FOUND) {
                return load_status;
            }
        }
        if (!found_record) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        // Only failed executions are eligible — completed runs have no
        // work left, cancelled runs are intentionally aborted, and
        // running/pending records would race with an already-live worker.
        if (record.value("status", std::string{}) != "failed") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string workflow_id =
            record.value("workflowId", std::string{});
        if (workflow_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Refresh the registry snapshot the same way workflows.run does so
        // an out-of-band edit to the definition takes effect on retry.
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            workflow_registry_.reload(scopes_);
        }
        WorkflowDefinition definition;
        if (!workflow_registry_.get(workflow_id, definition)) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        if (definition.steps.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Determine the target step index.  Callers may override with
        // `fromStep`; otherwise scan stepResults for the first
        // status=="failed" entry, falling back to record.currentStep when
        // the persisted stepResults array is missing (older schemas).
        size_t from_step = definition.steps.size();
        if (params.contains("fromStep") && !params["fromStep"].is_null()) {
            if (!params["fromStep"].is_number_integer() &&
                !params["fromStep"].is_number_unsigned()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const int64_t requested = params["fromStep"].get<int64_t>();
            if (requested < 0) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            from_step = static_cast<size_t>(requested);
        } else {
            bool found_failed = false;
            if (record.contains("stepResults") &&
                record["stepResults"].is_array()) {
                for (const auto& step_result : record["stepResults"]) {
                    if (!step_result.is_object()) {
                        continue;
                    }
                    if (step_result.value("status", std::string{}) ==
                            "failed" &&
                        step_result.contains("stepIndex")) {
                        const int64_t idx =
                            step_result["stepIndex"].get<int64_t>();
                        if (idx < 0) {
                            continue;
                        }
                        from_step = static_cast<size_t>(idx);
                        found_failed = true;
                        break;
                    }
                }
            }
            if (!found_failed) {
                const int64_t current = record.value("currentStep",
                                                      int64_t{0});
                from_step = current < 0 ? 0 : static_cast<size_t>(current);
            }
        }
        if (from_step >= definition.steps.size()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Resolve the (possibly-new) provider before we build the
        // execution so an invalid provider payload doesn't leak a
        // half-registered execution into workflow_executions_.
        Json provider;
        std::string api_key;
        const int32_t provider_status =
            resolve_provider(params, provider, api_key);
        if (provider_status != SAO_AI_EDITOR_OK) {
            return provider_status;
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
        const bool keep_variables = params.value("keepVariables", true);
        // Seed the initial input from the persisted record so
        // interpolations like {{input}} still resolve when
        // keepVariables=false.  The ctor then re-derives variables_
        // from this payload; keepVariables=true overrides variables_
        // wholesale after construction via seed_retry_state.
        Json initial_input = Json::object();
        if (record.contains("variables") && record["variables"].is_object() &&
            record["variables"].contains("input")) {
            initial_input["input"] = record["variables"]["input"];
        }
        std::unordered_map<std::string, std::string> preserved;
        if (keep_variables && record.contains("variables") &&
            record["variables"].is_object()) {
            for (const auto& [key, value] : record["variables"].items()) {
                if (value.is_string()) {
                    preserved[key] = value.get<std::string>();
                } else {
                    preserved[key] = value.dump();
                }
            }
        }
        static std::atomic<uint64_t> retry_execution_counter{0};
        const std::string new_id =
            "wf-" + std::to_string(GetTickCount64()) + "-retry-" +
            std::to_string(retry_execution_counter.fetch_add(
                1, std::memory_order_relaxed));
        auto execution = std::make_shared<WorkflowExecution>(
            new_id, std::move(definition), std::move(initial_input));
        execution->seed_retry_state(from_step, std::move(preserved),
                                     original_id);
        {
            std::lock_guard<std::mutex> guard(workflow_mutex_);
            workflow_executions_[execution->id()] = execution;
        }
        const std::filesystem::path history_dir =
            workflow_history_root(scopes_, "workspace");
        execution->start(*this, std::move(provider), model, chat_timeout,
                          history_dir);
        result = Json{{"executionId", execution->id()},
                      {"retryOf", original_id},
                      {"fromStep", static_cast<int64_t>(from_step)},
                      {"status", "running"}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "workflow.dry_run") {
        // Preview each step's interpolated prompt without hitting the LLM.
        // Reuses WorkflowExecution::interpolate_with so the substitution
        // rules stay 1:1 with a real workflows.run — callers can rely on
        // this to eyeball {{var}} bindings, group batching, and
        // confirmation gates before spending tokens.
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
        // Seed variables the same way WorkflowExecution's ctor does — start
        // from `input` (falling back to inputs[input]), fold every key
        // from `inputs`, then overlay `simulateOutputs` last so the caller
        // can pretend earlier steps already ran.  Non-string values are
        // JSON-encoded to match ctor semantics.
        std::unordered_map<std::string, std::string> variables;
        auto absorb_object = [&variables](const Json& source) {
            if (!source.is_object()) {
                return;
            }
            for (const auto& [key, value] : source.items()) {
                if (value.is_string()) {
                    variables[key] = value.get<std::string>();
                } else {
                    variables[key] = value.dump();
                }
            }
        };
        if (params.contains("input")) {
            const Json& input = params["input"];
            if (input.is_string()) {
                variables["input"] = input.get<std::string>();
            } else if (input.is_object()) {
                absorb_object(input);
            } else if (!input.is_null()) {
                variables["input"] = input.dump();
            }
        }
        if (params.contains("inputs")) {
            absorb_object(params["inputs"]);
        }
        if (variables.find("input") == variables.end()) {
            variables["input"] = "";
        }
        if (params.contains("simulateOutputs")) {
            absorb_object(params["simulateOutputs"]);
        }
        // Track the initial variable set separately so the response can
        // list "estimatedVariables" in the order they'd appear during a
        // real run: initial seed keys first, then each step's output_var
        // as it fires.
        std::vector<std::string> estimated_variables;
        std::unordered_map<std::string, size_t> estimated_index;
        auto record_var = [&](const std::string& name) {
            if (name.empty()) {
                return;
            }
            if (estimated_index.find(name) != estimated_index.end()) {
                return;
            }
            estimated_index.emplace(name, estimated_variables.size());
            estimated_variables.push_back(name);
        };
        // Seed keys are recorded in a stable order — `input` always
        // first, then everything else sorted for determinism.
        record_var("input");
        std::vector<std::string> seed_keys;
        for (const auto& [key, value] : variables) {
            (void)value;
            if (key != "input") {
                seed_keys.push_back(key);
            }
        }
        std::sort(seed_keys.begin(), seed_keys.end());
        for (const auto& key : seed_keys) {
            record_var(key);
        }
        // Walk the definition once, collecting per-step preview info and
        // sliding a window across contiguous same-group runs to identify
        // parallel batches.
        const auto& steps = definition.steps;
        Json preview = Json::array();
        Json groups = Json::array();
        std::vector<size_t> batch_end_of(steps.size(), 0);
        std::vector<size_t> batch_start_of(steps.size(), 0);
        for (size_t index = 0; index < steps.size();) {
            size_t batch_end = index + 1;
            const std::string& group_id = steps[index].group;
            if (!group_id.empty()) {
                while (batch_end < steps.size() &&
                       steps[batch_end].group == group_id) {
                    ++batch_end;
                }
            }
            const size_t batch_size = batch_end - index;
            const bool parallel = batch_size > 1;
            groups.push_back(
                Json{{"groupId", group_id},
                     {"startStep", static_cast<int64_t>(index)},
                     {"endStep", static_cast<int64_t>(batch_end - 1)},
                     {"parallel", parallel}});
            for (size_t k = 0; k < batch_size; ++k) {
                batch_start_of[index + k] = index;
                batch_end_of[index + k] = batch_end;
            }
            index = batch_end;
        }
        // Second pass: render each step against the current variables map,
        // then commit a synthesised output_var so subsequent steps see the
        // placeholder in {{var}} interpolations.  Parallel batches share
        // one snapshot for rendering (matching run_loop's pre-batch
        // snapshot semantics), then commit together at the batch boundary
        // — otherwise a group's later steps would see earlier steps' fake
        // output which never happens at runtime.
        for (size_t index = 0; index < steps.size();) {
            const size_t batch_start = index;
            const size_t batch_end = batch_end_of[index];
            const size_t batch_size = batch_end - batch_start;
            const std::string& group_id = steps[batch_start].group;
            const std::unordered_map<std::string, std::string> snapshot =
                variables;
            for (size_t k = 0; k < batch_size; ++k) {
                const size_t step_index = batch_start + k;
                const WorkflowStep& step = steps[step_index];
                const std::string rendered =
                    WorkflowExecution::interpolate_with(step.prompt, snapshot);
                Json will_parallel = Json::array();
                for (size_t j = 0; j < batch_size; ++j) {
                    if (batch_start + j == step_index) {
                        continue;
                    }
                    will_parallel.push_back(
                        static_cast<int64_t>(batch_start + j));
                }
                preview.push_back(
                    Json{{"stepIndex", static_cast<int64_t>(step_index)},
                         {"label", step.label},
                         {"agent", step.agent},
                         {"outputVar", step.output_var},
                         {"group", group_id},
                         {"requiresConfirmation", step.requires_confirmation},
                         {"renderedPrompt", rendered},
                         {"willBeParallelWith", std::move(will_parallel)}});
            }
            // Commit synthesised outputs after the whole batch renders so
            // the next batch's snapshot picks them up.  simulateOutputs
            // wins over the placeholder — the caller's provided value
            // remains authoritative across the whole preview.
            for (size_t k = 0; k < batch_size; ++k) {
                const WorkflowStep& step = steps[batch_start + k];
                if (step.output_var.empty()) {
                    continue;
                }
                record_var(step.output_var);
                if (variables.find(step.output_var) != variables.end()) {
                    // A caller-provided simulateOutputs (or a matching
                    // inputs key) trumps the synthetic placeholder.  Keep
                    // whatever's already there so downstream steps see
                    // that exact value.
                    continue;
                }
                std::string placeholder = "<simulated: ";
                placeholder += step.label.empty() ? step.output_var : step.label;
                placeholder += ">";
                variables[step.output_var] = std::move(placeholder);
            }
            index = batch_end;
        }
        Json estimated = Json::array();
        for (const auto& name : estimated_variables) {
            estimated.push_back(name);
        }
        result = Json{
            {"workflowId", definition.id},
            {"workflowName", definition.name},
            {"totalSteps", static_cast<int64_t>(steps.size())},
            {"groups", std::move(groups)},
            {"preview", std::move(preview)},
            {"estimatedVariables", std::move(estimated)}};
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
        const std::string requested_name = params["name"].get<std::string>();
        // Resolve aliases *before* firing hooks so the audit payload sees the
        // canonical tool name (matches the value passed to tool_filter).  The
        // alias table read is cheap + independent of store_mutex_, so we do it
        // outside the lock we take for execute().
        const std::string resolved_name = tools_.resolve_alias(requested_name);
        // Before-phase hooks fire even when the tool ends up returning
        // NOT_FOUND — the payload includes the tool name that failed to
        // dispatch, which is exactly what an audit sink wants to record.
        const auto before_hooks =
            tools_.snapshot_hooks("before", resolved_name);
        for (const auto& hook : before_hooks) {
            Json payload{{"hookId", hook.id},
                         {"phase", "before"},
                         {"tool", resolved_name},
                         {"arguments", arguments}};
            if (resolved_name != requested_name) {
                payload["requestedTool"] = requested_name;
            }
            emit(hook.emit_event, payload);
        }
        const auto call_start = std::chrono::steady_clock::now();
        // Telemetry: every dispatched tools.call counts as an invocation
        // regardless of cache outcome — matches VSCode's outputMonitor
        // where the "asked for it" event is what the LLM proxy records.
        // record_result() below fires on both hit + miss.
        tool_monitor_.record_invocation(resolved_name);
        // Session-memory dedup: mutation-aware invalidation runs first, then
        // read-only calls try the cache and short-circuit on hit.  observe()
        // is a no-op for read-only tools; lookup() returns nullopt for
        // uncached tools (mutations, custom).  See tool_result_cache.h.
        tool_cache_.observe(resolved_name, arguments);
        if (auto hit = tool_cache_.lookup(resolved_name, arguments)) {
            result = hit->result;
            const int64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            const int64_t age_ms = now_ms - hit->timestamp_ms;
            if (result.is_object()) {
                result["cacheHit"] = true;
                result["cacheAgeMs"] = age_ms;
            }
            Json hit_payload{{"tool", resolved_name},
                             {"arguments", arguments},
                             {"cacheAgeMs", age_ms}};
            if (resolved_name != requested_name) {
                hit_payload["requestedTool"] = requested_name;
            }
            emit("tools.cache.hit", hit_payload);
            // Telemetry (cache-hit path): duration is 0 by contract so the
            // monitor's avg_duration_ms excludes it — cache-hit latency is
            // essentially "one hash lookup" and folding it into the mean
            // would hide the true cost of the miss path.
            tool_monitor_.record_result(resolved_name, result, /*duration_ms=*/0,
                                        /*cache_hit=*/true);
            // Cache hits still fire the after-phase hook so audit sinks see a
            // canonical (before, after) pair even for served-from-memory calls.
            const auto after_hooks =
                tools_.snapshot_hooks("after", resolved_name);
            for (const auto& hook : after_hooks) {
                Json payload{{"hookId", hook.id},
                             {"phase", "after"},
                             {"tool", resolved_name},
                             {"arguments", arguments},
                             {"result", result},
                             {"durationMs", 0},
                             {"cacheHit", true}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                emit(hook.emit_event, payload);
            }
            return SAO_AI_EDITOR_OK;
        }
        int32_t status;
        {
            std::lock_guard<std::mutex> lock(store_mutex_);
            status = tools_.execute(mode, requested_name, arguments, result);
        }
        const auto call_end = std::chrono::steady_clock::now();
        const auto duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                call_end - call_start)
                .count();
        // Record successful executions into the session-memory cache.  We
        // deliberately skip failed calls (permission denied, boundary
        // violation, schema failure) so retries do not return the error
        // payload as if it were the ground truth.
        if (status == SAO_AI_EDITOR_OK) {
            tool_cache_.record(resolved_name, arguments, result);
            tool_monitor_.record_result(resolved_name, result, duration_ms,
                                        /*cache_hit=*/false);
        } else {
            // Error-path telemetry: bump error_count only.  We deliberately
            // skip record_result here so failed calls do not contaminate the
            // avg_duration_ms + total_bytes_returned aggregates.
            tool_monitor_.record_error(resolved_name);
        }
        // After / error hooks reflect the outcome.  We treat every non-OK
        // status as an error phase so validation failures + permission
        // denials also surface — audits typically care about *any* non-happy
        // path.  The result payload is included for after; for error we
        // include the numeric status code the JSON-RPC caller would see so
        // downstream sinks do not need to keep a parallel status table.
        if (status == SAO_AI_EDITOR_OK) {
            const auto after_hooks =
                tools_.snapshot_hooks("after", resolved_name);
            for (const auto& hook : after_hooks) {
                Json payload{{"hookId", hook.id},
                             {"phase", "after"},
                             {"tool", resolved_name},
                             {"arguments", arguments},
                             {"result", result},
                             {"durationMs", duration_ms}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                emit(hook.emit_event, payload);
            }
        } else {
            const auto error_hooks =
                tools_.snapshot_hooks("error", resolved_name);
            for (const auto& hook : error_hooks) {
                Json payload{{"hookId", hook.id},
                             {"phase", "error"},
                             {"tool", resolved_name},
                             {"arguments", arguments},
                             {"status", status},
                             {"durationMs", duration_ms}};
                if (resolved_name != requested_name) {
                    payload["requestedTool"] = requested_name;
                }
                if (!result.is_null()) {
                    payload["errorResult"] = result;
                }
                emit(hook.emit_event, payload);
            }
        }
        return status;
    }
    if (method == "tools.register") {
        if (!params.contains("name") || !params["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // description is optional, parameters is optional (defaults to a
        // {type:"object", properties:{}} schema inside the registry).
        const std::string description = params.value("description",
                                                     std::string{});
        const Json parameters = params.value("parameters", Json(nullptr));
        // Custom tools are read-only unless the caller opts in — matches
        // built-in defaults (readFile/listFiles/searchFiles are read-only).
        const bool read_only = params.value("readOnly", true);
        const std::string name = params["name"].get<std::string>();
        const int32_t status = tools_.register_custom(name, description,
                                                      parameters, read_only);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"name", name}};
        }
        return status;
    }
    if (method == "tools.unregister") {
        if (!params.contains("name") || !params["name"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string name = params["name"].get<std::string>();
        const int32_t status = tools_.unregister_custom(name);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"name", name}};
        }
        return status;
    }
    if (method == "tools.register_alias") {
        // alias + target are both required strings.  Anything else is
        // INVALID_ARGUMENT — the registry itself also validates but rejecting
        // here keeps the JSON-RPC error boundary crisp.
        if (!params.contains("alias") || !params["alias"].is_string() ||
            !params.contains("target") || !params["target"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string alias = params["alias"].get<std::string>();
        const std::string target = params["target"].get<std::string>();
        const int32_t status = tools_.register_alias(alias, target);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true},
                          {"alias", alias},
                          {"target", target}};
        }
        return status;
    }
    if (method == "tools.unregister_alias") {
        if (!params.contains("alias") || !params["alias"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string alias = params["alias"].get<std::string>();
        const int32_t status = tools_.unregister_alias(alias);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"alias", alias}};
        }
        return status;
    }
    if (method == "tools.register_hook") {
        // id + phase + emitEvent are required.  `toolFilter` is optional — null
        // or missing means "match every tool".  When present it must be an
        // array of strings (mixed / non-string entries are rejected up-front so
        // downstream matching does not need to filter them out per call).
        if (!params.contains("id") || !params["id"].is_string() ||
            !params.contains("phase") || !params["phase"].is_string() ||
            !params.contains("emitEvent") ||
            !params["emitEvent"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::vector<std::string> filter;
        if (params.contains("toolFilter") &&
            !params["toolFilter"].is_null()) {
            const auto& raw = params["toolFilter"];
            if (!raw.is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            filter.reserve(raw.size());
            for (const auto& entry : raw) {
                if (!entry.is_string()) {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                filter.push_back(entry.get<std::string>());
            }
        }
        const std::string id = params["id"].get<std::string>();
        const std::string phase = params["phase"].get<std::string>();
        const std::string emit_event =
            params["emitEvent"].get<std::string>();
        const int32_t status =
            tools_.register_hook(id, phase, filter, emit_event);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true},
                          {"id", id},
                          {"phase", phase},
                          {"emitEvent", emit_event}};
        }
        return status;
    }
    if (method == "tools.unregister_hook") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string id = params["id"].get<std::string>();
        const int32_t status = tools_.unregister_hook(id);
        if (status == SAO_AI_EDITOR_OK) {
            result = Json{{"ok", true}, {"id", id}};
        }
        return status;
    }
    if (method == "tools.cache_stats") {
        // Session-memory cache introspection — used by the debug console
        // + tests to confirm hit/miss/invalidation accounting.
        result = tool_cache_.stats();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.cache_clear") {
        // Wipe every cached entry + zero the counters.  No parameters — the
        // cache is per-runtime so scoping is implicit.  We deliberately do
        // NOT touch tool_monitor_ here — the telemetry ledger is a long-run
        // view of tool usage that shouldn't reset just because the dedup
        // cache was flushed.  Use `tools.telemetry_clear` for that.
        tool_cache_.clear();
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.telemetry_stats") {
        // Per-tool execution counters (invocations, bytes returned, cache
        // hit ratio, avg duration, error count).  See
        // tool_execution_monitor.h for the full schema.
        result = tool_monitor_.stats();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "tools.telemetry_clear") {
        // Wipe every monitor counter.  Deliberately independent from
        // tools.cache_clear so ops can zero the cache without losing
        // invocation history and vice-versa.
        tool_monitor_.clear();
        result = Json{{"ok", true}};
        return SAO_AI_EDITOR_OK;
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
    if (method == "chat.dispatch_tool_calls") {
        return dispatch_tool_calls(params, result);
    }
    if (method == "chat.set_pricing") {
        return set_pricing(params, result);
    }
    if (method == "chat.get_pricing") {
        return get_pricing(params, result);
    }
    if (method == "chat.list_pricing") {
        return list_pricing(params, result);
    }
    if (method == "chat.cost_stats") {
        return cost_stats(params, result);
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
    // vscode.* / sao.host.* — normally surfaced by the Node-side extension
    // shim invoking back through dispatch_extension_call.  We also expose
    // them on the JSON-RPC dispatch surface so tests + local tools can
    // exercise the same handlers without spinning up a Node worker.
    if (method.starts_with("vscode.") ||
        method.starts_with("sao.host.")) {
        return dispatch_extension_call(method, params, result);
    }
    if (method == "agents.list_defs" || method == "agents.get_def" ||
        method == "agents.save_def" || method == "agents.delete_def" ||
        method == "agents.invoke" || method == "agents.recommend" ||
        method == "agents.export" || method == "agents.import") {
        return dispatch_agent(method, params, result);
    }
    if (method == "agents.invoke_with_mcp") {
        return agent_invoke_with_mcp(params, result);
    }
    if (method == "agents.batch_invoke") {
        return batch_invoke_agents(params, result);
    }
    if (method == "prompts.list_defs" || method == "prompts.get_def" ||
        method == "prompts.save_def" || method == "prompts.delete_def" ||
        method == "prompts.render" || method == "prompts.render_batch" ||
        method == "prompts.list_tags" || method == "prompt.pin" ||
        method == "prompt.unpin") {
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
    if (method == "agents.recommend") {
        // Rule-based recommender.  The registry does the scoring; this branch
        // just marshals params → typed args, applies the include* filter mask
        // (an all-false include set short-circuits to an empty response so
        // we don't rely on the "0 == include everything" fallback), and
        // reloads the registry so any market presets / on-disk edits land
        // before we score.
        if (!params.contains("query") || !params["query"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const std::string query = params["query"].get<std::string>();
        const int top_k = params.value("topK", 3);
        const bool include_builtin = params.value("includeBuiltin", true);
        const bool include_market = params.value("includeMarket", true);
        const bool include_user = params.value("includeUser", true);
        uint32_t scope_flags = 0U;
        if (include_builtin) {
            scope_flags |= kRecommendIncludeBuiltin;
        }
        if (include_market) {
            scope_flags |= kRecommendIncludeMarket;
        }
        if (include_user) {
            scope_flags |= kRecommendIncludeUser;
        }
        const bool all_disabled =
            !include_builtin && !include_market && !include_user;
        std::vector<std::string> boost_tags;
        if (params.contains("boostTags") && params["boostTags"].is_array()) {
            for (const auto& tag : params["boostTags"]) {
                if (tag.is_string()) {
                    boost_tags.push_back(tag.get<std::string>());
                }
            }
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        if (all_disabled) {
            result = Json{{"query", query},
                          {"recommendations", Json::array()},
                          {"total", 0}};
            return SAO_AI_EDITOR_OK;
        }
        return agent_registry_.recommend(query, top_k, scope_flags, boost_tags,
                                          result);
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
    if (method == "agents.export") {
        // Mirrors workflow.export: `id` and `scope` are mutually exclusive.
        // Passing neither or both is a caller error rather than a "return
        // everything" convenience — the client already has agents.list_defs
        // for browsing.
        const bool has_id = params.contains("id");
        const bool has_scope = params.contains("scope");
        if (has_id == has_scope) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            agent_registry_.reload(scopes_);
        }
        if (has_id) {
            if (!params["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            AgentDefinition agent;
            if (!agent_registry_.get(params["id"].get<std::string>(), agent)) {
                return SAO_AI_EDITOR_ERR_NOT_FOUND;
            }
            const int64_t now = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::system_clock::now()
                                        .time_since_epoch())
                                    .count();
            result = Json{{"format", "sao-agent/1"},
                          {"agent", agent.to_json()},
                          {"exportedAt", now}};
            // Envelope integrity — see conversation.export for the invariant.
            result["sha256"] = sha256_envelope_hex(result);
            return SAO_AI_EDITOR_OK;
        }
        if (!params["scope"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // agent_registry_.export_all() stamps its own sha256.
        return agent_registry_.export_all(
            params["scope"].get<std::string>(), result);
    }
    if (method == "agents.import") {
        if (!params.contains("payload") || !params["payload"].is_object()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Accept the same scope key vocabulary as agents.save_def: bare
        // "workspace"/"system" or "plugin:<id>".  Default to workspace so
        // callers that omit `scope` land somewhere reasonable.
        const std::string scope_key =
            params.value("scope", std::string{"workspace"});
        std::string scope;
        std::string plugin_id;
        if (!parse_scope_key(scope_key, scope, plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        const bool overwrite = params.value("overwrite", false);
        const Json& payload = params["payload"];
        // Envelope integrity guard — see conversation.import for the invariant.
        // Older R13-era exports predate the checksum so a missing digest is
        // silently accepted; a present-but-wrong digest surfaces PROTOCOL.
        if (payload.contains("sha256") && payload["sha256"].is_string()) {
            const std::string claimed =
                payload["sha256"].get<std::string>();
            const std::string recomputed = sha256_envelope_hex(payload);
            if (claimed.empty() || recomputed.empty() ||
                claimed != recomputed) {
                result = Json{{"message", "envelope sha256 mismatch"},
                              {"expected", claimed},
                              {"actual", recomputed}};
                return SAO_AI_EDITOR_ERR_PROTOCOL;
            }
        }
        const std::string format = payload.value("format", std::string{});
        std::vector<Json> incoming;
        if (format == "sao-agent/1") {
            if (!payload.contains("agent") ||
                !payload["agent"].is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            incoming.push_back(payload["agent"]);
        } else if (format == "sao-agents/1") {
            if (!payload.contains("agents") ||
                !payload["agents"].is_array()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            for (const auto& item : payload["agents"]) {
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
        agent_registry_.reload(scopes_);
        // Two-pass conflict detection so overwrite=false rejects atomically
        // instead of half-importing a batch.  Built-ins are always fatal —
        // even with overwrite=true — because letting a user replace a
        // built-in id would either shadow it after reload or masquerade a
        // user agent as compile-time.
        Json conflicts = Json::array();
        for (const auto& agent : incoming) {
            const std::string candidate_id =
                agent.value("id", std::string{});
            if (candidate_id.empty() || !valid_simple_id(candidate_id)) {
                result = Json{{"message", "invalid agent id"},
                              {"id", candidate_id}};
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            if (agent_registry_.is_builtin(candidate_id)) {
                result = Json{{"imported", 0},
                              {"conflicts", Json::array({candidate_id})},
                              {"assignedIds", Json::array()},
                              {"message", "cannot overwrite builtin"}};
                return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
            }
            AgentDefinition existing;
            if (agent_registry_.get(candidate_id, existing) &&
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
        for (const auto& agent : incoming) {
            std::string assigned_id;
            const int32_t import_status = agent_registry_.import_agent(
                agent, scope, plugin_id, overwrite, scopes_, assigned_id);
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

int32_t NativeRuntime::batch_invoke_agents(const Json& params, Json& result) {
    if (!params.contains("agents") || !params["agents"].is_array() ||
        params["agents"].empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& agents_array = params["agents"];
    // Default fields let callers avoid repeating provider/model/message
    // per-agent when a common configuration applies.  Per-agent entries
    // override these with the usual JSON "value" semantics (empty string ==
    // missing).
    const Json default_provider = params.value("provider", Json::object());
    const Json top_default_provider =
        params.value("defaultProvider", default_provider);
    const std::string default_model = params.value(
        "defaultModel", params.value("model", std::string{}));
    const std::string default_message = params.value(
        "defaultMessage", std::string{});
    const uint32_t default_timeout_ms = params.value("timeoutMs", 60'000U);
    const std::string conversation_id = params.value("conversationId",
                                                       std::string{});
    // concurrency policy: clamp caller value to [1, 16].  Default 4 matches
    // the doc contract; upper bound 16 keeps the WinHTTP session pool from
    // exploding under pathological input.
    int concurrency = params.value("concurrency", 4);
    if (concurrency < 1) {
        concurrency = 1;
    }
    if (concurrency > 16) {
        concurrency = 16;
    }
    const size_t total = agents_array.size();
    const size_t worker_count = std::min<size_t>(
        static_cast<size_t>(concurrency), total);

    // Refresh the registry once up front so every worker sees a consistent
    // snapshot without re-taking store_mutex_ per agent (registry is otherwise
    // treated as read-only from worker threads).
    {
        std::lock_guard<std::mutex> guard(store_mutex_);
        agent_registry_.reload(scopes_);
    }

    struct Slot final {
        std::string agent_id;
        std::string agent_name;
        std::string message;
        std::string model;
        std::string content;
        std::string error;
        int32_t status = SAO_AI_EDITOR_OK;
        bool resolved = false;  // agent_id -> definition resolved OK
        uint64_t duration_ms = 0;
        Json params_snapshot = Json::object();
    };
    std::vector<Slot> slots(total);

    // Phase 1 (single-threaded): resolve every agent's definition and build
    // the run_chat_sync parameter blob.  Doing this before dispatching
    // workers keeps the JSON parsing on the caller thread and lets us
    // surface "agent not found" / "provider missing" as per-slot failures
    // without touching the WinHTTP path.
    for (size_t i = 0; i < total; ++i) {
        Slot& slot = slots[i];
        const Json& entry = agents_array[i];
        if (!entry.is_object()) {
            slot.agent_id = "";
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "agent entry must be an object";
            continue;
        }
        slot.agent_id = entry.value("id", std::string{});
        if (slot.agent_id.empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "agent id missing";
            continue;
        }
        AgentDefinition agent;
        if (!agent_registry_.get(slot.agent_id, agent)) {
            slot.status = SAO_AI_EDITOR_ERR_NOT_FOUND;
            slot.error = "agent not found";
            continue;
        }
        slot.agent_name = agent.name;
        // Per-agent message overrides defaultMessage; both empty -> failure.
        slot.message = entry.value("message", default_message);
        if (slot.message.empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "message missing";
            continue;
        }
        // Provider: per-agent > top-level defaultProvider > empty.  Model:
        // per-agent > defaultModel > agent.model.
        Json provider = entry.contains("provider") &&
                                entry["provider"].is_object()
                            ? entry["provider"]
                            : top_default_provider;
        std::string model = entry.value(
            "model", default_model.empty() ? agent.model : default_model);
        if ((!provider.is_object() || provider.empty()) && model.empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "provider or model missing";
            continue;
        }
        slot.model = model;
        // Build the messages array from the agent's system prompt + user
        // turn.  Unlike agents.invoke, batch mode never consults conversation
        // history for the outbound request — every agent gets a clean slate
        // per its own message.  We still write successful turns back to
        // conversation storage afterwards for callers that pass
        // conversationId.
        Json messages = agent_registry_.build_chat_messages(
            agent, slot.message, Json::array());
        Json chat_params = Json::object();
        if (provider.is_object() && !provider.empty()) {
            chat_params["provider"] = std::move(provider);
        }
        if (!model.empty()) {
            chat_params["model"] = model;
        }
        chat_params["messages"] = std::move(messages);
        // Forward a small allowlist of chat.run knobs so callers can pass
        // temperature / max_tokens once via defaults or per-agent overrides.
        for (const std::string_view field :
             {"temperature", "max_tokens", "response_format", "stream"}) {
            const std::string key(field);
            if (entry.contains(field)) {
                chat_params[key] = entry[field];
            } else if (params.contains(field)) {
                chat_params[key] = params[field];
            }
        }
        slot.params_snapshot = std::move(chat_params);
        slot.resolved = true;
    }

    // Phase 2: launch worker threads that only touch their own slot.  Use
    // an atomic counter as a lock-free work queue so `concurrency` acts as
    // an in-flight cap without per-task locking.  Slots that failed
    // resolution in phase 1 are skipped instantly.
    const auto batch_start = std::chrono::steady_clock::now();
    std::atomic<size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&, i_capture = w] {
            (void)i_capture;
            while (true) {
                const size_t idx =
                    next_index.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= total) {
                    break;
                }
                Slot& slot = slots[idx];
                if (!slot.resolved) {
                    continue;  // phase 1 already recorded status/error
                }
                const auto step_start = std::chrono::steady_clock::now();
                try {
                    std::string content;
                    const int32_t status = run_chat_sync(
                        slot.params_snapshot, default_timeout_ms, content);
                    slot.status = status;
                    if (status == SAO_AI_EDITOR_OK) {
                        slot.content = std::move(content);
                    } else {
                        slot.error = status_message(status);
                    }
                } catch (const std::exception& ex) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = std::string("exception: ") + ex.what();
                } catch (...) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = "unknown exception";
                }
                const auto step_end = std::chrono::steady_clock::now();
                slot.duration_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        step_end - step_start).count());
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto batch_end = std::chrono::steady_clock::now();
    const uint64_t total_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            batch_end - batch_start).count());

    // Phase 3: assemble results in original order.  Conversation appends
    // happen here (single-threaded) so the transcript ordering is
    // deterministic regardless of which worker finished first.  Successful
    // agent replies are prefixed with the agent name so downstream readers
    // can attribute lines back to their source.
    Json results = Json::array();
    size_t success = 0;
    size_t failure = 0;
    for (size_t i = 0; i < total; ++i) {
        const Slot& slot = slots[i];
        Json entry_result{{"agentId", slot.agent_id},
                          {"durationMs", slot.duration_ms}};
        if (!slot.agent_name.empty()) {
            entry_result["agentName"] = slot.agent_name;
        }
        if (!slot.model.empty()) {
            entry_result["model"] = slot.model;
        }
        if (slot.status == SAO_AI_EDITOR_OK && slot.resolved) {
            entry_result["status"] = "completed";
            entry_result["content"] = slot.content;
            ++success;
            if (!conversation_id.empty()) {
                std::lock_guard<std::mutex> guard(store_mutex_);
                Json append_result;
                conversations_.append(
                    conversation_id,
                    Json{{"role", "user"}, {"content", slot.message},
                         {"agentId", slot.agent_id}},
                    append_result);
                const std::string prefix = !slot.agent_name.empty()
                    ? "[" + slot.agent_name + "] "
                    : "[" + slot.agent_id + "] ";
                conversations_.append(
                    conversation_id,
                    Json{{"role", "assistant"},
                         {"content", prefix + slot.content},
                         {"agentId", slot.agent_id}},
                    append_result);
            }
        } else {
            entry_result["status"] = "failed";
            entry_result["error"] = slot.error.empty()
                                        ? status_message(slot.status)
                                        : slot.error;
            ++failure;
        }
        results.push_back(std::move(entry_result));
    }
    result = Json{{"results", std::move(results)},
                  {"successCount", success},
                  {"failureCount", failure},
                  {"totalMs", total_ms}};
    if (!conversation_id.empty()) {
        result["conversationId"] = conversation_id;
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_tool_calls(const Json& params, Json& result) {
    if (!params.contains("toolCalls") || !params["toolCalls"].is_array() ||
        params["toolCalls"].empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // mode is validated up-front: unlike batch_invoke_agents (where each
    // per-agent slot could conceivably carry its own override), the entire
    // dispatch shares a single permission policy so a stray "chat" mode
    // should fail fast instead of silently downgrading every call to agent.
    const std::string mode = mode_of(params);
    if (!supported_mode(mode)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const Json& tool_calls = params["toolCalls"];
    // Concurrency policy mirrors agents.batch_invoke: clamp [1, 16] with a
    // default of 4.  A tool dispatch batch is usually smaller than an agent
    // batch (an LLM rarely emits >5 tool_calls in one turn), but the same
    // upper bound stops a hostile prompt from spawning hundreds of threads.
    int concurrency = params.value("concurrency", 4);
    if (concurrency < 1) {
        concurrency = 1;
    }
    if (concurrency > 16) {
        concurrency = 16;
    }
    const size_t total = tool_calls.size();
    const size_t worker_count = std::min<size_t>(
        static_cast<size_t>(concurrency), total);

    struct Slot final {
        std::string id;        // caller-supplied opaque id (echoed back)
        std::string name;      // tool name (empty on validation failure)
        Json arguments = Json::object();
        Json result_payload = Json::object();
        std::string error;
        int32_t status = SAO_AI_EDITOR_OK;
        bool ready = false;    // arguments parsed / name checked OK
        uint64_t duration_ms = 0;
    };
    std::vector<Slot> slots(total);

    // Phase 1 (single-threaded): validate id/name/arguments shape and, when
    // arguments is a JSON string (as LLMs emit them), parse it eagerly so
    // worker threads never touch the raw string form.  Any per-call failure
    // is recorded on the slot and skipped by the worker — peers still run.
    for (size_t i = 0; i < total; ++i) {
        Slot& slot = slots[i];
        const Json& entry = tool_calls[i];
        if (!entry.is_object()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "tool_call entry must be an object";
            continue;
        }
        // id is optional in the OpenAI wire format (`call_...`) but we echo
        // it back verbatim so callers can correlate the response array with
        // whichever id scheme the LLM used.  Missing id -> empty string.
        if (entry.contains("id") && entry["id"].is_string()) {
            slot.id = entry["id"].get<std::string>();
        }
        if (!entry.contains("name") || !entry["name"].is_string() ||
            entry["name"].get<std::string>().empty()) {
            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            slot.error = "name missing or not a non-empty string";
            continue;
        }
        slot.name = entry["name"].get<std::string>();
        // arguments may be:
        //   - absent  -> default to {}
        //   - object  -> forwarded as-is
        //   - string  -> Json::parse'd; failure records "invalid arguments JSON"
        // Anything else (array/number/etc.) is rejected as malformed.
        if (!entry.contains("arguments")) {
            slot.arguments = Json::object();
        } else {
            const Json& raw = entry["arguments"];
            if (raw.is_object()) {
                slot.arguments = raw;
            } else if (raw.is_string()) {
                const std::string text = raw.get<std::string>();
                if (text.empty()) {
                    slot.arguments = Json::object();
                } else {
                    try {
                        Json parsed = Json::parse(text);
                        if (!parsed.is_object()) {
                            slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                            slot.error = "invalid arguments JSON";
                            continue;
                        }
                        slot.arguments = std::move(parsed);
                    } catch (const std::exception&) {
                        slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                        slot.error = "invalid arguments JSON";
                        continue;
                    }
                }
            } else {
                slot.status = SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                slot.error =
                    "arguments must be an object or JSON string";
                continue;
            }
        }
        slot.ready = true;
    }

    // Phase 2: worker pool.  Every worker grabs the next unclaimed slot via
    // an atomic counter (same lock-free pattern as batch_invoke_agents) and
    // runs tools_.execute directly.  Unlike the dispatch()-level tools.call
    // branch, we intentionally do NOT hold store_mutex_ during execution —
    // NativeToolRegistry::execute is const + self-synchronising, and the
    // whole point of the batch API is to run calls concurrently.  Holding
    // store_mutex_ here would serialise every worker back onto one thread
    // and defeat the concurrency knob.
    const auto batch_start = std::chrono::steady_clock::now();
    std::atomic<size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&] {
            while (true) {
                const size_t idx =
                    next_index.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= total) {
                    break;
                }
                Slot& slot = slots[idx];
                if (!slot.ready) {
                    continue;  // phase 1 already recorded status/error
                }
                const auto step_start = std::chrono::steady_clock::now();
                try {
                    Json tool_result;
                    const int32_t status = tools_.execute(
                        mode, slot.name, slot.arguments, tool_result);
                    slot.status = status;
                    if (status == SAO_AI_EDITOR_OK) {
                        slot.result_payload = std::move(tool_result);
                    } else {
                        // Propagate whatever `details` the tool registry
                        // returned (e.g. validationErrors) into the error
                        // path so callers still get structured feedback.
                        slot.result_payload = std::move(tool_result);
                        slot.error = status_message(status);
                    }
                } catch (const std::exception& ex) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = std::string("exception: ") + ex.what();
                } catch (...) {
                    slot.status = SAO_AI_EDITOR_ERR_HTTP;
                    slot.error = "unknown exception";
                }
                const auto step_end = std::chrono::steady_clock::now();
                slot.duration_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        step_end - step_start).count());
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto batch_end = std::chrono::steady_clock::now();
    const uint64_t total_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            batch_end - batch_start).count());

    // Phase 3: assemble the response in original input order.  Runs on the
    // caller thread so no synchronisation is needed even though the workers
    // may have completed out of order.
    Json results = Json::array();
    size_t success = 0;
    size_t failure = 0;
    for (size_t i = 0; i < total; ++i) {
        const Slot& slot = slots[i];
        Json entry_result{{"id", slot.id},
                          {"name", slot.name},
                          {"durationMs", slot.duration_ms}};
        if (slot.status == SAO_AI_EDITOR_OK && slot.ready) {
            entry_result["status"] = "completed";
            entry_result["result"] = slot.result_payload;
            ++success;
        } else {
            entry_result["status"] = "failed";
            entry_result["error"] = slot.error.empty()
                                        ? status_message(slot.status)
                                        : slot.error;
            // Surface tool-registry details (validationErrors etc.) even on
            // failure so LLM-facing UI can highlight the offending field.
            if (slot.result_payload.is_object() &&
                !slot.result_payload.empty()) {
                entry_result["details"] = slot.result_payload;
            }
            ++failure;
        }
        results.push_back(std::move(entry_result));
    }
    result = Json{{"results", std::move(results)},
                  {"successCount", success},
                  {"failureCount", failure},
                  {"totalMs", total_ms}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::dispatch_prompt(std::string_view method,
                                        const Json& params, Json& result) {
    if (method == "prompts.list_defs") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        // Optional tag filter: OR semantics — a prompt survives if it has
        // at least one tag in `params.tags`.  Missing / empty / non-array
        // falls back to the full list (backward compatible).
        std::vector<std::string> tag_filter;
        if (params.is_object() && params.contains("tags") &&
            params["tags"].is_array()) {
            for (const auto& item : params["tags"]) {
                if (item.is_string()) {
                    const std::string tag = item.get<std::string>();
                    if (!tag.empty()) {
                        tag_filter.push_back(tag);
                    }
                }
            }
        }
        Json items = Json::array();
        const auto prompts = tag_filter.empty()
                                 ? prompt_registry_.list()
                                 : prompt_registry_.list_by_tags(tag_filter);
        for (const auto& prompt : prompts) {
            items.push_back(prompt.to_json());
        }
        result = Json{{"items", std::move(items)}};
        result["total"] = result["items"].size();
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.list_tags") {
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        prompt_registry_.list_all_tags(result);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "prompts.render_batch") {
        if (!params.is_object() || !params.contains("items") ||
            !params["items"].is_array() || params["items"].empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // onMissing controls whether an unknown prompt id aborts the batch
        // ("fail" — INVALID_ARGUMENT so callers get a clear surface) or
        // just records a per-item not_found (skip).  Same shape as MCP
        // batch tool-call semantics elsewhere in the runtime.
        const std::string on_missing = params.value("onMissing",
                                                     std::string{"fail"});
        if (on_missing != "fail" && on_missing != "skip") {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> guard(store_mutex_);
            prompt_registry_.reload(scopes_);
        }
        Json results = Json::array();
        size_t success = 0;
        size_t failure = 0;
        for (const auto& entry : params["items"]) {
            if (!entry.is_object() || !entry.contains("id") ||
                !entry["id"].is_string()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            const std::string id = entry["id"].get<std::string>();
            Json arguments = entry.value("arguments", Json::object());
            if (!arguments.is_object()) {
                return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
            }
            PromptDefinition prompt;
            if (!prompt_registry_.get(id, prompt)) {
                if (on_missing == "fail") {
                    return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
                }
                results.push_back(Json{{"id", id},
                                         {"status", "not_found"},
                                         {"error", "prompt id not found"}});
                ++failure;
                continue;
            }
            results.push_back(Json{{"id", prompt.id},
                                     {"content", prompt.render(arguments)},
                                     {"status", "ok"}});
            ++success;
        }
        result = Json{{"results", std::move(results)},
                       {"successCount", success},
                       {"failureCount", failure}};
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
    if (method == "prompt.pin" || method == "prompt.unpin") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        // Reload before mutating so the registry reflects any external
        // edits before we consult the entry's scope key.  set_pinned()
        // is scope-aware and reuses save() to persist the toggled flag,
        // so no additional bookkeeping is required here.
        std::lock_guard<std::mutex> guard(store_mutex_);
        prompt_registry_.reload(scopes_);
        const bool pinned = (method == "prompt.pin");
        return prompt_registry_.set_pinned(
            params["id"].get<std::string>(), pinned, scopes_, result);
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
    // vscode.window.createWebviewPanel — register a fresh WebviewPanel
    // record and emit an event so the host UI (WebView2 bridge or Tk
    // fallback shim) can materialise a window.  Returns a JSON snapshot
    // of the new panel; the id is deterministic within a runtime instance.
    if (method == "vscode.window.createWebviewPanel") {
        const std::string view_type =
            params.value("viewType", std::string{});
        if (view_type.empty()) {
            result = Json{{"message", "viewType is required"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelOptions options;
        if (params.contains("options") && params["options"].is_object()) {
            const Json& opts = params["options"];
            options.enable_scripts =
                opts.value("enableScripts", true);
            options.retain_context_when_hidden =
                opts.value("retainContextWhenHidden", false);
            options.view_column = opts.value("viewColumn", 1);
            options.extras = opts.value("extras", Json::object());
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.create(
            params.value("panelId", std::string{}),
            view_type,
            params.value("title", std::string{}),
            options,
            state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "createWebviewPanel failed"}};
            return status;
        }
        emit("vscode.window.webviewPanel.created",
             panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.revealWebviewPanel") {
        const std::string panel_id =
            params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.reveal(
            panel_id,
            params.value("viewColumn", 1),
            params.value("preserveFocus", false),
            state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "reveal failed"},
                          {"panelId", panel_id}};
            return status;
        }
        emit("vscode.window.webviewPanel.revealed",
             panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.disposeWebviewPanel") {
        const std::string panel_id =
            params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.dispose(panel_id, state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "dispose failed"},
                          {"panelId", panel_id}};
            return status;
        }
        emit("vscode.window.webviewPanel.disposed",
             panel_state_to_json(state));
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.webview.postMessage") {
        const std::string panel_id =
            params.value("panelId", std::string{});
        const std::string view_id =
            params.value("viewId", std::string{});
        if (panel_id.empty() && view_id.empty()) {
            result = Json{{"message", "panelId or viewId is required"}};
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        if (!panel_id.empty()) {
            WebviewPanelState state;
            const int32_t status =
                webview_panels_.note_post_message(panel_id, state);
            if (status != SAO_AI_EDITOR_OK) {
                emit("vscode.window.webviewPanel.postFailed",
                     Json{{"panelId", panel_id},
                          {"status", status},
                          {"reason", status == SAO_AI_EDITOR_ERR_NOT_FOUND
                                         ? "unknown panel"
                                         : "panel disposed"}});
                result = Json{{"accepted", false},
                              {"message", "postMessage failed"},
                              {"panelId", panel_id}};
                return status;
            }
            Json bridge_params = params;
            bridge_params["messageSeq"] =
                static_cast<int64_t>(state.message_seq);
            const int32_t bridge_status = dispatch_webview_message_to_page(
                bridge_params, result);
            if (bridge_status != SAO_AI_EDITOR_OK) {
                emit("vscode.window.webviewPanel.postFailed",
                     Json{{"panelId", panel_id},
                          {"status", bridge_status},
                          {"reason", "webview bridge is not ready"}});
                result["accepted"] = false;
                return bridge_status;
            }
            emit("vscode.webview.postMessage", params);
            result = Json{{"accepted", true},
                          {"panelId", panel_id},
                          {"viewId", view_id}};
            return SAO_AI_EDITOR_OK;
        }
        result = Json{{"accepted", false},
                      {"message", "webview panel is not registered"},
                      {"panelId", panel_id},
                      {"viewId", view_id}};
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (method == "vscode.window.postMessageToWebview") {
        const std::string panel_id =
            params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status =
            webview_panels_.note_post_message(panel_id, state);
        if (status != SAO_AI_EDITOR_OK) {
            emit("vscode.window.webviewPanel.postFailed",
                 Json{{"panelId", panel_id},
                      {"status", status},
                      {"reason", status == SAO_AI_EDITOR_ERR_NOT_FOUND
                                     ? "unknown panel"
                                     : "panel disposed"}});
            result = Json{{"message", "postMessage failed"},
                          {"panelId", panel_id}};
            return status;
        }
        const Json payload = params.value("message", Json());
        Json bridge_result;
        const int32_t bridge_status = dispatch_webview_message_to_extension(
            params, bridge_result);
        if (bridge_status != SAO_AI_EDITOR_OK) {
              emit("vscode.window.webviewPanel.postFailed",
                  Json{{"panelId", panel_id},
                      {"status", bridge_status},
                      {"reason", "extension host is not ready"}});
            result = Json{{"accepted", false},
                          {"message", "extension host is not ready"},
                          {"panelId", panel_id},
                          {"bridgeStatus", bridge_status}};
            return bridge_status;
        }
        if (!bridge_result.value("ok", false)) {
            result = Json{{"accepted", false},
                          {"message", "webview message was not delivered"},
                          {"panelId", panel_id},
                          {"bridgeResult", bridge_result}};
            return SAO_AI_EDITOR_ERR_IPC_CLOSED;
        }
        emit("vscode.window.webviewPanel.postMessage",
             Json{{"panelId", panel_id},
                  {"messageSeq",
                   static_cast<int64_t>(state.message_seq)},
                  {"message", payload}});
        result = Json{{"panelId", panel_id},
                      {"messageSeq",
                       static_cast<int64_t>(state.message_seq)},
                      {"accepted", true}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.setWebviewHtml") {
        const std::string panel_id =
            params.value("panelId", std::string{});
        if (panel_id.empty()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        WebviewPanelState state;
        const int32_t status = webview_panels_.set_html(
            panel_id,
            params.value("html", std::string{}),
            state);
        if (status != SAO_AI_EDITOR_OK) {
            result = Json{{"message", "setWebviewHtml failed"},
                          {"panelId", panel_id}};
            return status;
        }
        emit("vscode.window.webviewPanel.htmlChanged",
             Json{{"panelId", panel_id},
                  {"htmlLength",
                   static_cast<int64_t>(state.html.size())}});
        result = panel_state_to_json(state);
        return SAO_AI_EDITOR_OK;
    }
    if (method == "vscode.window.listWebviewPanels") {
        Json array = Json::array();
        for (const auto& panel : webview_panels_.list_alive()) {
            array.push_back(panel_state_to_json(panel));
        }
        result = Json{{"panels", std::move(array)},
                      {"totalCreated",
                       static_cast<int64_t>(webview_panels_.total_created())}};
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

int32_t NativeRuntime::dispatch_webview_message_to_extension(
    const Json& params, Json& result) {
    if (extension_host_ == nullptr) {
        result = Json{{"accepted", false},
                      {"message", "extension host is not initialized"}};
        return SAO_AI_EDITOR_ERR_NOT_INITIALIZED;
    }
    const int32_t status =
        extension_host_->post_webview_message(params, result);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    return result.value("ok", false) ? SAO_AI_EDITOR_OK
                                      : SAO_AI_EDITOR_ERR_IPC_CLOSED;
}

int32_t NativeRuntime::dispatch_webview_message_to_page(
    const Json& params, Json& result) {
    const std::string panel_id =
        params.value("panelId", std::string{});
    const std::string view_id = params.value("viewId", std::string{});
    if (panel_id.empty() && view_id.empty()) {
        result = Json{{"accepted", false},
                      {"message", "panelId or viewId is required"}};
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    WebviewPostMessageHandler handler;
    {
        std::lock_guard<std::mutex> guard(webview_bridge_mutex_);
        handler = webview_post_message_handler_;
    }
    if (!handler || panel_id.empty()) {
        result = Json{{"accepted", false},
                      {"message", "webview bridge is not ready"},
                      {"panelId", panel_id},
                      {"viewId", view_id}};
        return SAO_AI_EDITOR_ERR_IPC_CLOSED;
    }
    const uint64_t message_seq = static_cast<uint64_t>(
        params.value("messageSeq", int64_t{0}));
    const bool accepted = handler(
        panel_id, message_seq, params.value("message", Json()));
    result = Json{{"accepted", accepted},
                  {"panelId", panel_id},
                  {"viewId", view_id}};
    return accepted ? SAO_AI_EDITOR_OK : SAO_AI_EDITOR_ERR_IPC_CLOSED;
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
    // Forward every mainstream OpenAI-compat sampling knob straight into the
    // request body.  Missing fields are silently dropped so callers only pay
    // for what they set; per-provider translation (Anthropic max_tokens,
    // Gemini generationConfig, etc.) happens inside build_provider_request
    // so unsupported knobs get pruned rather than smuggled through as
    // provider-illegal keys.
    for (const std::string_view field :
         {"temperature", "max_tokens", "top_p", "top_k", "frequency_penalty",
          "presence_penalty", "stop", "seed", "logit_bias", "logprobs",
          "top_logprobs", "n", "user", "tools", "tool_choice",
          "response_format", "parallel_tool_calls"}) {
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
    // Retry policy precedence: chat.run params.retry > provider.retry >
    // default (max_attempts=3).  run_chat_sync has no runId to attach
    // chat.retry events to, so retries are silent from the caller's view
    // but still honour the same backoff / status matrix as start_chat.
    if (params.contains("retry")) {
        request.retry = RetryPolicy::from_json(params["retry"]);
    } else if (provider.contains("retry")) {
        request.retry = RetryPolicy::from_json(provider["retry"]);
    }
    ChatCancellation cancellation;
    std::string streamed;
    Json transport_result;
    const int32_t chat_status = perform_openai_chat_with_retry(
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
        RetryNotifyCallback{},  // no runId available at this layer
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
        // Persist completed runs into the workspace history root, mirroring
        // the chat_history layout so users get a durable record across
        // process restarts.  ScopeStore always has a workspace scope
        // available here (initialize() ran before dispatch), so the empty
        // path branch that disables persistence is reserved for direct
        // WorkflowExecution consumers (i.e. unit tests).
        const std::filesystem::path history_dir =
            workflow_history_root(scopes_, "workspace");
        execution->start(*this, std::move(provider), model, chat_timeout,
                          history_dir);
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
    // Mirror run_chat_sync's full sampling-knob forward list so streaming
    // start_chat clients see the same params surface as the sync helper.
    // build_provider_request handles provider-specific translation/pruning.
    for (const std::string_view field :
         {"temperature", "max_tokens", "top_p", "top_k", "frequency_penalty",
          "presence_penalty", "stop", "seed", "logit_bias", "logprobs",
          "top_logprobs", "n", "user", "tools", "tool_choice",
          "response_format", "parallel_tool_calls"}) {
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
    // Retry policy precedence: chat.run params.retry > provider.retry >
    // default (max_attempts=3).  execute_chat picks this up and emits
    // chat.retry via the RetryNotifyCallback before each backoff sleep.
    if (params.contains("retry")) {
        request.retry = RetryPolicy::from_json(params["retry"]);
    } else if (provider.contains("retry")) {
        request.retry = RetryPolicy::from_json(provider["retry"]);
    }
    // Best-effort pricing lookup: the pricing map is keyed by
    // (provider_type, model), so we consult it here — winhttp_chat itself
    // stays free of the pricing map.  Missing entry leaves
    // `request.pricing_rule` empty which perform_openai_chat interprets as
    // `pricingApplied:false, costUsd:0`.
    resolve_pricing_rule(request.provider_type, model, request);
    auto run = std::make_shared<RunState>();
    run->id = new_run_id();
    run->cancellation = std::make_shared<ChatCancellation>();
    run->provider_type = request.provider_type;
    run->model = model;
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
    const uint32_t max_attempts = std::max<uint32_t>(request.retry.max_attempts, 1);
    const int32_t status = perform_openai_chat_with_retry(
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
        [&](uint32_t attempt, uint32_t delay_ms,
            std::string_view reason) {
            // Fire chat.retry *before* the sleep so subscribers can start
            // their own timers.  Emitting even when the run has been
            // superseded is safe — is_current() gates chat.delta but the
            // retry event is a runtime-wide signal and we still want it
            // visible for observability dashboards.
            emit("chat.retry",
                 Json{{"runId", run->id},
                      {"attempt", attempt},
                      {"maxAttempts", max_attempts},
                      {"delayMs", delay_ms},
                      {"reason", std::string(reason)}},
                 run->id);
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
            // Feed the runtime-wide accumulator so chat.cost_stats picks up
            // this run.  We accumulate regardless of whether a pricing rule
            // was applied — token counts stay useful even without cost.
            accumulate_cost_stats(run->provider_type, run->model,
                                  metrics_payload);
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

// Percent-encode a UTF-8 string for use inside a URI component.  RFC 3986
// unreserved set is left untouched; every other byte is upper-hex encoded.
// This matches the "reserved-safe" behaviour required by RFC 6570's simple
// (`{name}`) template expansion.
std::string percent_encode_component(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value) {
        const bool unreserved = (byte >= 'A' && byte <= 'Z') ||
                                (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') ||
                                byte == '-' || byte == '_' || byte == '.' ||
                                byte == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4) & 0x0FU]);
            encoded.push_back(hex[byte & 0x0FU]);
        }
    }
    return encoded;
}

// RFC 6570 reserved-expansion: keep gen-delims / sub-delims and any existing
// pct-triplet intact so `{+path}` inside `sao://workspace/{+path}` carries
// `foo/bar` as literal `foo/bar` rather than `foo%2Fbar`.  Everything outside
// the "reserved + unreserved + pct-encoded" set is still percent-encoded.
std::string percent_encode_reserved(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') ||
                                (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') ||
                                byte == '-' || byte == '_' || byte == '.' ||
                                byte == '~';
        // Reserved characters kept literal (RFC 3986 gen-delims + sub-delims).
        const bool reserved = byte == ':' || byte == '/' || byte == '?' ||
                              byte == '#' || byte == '[' || byte == ']' ||
                              byte == '@' || byte == '!' || byte == '$' ||
                              byte == '&' || byte == '\'' || byte == '(' ||
                              byte == ')' || byte == '*' || byte == '+' ||
                              byte == ',' || byte == ';' || byte == '=';
        // Preserve existing pct-triplets so already-encoded input is not
        // double-encoded when it round-trips through `{+var}`.
        if (byte == '%' && index + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[index + 2]))) {
            encoded.push_back('%');
            encoded.push_back(value[index + 1]);
            encoded.push_back(value[index + 2]);
            index += 2;
            continue;
        }
        if (unreserved || reserved) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4) & 0x0FU]);
            encoded.push_back(hex[byte & 0x0FU]);
        }
    }
    return encoded;
}

// Extract the string form of any JSON scalar for URI templating.  Objects /
// arrays are stringified as JSON so callers get a deterministic (if ugly)
// value instead of the C++ default "true/false" surprise.
std::string uri_variable_value(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_null()) {
        return {};
    }
    return value.dump();
}

// Minimal RFC 6570 URI-template renderer.  Supports the simple (`{var}`) and
// reserved (`{+var}`) operators, which is what MCP resource templates use in
// practice today.  Anything else (fragment `#`, path-segment `/`, form-style
// `?`, `.` prefixed, etc.) is left as-is so callers see the raw template and
// can spot bugs in their schema — better than silently rewriting `{?q}` to
// an empty string.
std::string render_uri_template(std::string_view templ, const Json& arguments) {
    std::string output;
    output.reserve(templ.size());
    size_t index = 0;
    while (index < templ.size()) {
        const char current = templ[index];
        if (current != '{') {
            output.push_back(current);
            ++index;
            continue;
        }
        const size_t close = templ.find('}', index + 1);
        if (close == std::string_view::npos) {
            // Unmatched `{` — preserve the remainder verbatim.
            output.append(templ.substr(index));
            break;
        }
        std::string_view expr = templ.substr(index + 1, close - index - 1);
        if (expr.empty()) {
            output.append(templ.substr(index, close - index + 1));
            index = close + 1;
            continue;
        }
        bool reserved_expansion = false;
        if (expr.front() == '+') {
            reserved_expansion = true;
            expr.remove_prefix(1);
        } else if (expr.front() == '#' || expr.front() == '/' ||
                   expr.front() == '.' || expr.front() == ';' ||
                   expr.front() == '?' || expr.front() == '&') {
            // Unsupported operator — preserve verbatim.
            output.append(templ.substr(index, close - index + 1));
            index = close + 1;
            continue;
        }
        const std::string name(expr);
        std::string value;
        if (arguments.is_object() && arguments.contains(name)) {
            value = uri_variable_value(arguments[name]);
        }
        output.append(reserved_expansion ? percent_encode_reserved(value)
                                         : percent_encode_component(value));
        index = close + 1;
    }
    return output;
}

int32_t render_resource_uri(const Json& params, Json& result) {
    if (!params.contains("template") || !params["template"].is_string()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string templ = params["template"].get<std::string>();
    // `arguments` is optional — an absent / non-object payload is treated as
    // "no variables set", which produces the template with `{name}` expanded
    // to empty strings.  Matches RFC 6570 §3.2.1 behaviour for undefined vars.
    Json arguments = Json::object();
    if (params.contains("arguments") && params["arguments"].is_object()) {
        arguments = params["arguments"];
    }
    result = Json{{"uri", render_uri_template(templ, arguments)}};
    return SAO_AI_EDITOR_OK;
}

// Compose the "<provider>|<model>" key used by both pricing_rules_ and
// cost_stats_.  Anything missing collapses to an empty component (never
// synthesises a placeholder) so unknown-provider / unknown-model rows
// stay disambiguated on lookup.
std::string pricing_key(std::string_view provider, std::string_view model) {
    std::string key;
    key.reserve(provider.size() + 1 + model.size());
    key.append(provider);
    key.push_back('|');
    key.append(model);
    return key;
}

// Read a number-typed field off a metrics object, falling back to zero on
// missing / non-numeric so cost_stats accumulation stays defensive
// against partial provider payloads.
int64_t metrics_int64(const Json& metrics, const char* key) {
    const auto it = metrics.find(key);
    if (it == metrics.end() || !it->is_number()) {
        return 0;
    }
    if (it->is_number_integer()) {
        return it->get<int64_t>();
    }
    return static_cast<int64_t>(it->get<double>());
}

double metrics_double(const Json& metrics, const char* key) {
    const auto it = metrics.find(key);
    if (it == metrics.end() || !it->is_number()) {
        return 0.0;
    }
    return it->get<double>();
}

}  // namespace

int32_t NativeRuntime::set_pricing(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string provider = params.value("provider", std::string{});
    const std::string model = params.value("model", std::string{});
    if (provider.empty() || model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json rule = Json::object();
    // Store the rule as a plain object with the two numeric fields we
    // recognise.  Extra caller-supplied fields are dropped so downstream
    // cost math has a stable, minimal shape.  At least one of the two
    // fields must parse as a number — an empty rule is rejected so
    // set_pricing never silently registers a no-op entry.
    bool has_any = false;
    if (params.contains("promptPer1K") && params["promptPer1K"].is_number()) {
        rule["promptPer1K"] = params["promptPer1K"].get<double>();
        has_any = true;
    }
    if (params.contains("completionPer1K") &&
        params["completionPer1K"].is_number()) {
        rule["completionPer1K"] = params["completionPer1K"].get<double>();
        has_any = true;
    }
    if (!has_any) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = pricing_key(provider, model);
    {
        std::lock_guard<std::mutex> lock(pricing_mutex_);
        pricing_rules_[key] = rule;
    }
    result = Json{{"ok", true},
                  {"provider", provider},
                  {"model", model},
                  {"rule", std::move(rule)}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::get_pricing(const Json& params, Json& result) {
    if (!params.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string provider = params.value("provider", std::string{});
    const std::string model = params.value("model", std::string{});
    if (provider.empty() || model.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    const std::string key = pricing_key(provider, model);
    Json rule;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(pricing_mutex_);
        const auto it = pricing_rules_.find(key);
        if (it != pricing_rules_.end()) {
            rule = it->second;
            found = true;
        }
    }
    result = Json{{"provider", provider}, {"model", model}, {"found", found}};
    if (found) {
        result["rule"] = std::move(rule);
    }
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::list_pricing(const Json& /*params*/, Json& result) {
    Json items = Json::array();
    {
        std::lock_guard<std::mutex> lock(pricing_mutex_);
        for (const auto& [key, rule] : pricing_rules_) {
            const auto pipe = key.find('|');
            if (pipe == std::string::npos) {
                continue;
            }
            items.push_back(Json{
                {"provider", key.substr(0, pipe)},
                {"model", key.substr(pipe + 1)},
                {"rule", rule},
            });
        }
    }
    result = Json{{"items", std::move(items)}};
    return SAO_AI_EDITOR_OK;
}

int32_t NativeRuntime::cost_stats(const Json& params, Json& result) {
    // `days` is accepted for forward compatibility but the current
    // in-process accumulator has no time bucketing — restarting drops the
    // history.  We still surface the value on the response so callers can
    // sanity-check what they asked for.
    int64_t days = 7;
    if (params.is_object() && params.contains("days") &&
        params["days"].is_number_integer()) {
        days = params["days"].get<int64_t>();
    }
    uint64_t total_requests = 0;
    int64_t total_prompt = 0;
    int64_t total_completion = 0;
    double total_cost = 0.0;
    Json by_model = Json::object();
    // Roll up per-provider aggregates from the same rows so callers can
    // slice by vendor without walking `byModel`.  Kept parallel to
    // byModel/CostStatsRow so both views stay lookup-compatible.
    struct ProviderAgg {
        uint64_t requests = 0;
        int64_t prompt_tokens = 0;
        int64_t completion_tokens = 0;
        double cost_usd = 0.0;
    };
    std::unordered_map<std::string, ProviderAgg> provider_totals;
    {
        std::lock_guard<std::mutex> lock(cost_stats_mutex_);
        for (const auto& [key, row] : cost_stats_) {
            total_requests += row.requests;
            total_prompt += row.prompt_tokens;
            total_completion += row.completion_tokens;
            total_cost += row.cost_usd;
            // Public key on the response uses the model name so callers can
            // slice quickly; the provider is stashed inside so lookups can
            // still disambiguate identical model names across vendors.
            Json entry{
                {"provider", row.provider},
                {"requests", row.requests},
                {"promptTokens", row.prompt_tokens},
                {"completionTokens", row.completion_tokens},
                {"costUsd", row.cost_usd},
            };
            by_model[row.model] = std::move(entry);
            auto& agg = provider_totals[row.provider];
            agg.requests += row.requests;
            agg.prompt_tokens += row.prompt_tokens;
            agg.completion_tokens += row.completion_tokens;
            agg.cost_usd += row.cost_usd;
        }
    }
    Json by_provider = Json::object();
    for (const auto& [provider, agg] : provider_totals) {
        by_provider[provider] = Json{
            {"requests", agg.requests},
            {"promptTokens", agg.prompt_tokens},
            {"completionTokens", agg.completion_tokens},
            {"costUsd", agg.cost_usd},
        };
    }
    Json average = Json::object();
    if (total_requests > 0) {
        average["promptTokens"] =
            static_cast<int64_t>(total_prompt /
                                  static_cast<int64_t>(total_requests));
        average["completionTokens"] =
            static_cast<int64_t>(total_completion /
                                  static_cast<int64_t>(total_requests));
        average["costUsd"] =
            total_cost / static_cast<double>(total_requests);
    } else {
        average["promptTokens"] = 0;
        average["completionTokens"] = 0;
        average["costUsd"] = 0.0;
    }
    result = Json{
        {"days", days},
        {"totalRequestsSampled", total_requests},
        {"totalPromptTokens", total_prompt},
        {"totalCompletionTokens", total_completion},
        {"totalCostUsd", total_cost},
        {"byModel", std::move(by_model)},
        {"byProvider", std::move(by_provider)},
        {"averagePerRequest", std::move(average)},
    };
    return SAO_AI_EDITOR_OK;
}

void NativeRuntime::resolve_pricing_rule(std::string_view provider_type,
                                         std::string_view model,
                                         HttpChatRequest& request) const {
    if (provider_type.empty() || model.empty()) {
        return;
    }
    const std::string key = pricing_key(provider_type, model);
    std::lock_guard<std::mutex> lock(pricing_mutex_);
    const auto it = pricing_rules_.find(key);
    if (it != pricing_rules_.end()) {
        request.pricing_rule = it->second;
    }
}

void NativeRuntime::accumulate_cost_stats(std::string_view provider_type,
                                          std::string_view model,
                                          const Json& metrics) {
    if (!metrics.is_object()) {
        return;
    }
    const int64_t prompt = metrics_int64(metrics, "promptTokens");
    const int64_t completion = metrics_int64(metrics, "completionTokens");
    const double cost = metrics_double(metrics, "costUsd");
    const std::string key = pricing_key(provider_type, model);
    std::lock_guard<std::mutex> lock(cost_stats_mutex_);
    auto& row = cost_stats_[key];
    if (row.model.empty()) {
        row.provider.assign(provider_type);
        row.model.assign(model);
    }
    ++row.requests;
    row.prompt_tokens += prompt;
    row.completion_tokens += completion;
    row.cost_usd += cost;
}

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
    if (method == "mcp.list_resource_templates") {
        // MCP `resources/templates/list` aggregation.  The underlying C client
        // does not yet expose a dedicated primitive for templates — surface a
        // well-formed but empty response so callers can adopt this dispatch
        // method today and start receiving real templates once the client
        // gains the primitive.  Down-stream code should treat an empty array
        // as "no template metadata available", not "no template exists".
        (void)raw;
        result = Json{{"items", Json::array()}, {"total", 0}};
        return SAO_AI_EDITOR_OK;
    }
    if (method == "mcp.render_resource_uri") {
        // Local-only URI template rendering.  No network / MCP round-trip —
        // pure string substitution against the caller-supplied `arguments`
        // map, so an offline UI can pre-compute the URI for `mcp.read_resource`.
        (void)raw;
        return render_resource_uri(params, result);
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

namespace sao::ai_editor::native {

RuntimeLease::RuntimeLease(SaoAiEditorRuntime* handle) noexcept
    : handle_(handle) {
    if (handle_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(handle_->lifetime_mutex);
    if (handle_->destroying || handle_->implementation == nullptr) {
        handle_ = nullptr;
        return;
    }
    ++handle_->leases;
    runtime_ = handle_->implementation.get();
}

RuntimeLease::~RuntimeLease() {
    if (handle_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(handle_->lifetime_mutex);
    if (handle_->leases > 0) {
        --handle_->leases;
    }
    if (handle_->destroying && handle_->leases == 0) {
        handle_->lifetime_ready.notify_all();
    }
}

}  // namespace sao::ai_editor::native

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
        sao::ai_editor::native::RuntimeLease lease(handle);
        auto* runtime = lease.get();
        if (runtime == nullptr) {
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
                runtime->dispatch(request, response);
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
        sao::ai_editor::native::RuntimeLease lease(handle);
        auto* runtime = lease.get();
        if (runtime == nullptr) {
            return SAO_AI_EDITOR_ERR_HANDLE_INVALID;
        }
        if (out_len == nullptr) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(handle->event_mutex);
        if (handle->pending_event.empty()) {
            const int32_t status =
                runtime->next_event(0, handle->pending_event);
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
        sao::ai_editor::native::RuntimeLease lease(handle);
        auto* runtime = lease.get();
        if (runtime == nullptr) {
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
            runtime->dispatch(request, response);
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
        if (handle == nullptr) {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(handle->lifetime_mutex);
            handle->destroying = true;
            handle->lifetime_ready.wait(lock, [handle] {
                return handle->leases == 0;
            });
        }
        delete handle;
    } catch (...) {
    }
}
