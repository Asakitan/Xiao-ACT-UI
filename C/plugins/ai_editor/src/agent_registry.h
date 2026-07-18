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
