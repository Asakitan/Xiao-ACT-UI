#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent_registry.h"
#include "auth_device_flow.h"
#include "conversation_store.h"
#include "extension_host.h"
#include "native_secret_store.h"
#include "native_tool_registry.h"
#include "prompt_registry.h"
#include "tool_execution_monitor.h"
#include "tool_result_cache.h"
#include "tool_result_filter.h"
#include "webview_panel_registry.h"
#include "sao/ai_editor/mcp_client.h"
#include "winhttp_chat.h"
#include "workflow_engine.h"

namespace sao::ai_editor::native {

struct RuntimeOptions final {
    std::string workspace_root;
    std::string system_root;
    std::string plugin_roots_json;
    uint32_t maximum_event_queue = 256;
    uint32_t maximum_file_bytes = kDefaultMaximumFileBytes;
    uint32_t maximum_search_results = kDefaultSearchResults;
};

class WorkflowExecution;

class NativeRuntime final {
public:
    friend class WorkflowExecution;
    explicit NativeRuntime(RuntimeOptions options);
    ~NativeRuntime();

    NativeRuntime(const NativeRuntime&) = delete;
    NativeRuntime& operator=(const NativeRuntime&) = delete;

    int32_t initialize();
    int32_t dispatch(const Json& request, Json& response);
    int32_t next_event(uint32_t timeout_ms, std::string& event_json);

private:
    struct RunState final {
        std::string id;
        std::string status = "running";
        uint64_t generation = 0;
        std::shared_ptr<ChatCancellation> cancellation;
        std::thread worker;
        Json result = Json::object();
        std::string error;
        // Provider type + model captured at run-time so execute_chat can
        // credit the cost accumulator to the right (provider, model) row
        // even after the HttpChatRequest is moved out of scope.
        std::string provider_type;
        std::string model;
    };

    int32_t invoke(std::string_view method,
                   const Json& params,
                   Json& result);
    int32_t start_chat(const Json& params, Json& result);
    int32_t cancel_run(const Json& params, Json& result);
    int32_t run_status(const Json& params, Json& result);
    // chat.set_pricing / chat.get_pricing / chat.list_pricing / chat.cost_stats
    // — in-process pricing rules and per-model cost accumulation.  Storage is
    // an unordered_map keyed by "<provider>|<model>" so a caller can look up
    // rules deterministically; the map lives entirely inside the runtime and
    // is not persisted across process restarts (documented on the wire).
    int32_t set_pricing(const Json& params, Json& result);
    int32_t get_pricing(const Json& params, Json& result);
    int32_t list_pricing(const Json& params, Json& result);
    int32_t cost_stats(const Json& params, Json& result);
    // Resolve the injected pricing rule for a (provider, model) tuple and
    // fill `HttpChatRequest::pricing_rule` when found.  No-op if either
    // key component is empty or the map has no entry.
    void resolve_pricing_rule(std::string_view provider_type,
                              std::string_view model,
                              HttpChatRequest& request) const;
    // Called from execute_chat once the transport reports final metrics so
    // the in-process cost accumulator picks up prompt / completion / cost
    // even when the caller ignores chat.metrics.  Runs under
    // `cost_stats_mutex_`; feeds `chat.cost_stats` responses verbatim.
    void accumulate_cost_stats(std::string_view provider_type,
                               std::string_view model,
                               const Json& metrics);
    int32_t dispatch_mcp(std::string_view method,
                         const Json& params,
                         Json& result);
    // Aggregate MCP tools filtered by `mcp_server_filter` (JSON array of names;
    // if empty/absent -> all registered servers) and emit them as OpenAI-shape
    // `function` tools with `mcp__<server>__<name>` naming.  Returns an empty
    // array when no MCP client is registered / servers are configured.
    int32_t collect_mcp_openai_tools(const Json& mcp_server_filter,
                                     Json& out_tools);
    // chat.run_with_mcp / agents.invoke_with_mcp — thin wrappers that inject
    // collected MCP tools into params.tools before delegating to start_chat /
    // dispatch_agent("agents.invoke").
    int32_t start_chat_with_mcp(const Json& params, Json& result);
    int32_t agent_invoke_with_mcp(const Json& params, Json& result);
    // Resolve `systemPromptSource:{server,name,arguments?}` by calling the
    // registered MCP server's prompts/get and prepending the returned
    // messages (and optional description as a system header) to
    // `params.messages`.  Returns SAO_AI_EDITOR_OK when the source field is
    // absent, empty, or successfully rendered; propagates the get_prompt
    // failure otherwise (never silently swallowed).
    int32_t apply_system_prompt_source(Json& params);
    int32_t dispatch_workflow(std::string_view method,
                              const Json& params,
                              Json& result);
    int32_t dispatch_auth(std::string_view method,
                          const Json& params,
                          Json& result);
    int32_t dispatch_extension(std::string_view method,
                               const Json& params,
                               Json& result);
    int32_t dispatch_agent(std::string_view method,
                           const Json& params,
                           Json& result);
    // agents.batch_invoke — fan-out multiple agents in parallel (one worker
    // thread per agent, throttled by params.concurrency in [1, 16]).  Each
    // agent runs the full agents.invoke pipeline (agent lookup +
    // build_chat_messages + run_chat_sync) with per-agent overrides layered
    // over the top-level defaults (defaultProvider / defaultModel /
    // defaultMessage / timeoutMs).  Failures never abort peers; every entry
    // ends up in `results` with either {status:"completed", content, ...}
    // or {status:"failed", error, ...}.  If conversationId is supplied,
    // successful (user, assistant) turn pairs from each agent are appended
    // to the same conversation in original agent order (post-join, to keep
    // the transcript deterministic under parallelism).
    int32_t batch_invoke_agents(const Json& params, Json& result);
    // chat.dispatch_tool_calls — fan-out an assistant response's tool_calls
    // array over a worker pool (throttled by params.concurrency in [1, 16],
    // default 4).  Each entry runs tools_.execute(mode, name, arguments) so
    // the built-in permission gating (ask/plan/agent) applies uniformly.
    // LLM callers usually hand back `arguments` as a serialised JSON string;
    // both a raw object *and* a string that parses into an object are
    // accepted (parse failures are recorded per-call, never abort peers).
    // Results are assembled in input order; each entry carries
    // {id, name, status, durationMs} plus either result or error.
    int32_t dispatch_tool_calls(const Json& params, Json& result);
    int32_t dispatch_prompt(std::string_view method,
                            const Json& params,
                            Json& result);
    // Resolve `promptId` + optional `promptArguments` in a chat.run-style
    // params payload: render the referenced PromptDefinition and prepend it
    // to `params.messages` either as an extra system message (when the
    // caller has no system message yet) or as a user turn.  Returns
    // SAO_AI_EDITOR_OK when the promptId field is absent, empty, or
    // successfully rendered; propagates registry lookup failures otherwise.
    int32_t apply_prompt_source(Json& params);
    int32_t run_chat_sync(const Json& params, uint32_t timeout_ms,
                          std::string& out_content,
                          std::function<void(const Json&)> on_delta = nullptr);
    // Public helper so WorkflowExecution / other friend-classes can push
    // structured events (e.g. streaming step deltas) to the same queue as
    // emit().  Delegates to the private emit() method internally.
    void emit_workflow_event(std::string_view event_name,
                             const Json& payload,
                             std::string_view run_id = {}) {
        emit(event_name, payload, run_id);
    }
    void execute_chat(const std::shared_ptr<RunState>& run,
                      HttpChatRequest request,
                      std::string conversation_id);
    void emit(std::string_view event_name,
              const Json& payload,
              std::string_view run_id = {});
    bool is_current(const std::shared_ptr<RunState>& run) const;
    int32_t resolve_provider(const Json& params,
                             Json& provider,
                             std::string& api_key);
    static Json permission_policy(std::string_view mode);
    // C-callable trampoline that unpacks the MCP notification envelope and
    // routes it into the runtime's event queue as an "mcp.notification"
    // sao.event.
    static void SAO_AI_EDITOR_CALL mcp_notification_trampoline(
        void* user, const char* json_utf8, uint32_t json_len);

    struct McpClientDeleter final {
        void operator()(SaoAiEditorMcpClient* client) const noexcept {
            if (client != nullptr) {
                sao_ai_editor_mcp_client_destroy(client);
            }
        }
    };

    ScopeStore scopes_;
    RuntimeOptions options_;
    ConversationStore conversations_;
    NativeToolRegistry tools_;
    // Session-memory dedup cache for read-only tool calls.  Owns its own
    // mutex; safe to call from the dispatch thread without holding
    // store_mutex_.  See tool_result_cache.h for TTL / classification rules.
    ToolResultCache tool_cache_;
    // Per-tool execution telemetry (invocations, bytes returned, cache hit
    // ratio, avg duration).  Deliberately independent from tool_cache_ so a
    // `tools.cache_clear` call preserves the invocation ledger; only
    // `tools.telemetry_clear` (dispatched separately) wipes these counters.
    // See tool_execution_monitor.h.
    ToolExecutionMonitor tool_monitor_;
    // Compression chain applied to tools.execute() output before it hits the
    // JSON-RPC wire.  Populated with the 3 built-in filters
    // (ListFilesFolder / SearchFilesCollapse / ReadFileTruncate) in the
    // NativeRuntime constructor; wired into tools_ via set_filter_registry
    // so the registry can run the chain from inside execute() without the
    // dispatch layer having to remember to invoke it.
    ToolResultFilterRegistry filter_registry_;
    std::unique_ptr<SecretStore> secrets_;
    std::unique_ptr<SaoAiEditorMcpClient, McpClientDeleter> mcp_client_;
    WorkflowRegistry workflow_registry_;
    mutable std::mutex workflow_mutex_;
    std::unordered_map<std::string, std::shared_ptr<WorkflowExecution>>
        workflow_executions_;
    std::unique_ptr<AuthDeviceFlow> auth_flow_;
    std::unique_ptr<ExtensionHost> extension_host_;
    AgentRegistry agent_registry_;
    PromptRegistry prompt_registry_;
    // Metadata store for vscode.window.createWebviewPanel — see
    // webview_panel_registry.h.  Unconditionally compiled: even without
    // WebView2 the Node side needs a deterministic id + state surface.
    WebviewPanelRegistry webview_panels_;
    uint32_t maximum_event_queue_;

public:
    // Node-side vscode.* / sao.host.* callbacks; the extension shim invokes
    // these when extensions call vscode API surface.
    int32_t dispatch_extension_call(std::string_view method,
                                    const Json& params,
                                    Json& result);

    mutable std::mutex store_mutex_;
    mutable std::mutex state_mutex_;
    std::unordered_map<std::string, std::shared_ptr<RunState>> runs_;
    std::string active_run_id_;
    uint64_t generation_ = 0;

    std::mutex event_mutex_;
    std::condition_variable event_ready_;
    std::deque<std::string> events_;
    bool stopping_ = false;

    // Pricing rules keyed by "<provider>|<model>".  Each value is a JSON
    // object with promptPer1K / completionPer1K numeric fields (both
    // optional; missing fields treated as 0 during cost math).  Storage is
    // in-process only — restarting the runtime drops the map.  Guarded by
    // `pricing_mutex_`.
    mutable std::mutex pricing_mutex_;
    std::unordered_map<std::string, Json> pricing_rules_;

    // Per-model cost accumulator.  Updated once per completed chat run
    // (called from execute_chat under `cost_stats_mutex_`).  Keyed by the
    // same "<provider>|<model>" convention as `pricing_rules_` so both
    // stay lookup-compatible.
    struct CostStatsRow final {
        std::string provider;
        std::string model;
        uint64_t requests = 0;
        int64_t prompt_tokens = 0;
        int64_t completion_tokens = 0;
        double cost_usd = 0.0;
    };
    mutable std::mutex cost_stats_mutex_;
    std::unordered_map<std::string, CostStatsRow> cost_stats_;
};

}  // namespace sao::ai_editor::native
