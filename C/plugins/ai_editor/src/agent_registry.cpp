#include "agent_registry.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "scope_store.h"

namespace sao::ai_editor::native {
namespace {

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

// Locate a market-preset subdirectory of the installed/deployed ai_editor
// assets tree.  Mirrors default_shim_path_utf8() in extension_host.cpp:
// anchor at the DLL directory and try both install-tree and source-tree
// relative candidates.  Returns an empty path when nothing is found so
// callers treat market presets as optional.
std::filesystem::path resolve_market_assets_dir(std::wstring_view subdir) {
    const std::filesystem::path anchor = market_module_directory();
    if (anchor.empty()) {
        return {};
    }
    const std::wstring rel_install =
        std::wstring(L"assets/ai_editor/") + std::wstring(subdir);
    // Candidates ordered from most specific (install layout) to source
    // layout used during dev-tree tests.
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

// Read every *.json file in `dir` and hand it to `sink`. Malformed files
// are skipped silently — market presets are optional and must never break
// registry reload.
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

const std::vector<AgentDefinition>& builtin_agents() {
    static const std::vector<AgentDefinition> agents = [] {
        std::vector<AgentDefinition> result;
        {
            AgentDefinition agent;
            agent.id = "code-reviewer";
            agent.name = "Code Reviewer";
            agent.description =
                "Reviews code for bugs, security issues, and style";
            agent.system_prompt =
                "You are a meticulous code reviewer. Analyze the given code for:\n"
                "1. Bugs and logic errors\n"
                "2. Security vulnerabilities (injection, XSS, SSRF, etc.)\n"
                "3. Performance issues\n"
                "4. Style and readability concerns\n\n"
                "Provide specific, actionable feedback with line references. "
                "Prioritize: Critical > Warning > Info.";
            agent.tools = {"readFile", "searchFiles", "listFiles"};
            agent.icon = "\xF0\x9F\x94\x8D";  // 🔍
            agent.when_to_use =
                "When the user asks to review, audit, or check code quality";
            agent.builtin = true;
            agent.scope = "builtin";
            result.push_back(std::move(agent));
        }
        {
            AgentDefinition agent;
            agent.id = "explainer";
            agent.name = "Code Explainer";
            agent.description =
                "Explains code structure, logic, and patterns in detail";
            agent.system_prompt =
                "You are an expert code explainer. When given code:\n"
                "1. Explain what it does at a high level\n"
                "2. Walk through the logic step by step\n"
                "3. Identify design patterns and architectural decisions\n"
                "4. Note non-obvious behavior or edge cases\n\n"
                "Use clear language. Include mermaid diagrams when helpful.";
            agent.tools = {"readFile", "searchFiles", "listFiles"};
            agent.icon = "\xF0\x9F\x93\x96";  // 📖
            agent.when_to_use =
                "When the user asks to explain, understand, or document code";
            agent.builtin = true;
            agent.scope = "builtin";
            result.push_back(std::move(agent));
        }
        {
            AgentDefinition agent;
            agent.id = "debugger";
            agent.name = "Debugger";
            agent.description = "Systematically diagnoses and fixes bugs";
            agent.system_prompt =
                "You are a systematic debugger. Given a bug or error:\n"
                "1. Reproduce - understand the failing behavior\n"
                "2. Isolate - narrow down the root cause\n"
                "3. Diagnose - identify the exact issue\n"
                "4. Fix - provide the minimal correct fix\n"
                "5. Verify - explain how to confirm the fix\n\n"
                "Always read the relevant code before suggesting fixes.";
            agent.icon = "\xF0\x9F\x90\x9B";  // 🐛
            agent.when_to_use =
                "When the user reports a bug, error, or unexpected behavior";
            agent.builtin = true;
            agent.scope = "builtin";
            result.push_back(std::move(agent));
        }
        {
            AgentDefinition agent;
            agent.id = "optimizer";
            agent.name = "Performance Optimizer";
            agent.description =
                "Identifies and fixes performance bottlenecks";
            agent.system_prompt =
                "You are a performance optimization expert. Analyze code for:\n"
                "1. Algorithmic inefficiency\n"
                "2. Memory waste (unnecessary copies, leaks)\n"
                "3. I/O bottlenecks (blocking calls, missing caching)\n"
                "4. Concurrency issues (GIL, lock contention)\n\n"
                "Quantify impact when possible. Suggest Cython for hot paths.";
            agent.icon = "\xE2\x9A\xA1";  // ⚡
            agent.when_to_use =
                "When the user asks about performance, speed, or optimization";
            agent.builtin = true;
            agent.scope = "builtin";
            result.push_back(std::move(agent));
        }
        {
            AgentDefinition agent;
            agent.id = "documenter";
            agent.name = "Documentation Writer";
            agent.description =
                "Generates clear, comprehensive documentation";
            agent.system_prompt =
                "You are a technical documentation expert. Generate:\n"
                "1. Module/function docstrings (Google style)\n"
                "2. API documentation with examples\n"
                "3. Architecture overviews with diagrams\n"
                "4. User guides and tutorials\n\n"
                "Keep documentation accurate and concise.";
            agent.tools = {"readFile", "searchFiles", "listFiles", "editFile"};
            agent.icon = "\xF0\x9F\x93\x9D";  // 📝
            agent.when_to_use =
                "When the user asks to document or write docs";
            agent.builtin = true;
            agent.scope = "builtin";
            result.push_back(std::move(agent));
        }
        return result;
    }();
    return agents;
}

}  // namespace

AgentDefinition AgentDefinition::from_json(const Json& value) {
    AgentDefinition agent;
    if (!value.is_object()) {
        return agent;
    }
    agent.id = value.value("id", std::string{});
    agent.name = value.value("name", std::string{});
    agent.description = value.value("description", std::string{});
    agent.system_prompt = value.value("system_prompt", std::string{});
    agent.model = value.value("model", std::string{});
    agent.icon = value.value("icon", std::string{});
    agent.when_to_use = value.value("when_to_use", std::string{});
    agent.builtin = value.value("builtin", false);
    agent.scope = value.value("scope", std::string{"workspace"});
    if (value.contains("tools") && value["tools"].is_array()) {
        for (const auto& item : value["tools"]) {
            if (item.is_string()) {
                agent.tools.push_back(item.get<std::string>());
            }
        }
    }
    return agent;
}

Json AgentDefinition::to_json() const {
    Json tools_json = Json::array();
    for (const auto& tool : tools) {
        tools_json.push_back(tool);
    }
    return Json{{"id", id},
                {"name", name},
                {"description", description},
                {"system_prompt", system_prompt},
                {"tools", std::move(tools_json)},
                {"model", model},
                {"icon", icon},
                {"when_to_use", when_to_use},
                {"builtin", builtin},
                {"scope", scope}};
}

void AgentRegistry::reload(const ScopeStore& scopes) {
    std::lock_guard<std::mutex> guard(mutex_);
    agents_.clear();
    for (const auto& agent : builtin_agents()) {
        agents_.emplace(agent.id, agent);
    }
    // Market presets: install-time bundled agent definitions.  Loaded
    // after built-ins so built-in ids win, and before user scopes so user
    // edits with the same id override the market copy.  Marked
    // builtin=false / scope="market" so users can override or delete them.
    enumerate_market_json(
        resolve_market_assets_dir(L"agents"), [this](const Json& item) {
            if (!item.contains("id") || !item["id"].is_string()) {
                return;
            }
            AgentDefinition agent = AgentDefinition::from_json(item);
            if (agent.id.empty()) {
                return;
            }
            const auto existing = agents_.find(agent.id);
            if (existing != agents_.end() && existing->second.builtin) {
                return;  // built-in wins over market
            }
            agent.builtin = false;
            agent.scope = "market";
            agents_[agent.id] = std::move(agent);
        });
    Json registry;
    if (scopes.load_registry("agents", Json::array(), registry) !=
            SAO_AI_EDITOR_OK ||
        !registry.is_array()) {
        return;
    }
    for (const auto& item : registry) {
        if (!item.is_object() || !item.contains("id") ||
            !item["id"].is_string()) {
            continue;
        }
        AgentDefinition agent = AgentDefinition::from_json(item);
        agent.builtin = false;
        agent.scope = item.value("scope", std::string{"workspace"});
        agents_[agent.id] = std::move(agent);
    }
}

std::vector<AgentDefinition> AgentRegistry::list() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<AgentDefinition> result;
    result.reserve(agents_.size());
    for (const auto& [id, agent] : agents_) {
        (void)id;
        result.push_back(agent);
    }
    std::sort(result.begin(), result.end(),
              [](const AgentDefinition& lhs, const AgentDefinition& rhs) {
                  if (lhs.builtin != rhs.builtin) {
                      return lhs.builtin;
                  }
                  return lhs.id < rhs.id;
              });
    return result;
}

bool AgentRegistry::get(std::string_view id, AgentDefinition& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = agents_.find(std::string(id));
    if (found == agents_.end()) {
        return false;
    }
    out = found->second;
    return true;
}

int32_t AgentRegistry::save(const AgentDefinition& agent,
                            const ScopeStore& scopes,
                            std::string_view scope,
                            std::string_view plugin_id) {
    if (!valid_simple_id(agent.id) || agent.name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto existing = agents_.find(agent.id);
        if (existing != agents_.end() && existing->second.builtin) {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
    }
    Json payload = agent.to_json();
    payload.erase("builtin");
    payload.erase("scope");
    const int32_t status = scopes.save_registry_item("agents", scope, plugin_id,
                                                      agent.id, payload);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    AgentDefinition stored = agent;
    stored.builtin = false;
    stored.scope = std::string(scope) +
                   (plugin_id.empty() ? std::string{}
                                      : ":" + std::string(plugin_id));
    agents_[agent.id] = std::move(stored);
    return SAO_AI_EDITOR_OK;
}

int32_t AgentRegistry::remove(std::string_view id, const ScopeStore& scopes,
                              std::string_view scope,
                              std::string_view plugin_id) {
    (void)scopes;
    (void)scope;
    (void)plugin_id;
    if (!valid_simple_id(id)) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = agents_.find(std::string(id));
    if (found == agents_.end()) {
        return SAO_AI_EDITOR_ERR_NOT_FOUND;
    }
    if (found->second.builtin) {
        return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
    }
    agents_.erase(found);
    return SAO_AI_EDITOR_OK;
}

Json AgentRegistry::build_chat_messages(const AgentDefinition& agent,
                                        std::string_view message,
                                        const Json& history) const {
    Json messages = Json::array();
    if (!agent.system_prompt.empty()) {
        messages.push_back(
            Json{{"role", "system"}, {"content", agent.system_prompt}});
    }
    if (history.is_array()) {
        for (const auto& entry : history) {
            if (entry.is_object() && entry.contains("role") &&
                entry.contains("content")) {
                messages.push_back(entry);
            }
        }
    }
    messages.push_back(
        Json{{"role", "user"}, {"content", std::string(message)}});
    return messages;
}

}  // namespace sao::ai_editor::native
