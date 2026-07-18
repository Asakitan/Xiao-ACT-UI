#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {

class ScopeStore;

struct AgentDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string system_prompt;
    std::vector<std::string> tools;
    std::string model;
    std::string icon;
    std::string when_to_use;
    bool builtin = false;
    std::string scope{"builtin"};

    static AgentDefinition from_json(const Json& value);
    Json to_json() const;
};

// Filter bitmask for AgentRegistry::recommend — pass a bitwise-OR of these
// flags to opt scope categories in or out.  Everything defaults to on.
enum RecommendScopeFlags : uint32_t {
    kRecommendIncludeBuiltin = 1U << 0,
    kRecommendIncludeMarket = 1U << 1,
    kRecommendIncludeUser = 1U << 2,
    kRecommendIncludeAll = kRecommendIncludeBuiltin |
                          kRecommendIncludeMarket |
                          kRecommendIncludeUser,
};

class AgentRegistry {
public:
    // (Re)load built-ins + on-disk agents from every configured scope.
    void reload(const ScopeStore& scopes);
    std::vector<AgentDefinition> list() const;
    bool get(std::string_view id, AgentDefinition& out) const;
    int32_t save(const AgentDefinition& agent, const ScopeStore& scopes,
                 std::string_view scope, std::string_view plugin_id);
    int32_t remove(std::string_view id, const ScopeStore& scopes,
                   std::string_view scope, std::string_view plugin_id);

    // Snapshot agent definitions for export.  Mirrors WorkflowRegistry::
    // export_all so the dispatch layer can use the same envelope pattern.
    // scope options:
    //   "workspace" — user agents currently attached to the workspace scope
    //   "system"    — user agents currently attached to the system scope
    //   "builtin"   — the compiled-in built-in agents
    //   "market"    — market-preset agents shipped alongside the plugin
    //   "all"       — everything the registry knows about (workspace + system
    //                 + plugin:* + builtin + market).  Only user agents keep
    //                 their storage scope; built-ins retain scope="builtin"
    //                 and market presets retain scope="market".
    // Populates `result` with a sao-agents/1 envelope.
    int32_t export_all(std::string_view scope, Json& result) const;

    // Import a single agent definition into the target scope.  Mirrors
    // WorkflowRegistry::import_workflow: honours `overwrite` on conflict,
    // blocks any attempt to shadow a built-in, and yields the id that was
    // actually written.  Callers stage two passes for atomicity (see
    // dispatch_agent) so the store either fully accepts or fully rejects a
    // batch.
    int32_t import_agent(const Json& agent, std::string_view scope,
                         std::string_view plugin_id, bool overwrite,
                         const ScopeStore& scopes, std::string& out_id);

    // Look up whether an id is currently owned by a built-in; used by the
    // dispatch layer to fail atomically before any import writes happen.
    bool is_builtin(std::string_view id) const;

    // Rule-based agent recommendation for a natural-language query.  See
    // agent_registry.cpp for the scoring rubric (name > when_to_use >
    // description > tools, phrase-match bonus, boost tags).  Fills `result`
    // with { query, recommendations:[{agentId,agentName,score,reason,
    // matches}], total }.  scope_flags is a bitmask of
    // RecommendScopeFlags — kRecommendIncludeAll to opt in everything.
    // top_k <= 0 defaults to 3.
    int32_t recommend(std::string_view query,
                      int top_k,
                      uint32_t scope_flags,
                      const std::vector<std::string>& boost_tags,
                      Json& result) const;

    // Compose a role/prompt bundle for `chat.run`:
    //   [{"role":"system","content":<agent.system_prompt>},
    //    {"role":"user","content":<message>}]
    // The bundle honours `overrides.model` if set, otherwise the agent's
    // model; when neither is set the returned Json.model is empty and the
    // caller must supply the model at chat.run time.
    Json build_chat_messages(const AgentDefinition& agent,
                             std::string_view message,
                             const Json& history) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AgentDefinition> agents_;
};

}  // namespace sao::ai_editor::native
