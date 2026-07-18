#include "sao/ai_editor/ai_editor_native.h"

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
    return SAO_AI_EDITOR_OK;
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
    if (method == "conversation.delete") {
        if (!params.contains("id") || !params["id"].is_string()) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard<std::mutex> lock(store_mutex_);
        return conversations_.remove(params["id"].get<std::string>(), result);
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
        return start_chat(params, result);
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
    HttpChatRequest request{endpoint, api_key, body.dump(),
                            params.value("timeoutMs", 60'000U), stream};
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
    if (method == "mcp.call_tool" || method == "mcp.read_resource") {
        const std::string request = dump_json(params);
        auto invoker = method == "mcp.call_tool"
            ? &sao_ai_editor_mcp_client_call_tool
            : &sao_ai_editor_mcp_client_read_resource;
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
