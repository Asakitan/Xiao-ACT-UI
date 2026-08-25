#include "prompt_registry.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "scope_store.h"

namespace sao::ai_editor::native {
namespace {

// Duplicated (rather than shared) with agent_registry.cpp on purpose: the
// module-directory anchor must resolve against the DLL that owns *this*
// translation unit, and each file's helper is a couple of dozen lines.
// Splitting them into a shared utility would drag the market resolution
// helpers into native_utils.{h,cpp} without meaningfully saving code.
std::filesystem::path market_module_directory() {
    HMODULE module_handle = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&market_module_directory),
            &module_handle) ||
        module_handle == nullptr) {
        return {};
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            module_handle, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            std::filesystem::path path(buffer.data());
            return path.parent_path();
        }
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768) {
            return {};
        }
    }
}

std::filesystem::path resolve_market_assets_dir(std::wstring_view subdir) {
    const std::filesystem::path anchor = market_module_directory();
    if (anchor.empty()) {
        return {};
    }
    const std::wstring rel_install =
        std::wstring(L"assets/ai_editor/") + std::wstring(subdir);
    const std::wstring candidates[] = {
        rel_install,
        std::wstring(L"../") + rel_install,
        std::wstring(L"../../") + rel_install,
        std::wstring(L"../plugins/ai_editor/") + rel_install,
        std::wstring(L"../../plugins/ai_editor/") + rel_install,
        std::wstring(L"../../../plugins/ai_editor/") + rel_install,
        std::wstring(L"../../../../plugins/ai_editor/") + rel_install,
    };
    for (const auto& rel : candidates) {
        std::filesystem::path candidate = anchor / rel;
        std::error_code error;
        candidate = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            continue;
        }
        if (std::filesystem::is_directory(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

template <typename Sink>
void enumerate_market_json(const std::filesystem::path& dir, Sink&& sink) {
    if (dir.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(dir, error);
    if (error) {
        return;
    }
    for (const auto& entry : iterator) {
        std::error_code file_error;
        if (!entry.is_regular_file(file_error) || file_error) {
            continue;
        }
        const auto& path = entry.path();
        if (path.extension() != L".json") {
            continue;
        }
        std::string text;
        if (read_text_file(path, kMaximumJsonBytes, text) !=
                SAO_AI_EDITOR_OK ||
            text.empty()) {
            continue;
        }
        Json parsed = Json::parse(text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            continue;
        }
        sink(parsed);
    }
}

// Return `content` with leading/trailing ASCII whitespace stripped.  Only
// used for `{{ var }}` placeholder names — MCP-style prompt vars stay
// pristine because their keys are trimmed at declaration time.
std::string trim_ascii(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

const std::vector<PromptDefinition>& builtin_prompts() {
    static const std::vector<PromptDefinition> prompts = [] {
        std::vector<PromptDefinition> result;
        {
            PromptDefinition prompt;
            prompt.id = "code-review-base";
            prompt.name = "Code Review (Base)";
            prompt.description =
                "General-purpose code review scaffold with a focus area "
                "variable";
            prompt.content =
                "Review the following code. Focus on: {{focus}}\n\n"
                "{{code}}";
            prompt.variables.push_back(Json{
                {"name", "focus"},
                {"description", "Aspect to prioritise (bugs, security, style)"},
                {"default", "correctness and security"}});
            prompt.variables.push_back(
                Json{{"name", "code"},
                     {"description", "Source snippet to review"}});
            prompt.tags = {"review", "code"};
            prompt.icon = "\xF0\x9F\x94\x8D";  // 🔍
            prompt.builtin = true;
            prompt.scope = "builtin";
            result.push_back(std::move(prompt));
        }
        {
            PromptDefinition prompt;
            prompt.id = "explain-code";
            prompt.name = "Explain Code";
            prompt.description =
                "Walk through code with a caller-selected level of depth";
            prompt.content =
                "Explain this code:\n\n{{code}}\n\nDepth: {{depth}}";
            prompt.variables.push_back(
                Json{{"name", "code"},
                     {"description", "Snippet or file contents to explain"}});
            prompt.variables.push_back(Json{
                {"name", "depth"},
                {"description",
                 "How deep to go (overview | detailed | line-by-line)"},
                {"default", "detailed"}});
            prompt.tags = {"explain", "docs"};
            prompt.icon = "\xF0\x9F\x93\x96";  // 📖
            prompt.builtin = true;
            prompt.scope = "builtin";
            result.push_back(std::move(prompt));
        }
        {
            PromptDefinition prompt;
            prompt.id = "bug-diagnosis";
            prompt.name = "Bug Diagnosis";
            prompt.description =
                "Structured bug intake: paste the failure + surrounding "
                "context";
            prompt.content =
                "Diagnose this bug/error:\n\n{{error}}\n\n"
                "Context:\n{{context}}";
            prompt.variables.push_back(
                Json{{"name", "error"},
                     {"description",
                      "The error message, stack trace, or observed behaviour"}});
            prompt.variables.push_back(Json{
                {"name", "context"},
                {"description",
                 "Relevant source, config, or environment information"},
                {"default", "(no additional context supplied)"}});
            prompt.tags = {"debug", "diagnose"};
            prompt.icon = "\xF0\x9F\x90\x9B";  // 🐛
            prompt.builtin = true;
            prompt.scope = "builtin";
            result.push_back(std::move(prompt));
        }
        return result;
    }();
    return prompts;
}

}  // namespace

PromptDefinition PromptDefinition::from_json(const Json& value) {
    PromptDefinition prompt;
    if (!value.is_object()) {
        return prompt;
    }
    prompt.id = value.value("id", std::string{});
    prompt.name = value.value("name", std::string{});
    prompt.description = value.value("description", std::string{});
    prompt.content = value.value("content", std::string{});
    prompt.icon = value.value("icon", std::string{});
    prompt.builtin = value.value("builtin", false);
    prompt.scope = value.value("scope", std::string{"workspace"});
    // Older user prompts predate the `pinned` field; missing / non-bool
    // silently defaults to false so pre-R-current stored JSON stays valid.
    prompt.pinned = value.value("pinned", false);
    if (value.contains("variables") && value["variables"].is_array()) {
        for (const auto& item : value["variables"]) {
            if (item.is_object()) {
                prompt.variables.push_back(item);
            }
        }
    }
    if (value.contains("tags") && value["tags"].is_array()) {
        for (const auto& item : value["tags"]) {
            if (item.is_string()) {
                prompt.tags.push_back(item.get<std::string>());
            }
        }
    }
    return prompt;
}

Json PromptDefinition::to_json() const {
    Json variables_json = Json::array();
    for (const auto& variable : variables) {
        variables_json.push_back(variable);
    }
    Json tags_json = Json::array();
    for (const auto& tag : tags) {
        tags_json.push_back(tag);
    }
    return Json{{"id", id},
                {"name", name},
                {"description", description},
                {"content", content},
                {"variables", std::move(variables_json)},
                {"tags", std::move(tags_json)},
                {"icon", icon},
                {"builtin", builtin},
                {"scope", scope},
                {"pinned", pinned}};
}

std::string PromptDefinition::render(const Json& arguments) const {
    std::string output;
    output.reserve(content.size());
    size_t cursor = 0;
    const size_t size = content.size();
    while (cursor < size) {
        const size_t open = content.find("{{", cursor);
        if (open == std::string::npos) {
            output.append(content, cursor, std::string::npos);
            break;
        }
        // Copy literal chunk up to the placeholder.
        if (open > cursor) {
            output.append(content, cursor, open - cursor);
        }
        const size_t close = content.find("}}", open + 2);
        if (close == std::string::npos) {
            // Unterminated: pass through the remainder verbatim so callers
            // can distinguish it from a resolved-empty substitution.
            output.append(content, open, std::string::npos);
            break;
        }
        const std::string_view raw_name(content.data() + open + 2,
                                         close - open - 2);
        const std::string placeholder_name = trim_ascii(raw_name);
        if (placeholder_name.empty()) {
            // Empty placeholder — behave like an unknown variable and emit
            // nothing.  Advance past `}}` so we don't loop forever.
            cursor = close + 2;
            continue;
        }
        bool substituted = false;
        if (arguments.is_object() && arguments.contains(placeholder_name)) {
            const Json& value = arguments[placeholder_name];
            if (value.is_string()) {
                output.append(value.get<std::string>());
            } else if (value.is_null()) {
                // Explicit null -> treat as an empty override.
            } else {
                output.append(value.dump());
            }
            substituted = true;
        }
        if (!substituted) {
            // Fall back to the variable's declared default.  Unknown
            // variables collapse to an empty string.
            for (const auto& variable : variables) {
                if (!variable.is_object()) {
                    continue;
                }
                if (variable.value("name", std::string{}) != placeholder_name) {
                    continue;
                }
                if (variable.contains("default")) {
                    const Json& fallback = variable["default"];
                    if (fallback.is_string()) {
                        output.append(fallback.get<std::string>());
                    } else if (!fallback.is_null()) {
                        output.append(fallback.dump());
                    }
                }
                break;
            }
        }
        cursor = close + 2;
    }
    return output;
}

void PromptRegistry::reload(const ScopeStore& scopes) {
    std::lock_guard<std::mutex> guard(mutex_);
    prompts_.clear();
    for (const auto& prompt : builtin_prompts()) {
        prompts_.emplace(prompt.id, prompt);
    }
    // Market presets: install-time bundled prompts.  Load after built-ins so
    // matching ids in `assets/ai_editor/prompts/*.json` cannot shadow a
    // built-in, and before user scopes so authored copies take priority.
    enumerate_market_json(
        resolve_market_assets_dir(L"prompts"), [this](const Json& item) {
            if (!item.contains("id") || !item["id"].is_string()) {
                return;
            }
            PromptDefinition prompt = PromptDefinition::from_json(item);
            if (prompt.id.empty()) {
                return;
            }
            const auto existing = prompts_.find(prompt.id);
            if (existing != prompts_.end() && existing->second.builtin) {
                return;
            }
            prompt.builtin = false;
            prompt.scope = "market";
            prompts_[prompt.id] = std::move(prompt);
        });
    Json registry;
    if (scopes.load_registry("prompts", Json::array(), registry) !=
            SAO_AI_EDITOR_OK ||
        !registry.is_array()) {
        return;
    }
    for (const auto& item : registry) {
        if (!item.is_object() || !item.contains("id") ||
            !item["id"].is_string()) {
            continue;
        }
        PromptDefinition prompt = PromptDefinition::from_json(item);
        prompt.builtin = false;
        prompt.scope = item.value("scope", std::string{"workspace"});
        prompts_[prompt.id] = std::move(prompt);
    }
}

std::vector<PromptDefinition> PromptRegistry::list() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<PromptDefinition> result;
    result.reserve(prompts_.size());
    for (const auto& [id, prompt] : prompts_) {
        (void)id;
        result.push_back(prompt);
    }
    std::sort(result.begin(), result.end(),
              [](const PromptDefinition& lhs, const PromptDefinition& rhs) {
                  // Pinned entries float ahead of everything else so users
                  // can promote favourites past the builtin/id tie-break
                  // that would otherwise anchor them lower in the list.
                  if (lhs.pinned != rhs.pinned) {
                      return lhs.pinned;
                  }
                  if (lhs.builtin != rhs.builtin) {
                      return lhs.builtin;
                  }
                  return lhs.id < rhs.id;
              });
    return result;
}

std::vector<PromptDefinition> PromptRegistry::list_by_tags(
    const std::vector<std::string>& tags) const {
    // Empty filter degenerates to list() so callers can forward an optional
    // params.tags without a branch.  Copy the set-membership check into a
    // small unordered_set once per call to keep the intersection O(prompt_tag
    // count) rather than O(filter x prompt_tag).
    std::unordered_map<std::string, int> filter;
    for (const auto& tag : tags) {
        if (!tag.empty()) {
            filter[tag] = 1;
        }
    }
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<PromptDefinition> result;
    result.reserve(prompts_.size());
    for (const auto& [id, prompt] : prompts_) {
        (void)id;
        if (filter.empty()) {
            result.push_back(prompt);
            continue;
        }
        bool matched = false;
        for (const auto& tag : prompt.tags) {
            if (filter.count(tag) != 0) {
                matched = true;
                break;
            }
        }
        if (matched) {
            result.push_back(prompt);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const PromptDefinition& lhs, const PromptDefinition& rhs) {
                  // Same tie-break order as list(): pinned wins first,
                  // builtin second, then alphabetical id.  Kept identical
                  // so tag-filtered views mirror the untouched list order.
                  if (lhs.pinned != rhs.pinned) {
                      return lhs.pinned;
                  }
                  if (lhs.builtin != rhs.builtin) {
                      return lhs.builtin;
                  }
                  return lhs.id < rhs.id;
              });
    return result;
}

void PromptRegistry::list_all_tags(Json& result) const {
    // Aggregation: walk every prompt, bucket tag -> {count, sorted prompt
    // ids}.  Insertion-sort prompt ids as we go so the output is
    // deterministic without a second pass.  The vector<pair> layout beats
    // a nested map at this scale (single-digit prompts, low-dozens tags)
    // and keeps memory tight for the "no tags anywhere" edge case.
    struct Bucket {
        std::string name;
        size_t count = 0;
        std::vector<std::string> prompt_ids;  // kept sorted
    };
    std::unordered_map<std::string, size_t> index;
    std::vector<Bucket> buckets;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& [id, prompt] : prompts_) {
            (void)id;
            for (const auto& tag : prompt.tags) {
                if (tag.empty()) {
                    continue;
                }
                auto found = index.find(tag);
                if (found == index.end()) {
                    Bucket bucket;
                    bucket.name = tag;
                    bucket.count = 1;
                    bucket.prompt_ids.push_back(prompt.id);
                    index.emplace(tag, buckets.size());
                    buckets.push_back(std::move(bucket));
                    continue;
                }
                Bucket& bucket = buckets[found->second];
                ++bucket.count;
                // Insertion-sort so `prompts` stays deterministic and unique.
                auto insertion = std::lower_bound(
                    bucket.prompt_ids.begin(), bucket.prompt_ids.end(),
                    prompt.id);
                if (insertion == bucket.prompt_ids.end() ||
                    *insertion != prompt.id) {
                    bucket.prompt_ids.insert(insertion, prompt.id);
                }
            }
        }
    }
    std::sort(buckets.begin(), buckets.end(),
              [](const Bucket& lhs, const Bucket& rhs) {
                  if (lhs.count != rhs.count) {
                      return lhs.count > rhs.count;  // desc by count
                  }
                  return lhs.name < rhs.name;  // asc by name (tie break)
              });
    Json tags_array = Json::array();
    for (const auto& bucket : buckets) {
        Json ids_json = Json::array();
        for (const auto& id : bucket.prompt_ids) {
            ids_json.push_back(id);
        }
        tags_array.push_back(Json{{"name", bucket.name},
                                    {"count", bucket.count},
                                    {"prompts", std::move(ids_json)}});
    }
    result = Json{{"tags", std::move(tags_array)},
                  {"total", buckets.size()}};
}

bool PromptRegistry::get(std::string_view id, PromptDefinition& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = prompts_.find(std::string(id));
    if (found == prompts_.end()) {
        return false;
    }
    out = found->second;
    return true;
}

int32_t PromptRegistry::save(const PromptDefinition& prompt,
                             const ScopeStore& scopes,
                             std::string_view scope,
                             std::string_view plugin_id) {
    if (!valid_simple_id(prompt.id) || prompt.name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto existing = prompts_.find(prompt.id);
        if (existing != prompts_.end() && existing->second.builtin) {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
    }
    Json payload = prompt.to_json();
    payload.erase("builtin");
    payload.erase("scope");
    // `pinned` is deliberately retained in the persisted payload so a
    // save() round-trip preserves the flag.  set_pinned() reuses this
    // path — writing the prompt back to disk with the toggled state.
    const int32_t status = scopes.save_registry_item(
        "prompts", scope, plugin_id, prompt.id, payload);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    PromptDefinition stored = prompt;
    stored.builtin = false;
    stored.scope = std::string(scope) +
                   (plugin_id.empty() ? std::string{}
                                      : ":" + std::string(plugin_id));
    prompts_[prompt.id] = std::move(stored);
    return SAO_AI_EDITOR_OK;
}

int32_t PromptRegistry::set_pinned(std::string_view id, bool pinned,
                                   const ScopeStore& scopes, Json& result) {
    // Look up the current entry under the registry lock so we can capture
    // its scope + payload before releasing the lock for the disk write.
    PromptDefinition current;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = prompts_.find(std::string(id));
        if (found == prompts_.end()) {
            return SAO_AI_EDITOR_ERR_NOT_FOUND;
        }
        // Builtin + market prompts are compile-time / install-time defaults
        // and must stay in their canonical order.  Users who really want
        // to promote them can save a workspace override with the same id
        // — that lands as builtin=false and becomes pin-eligible.
        if (found->second.builtin || found->second.scope == "builtin" ||
            found->second.scope == "market") {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
        current = found->second;
    }
    // Decode the stored scope key back into (scope, plugin_id).  The value
    // matches what save() writes: "workspace" / "system" / "plugin:<id>".
    std::string scope_key = current.scope;
    std::string scope;
    std::string plugin_id;
    if (scope_key == "workspace" || scope_key == "system") {
        scope = scope_key;
    } else if (scope_key.rfind("plugin:", 0) == 0) {
        scope = "plugin";
        plugin_id = scope_key.substr(7);
        if (!valid_simple_id(plugin_id)) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    } else {
        // Unknown scope key — should never happen for a non-builtin entry,
        // but propagate as invalid rather than silently mis-writing.
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    current.pinned = pinned;
    const int32_t status = save(current, scopes, scope, plugin_id);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    result = Json{{"id", std::string(id)}, {"pinned", pinned}};
    return SAO_AI_EDITOR_OK;
}

int32_t PromptRegistry::remove(std::string_view id, const ScopeStore& scopes,
                               std::string_view scope,
                               std::string_view plugin_id) {
    (void)scopes;
    (void)scope;
    (void)plugin_id;
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = prompts_.find(std::string(id));
    if (found == prompts_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.builtin) {
        return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
    }
    prompts_.erase(found);
    return SAO_AI_EDITOR_OK;
}

}  // namespace sao::ai_editor::native
