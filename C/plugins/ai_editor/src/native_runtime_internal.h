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
    };

    int32_t invoke(std::string_view method,
                   const Json& params,
                   Json& result);
    int32_t start_chat(const Json& params, Json& result);
    int32_t cancel_run(const Json& params, Json& result);
    int32_t run_status(const Json& params, Json& result);
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
};

}  // namespace sao::ai_editor::native
