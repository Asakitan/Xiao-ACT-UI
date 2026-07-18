#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "native_utils.h"

namespace sao::ai_editor::native {

class ScopeStore;

// Prompt library entry: a reusable prompt template with `{{var}}` placeholders
// and a matching variable schema for introspection / UI.  Modelled after
// AgentDefinition so scope/builtin ownership rules stay identical.
struct PromptDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string content;
    // Each entry is an object; the loader keeps unknown fields intact so the
    // UI can carry extra metadata without a schema change.  The runtime only
    // interprets `name` (required), `description` (optional), and `default`
    // (optional) when rendering.
    std::vector<Json> variables;
    std::vector<std::string> tags;
    std::string icon;
    bool builtin = false;
    std::string scope{"builtin"};

    static PromptDefinition from_json(const Json& value);
    Json to_json() const;

    // Substitute every `{{var}}` placeholder in `content` with:
    //   1. `arguments[var]` when present (string is used verbatim, non-string
    //      is JSON-dumped so callers can pass structured values);
    //   2. otherwise the matching variable's `default`;
    //   3. otherwise an empty string.  Unknown / unterminated braces are
    //      passed through untouched so partial matches don't corrupt output.
    std::string render(const Json& arguments) const;
};

class PromptRegistry {
public:
    // Load built-ins + market presets + on-disk prompts from every configured
    // scope. Matches AgentRegistry::reload semantics: built-ins win, market
    // presets fill in, user scopes shadow same-id entries.
    void reload(const ScopeStore& scopes);
    std::vector<PromptDefinition> list() const;
    bool get(std::string_view id, PromptDefinition& out) const;
    int32_t save(const PromptDefinition& prompt, const ScopeStore& scopes,
                 std::string_view scope, std::string_view plugin_id);
    int32_t remove(std::string_view id, const ScopeStore& scopes,
                   std::string_view scope, std::string_view plugin_id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PromptDefinition> prompts_;
};

}  // namespace sao::ai_editor::native
