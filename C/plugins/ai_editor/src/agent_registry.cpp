#include "agent_registry.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <unordered_set>
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

// --- Rule-based recommendation helpers --------------------------------------
// The scorer is intentionally cheap and dependency-free: no jieba, no LLM.
// It tokenises the query into UTF-8 tokens (ASCII words lowercased, CJK code
// points kept as single-glyph tokens) and matches them against each agent's
// searchable fields with per-field weights.

// True if the byte is the start of an ASCII letter/digit — anything else is a
// token boundary in the ASCII stream.
bool is_ascii_word_byte(unsigned char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9');
}

// Decode one UTF-8 code point starting at `text[cursor]`.  On success advances
// `cursor` past the code point and returns the code point.  On malformed input
// falls back to reading a single byte and returning U+FFFD (0xFFFD).
uint32_t decode_utf8_codepoint(std::string_view text, size_t& cursor) noexcept {
    const size_t remaining = text.size() - cursor;
    if (remaining == 0) {
        return 0;
    }
    const unsigned char byte = static_cast<unsigned char>(text[cursor]);
    if (byte < 0x80) {
        cursor += 1;
        return byte;
    }
    auto continuation = [&](size_t offset, unsigned char& out) {
        if (offset >= remaining) {
            return false;
        }
        const unsigned char raw =
            static_cast<unsigned char>(text[cursor + offset]);
        if ((raw & 0xC0) != 0x80) {
            return false;
        }
        out = raw & 0x3F;
        return true;
    };
    unsigned char c1 = 0;
    unsigned char c2 = 0;
    unsigned char c3 = 0;
    if ((byte & 0xE0) == 0xC0 && remaining >= 2 && continuation(1, c1)) {
        cursor += 2;
        return (static_cast<uint32_t>(byte & 0x1F) << 6) | c1;
    }
    if ((byte & 0xF0) == 0xE0 && remaining >= 3 && continuation(1, c1) &&
        continuation(2, c2)) {
        cursor += 3;
        return (static_cast<uint32_t>(byte & 0x0F) << 12) |
               (static_cast<uint32_t>(c1) << 6) | c2;
    }
    if ((byte & 0xF8) == 0xF0 && remaining >= 4 && continuation(1, c1) &&
        continuation(2, c2) && continuation(3, c3)) {
        cursor += 4;
        return (static_cast<uint32_t>(byte & 0x07) << 18) |
               (static_cast<uint32_t>(c1) << 12) |
               (static_cast<uint32_t>(c2) << 6) | c3;
    }
    // Invalid — skip one byte to keep making progress.
    cursor += 1;
    return 0xFFFD;
}

// True for common CJK ranges (Han, Hiragana, Katakana, Hangul).  We treat each
// such code point as its own token — simple substring-style matching still
// works because tokens are stored as UTF-8 strings.
bool is_cjk_codepoint(uint32_t codepoint) noexcept {
    return (codepoint >= 0x3040 && codepoint <= 0x30FF) ||  // Hiragana+Katakana
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||  // CJK Ext A
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||  // CJK Unified
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||  // Hangul syllables
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||  // CJK Compat
           (codepoint >= 0x20000 && codepoint <= 0x2FFFF);  // CJK Ext B-F
}

// Encode one code point back into UTF-8, appending to `out`.
void append_codepoint_utf8(uint32_t codepoint, std::string& out) {
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

// Tokenize a UTF-8 string:
//   * runs of ASCII word bytes become one lowercased token
//   * each CJK code point becomes its own single-glyph token
//   * everything else (punctuation, whitespace, symbols) is a token boundary
// Returns tokens in appearance order (with duplicates preserved so phrase
// matching can walk bigrams).
std::vector<std::string> tokenize_utf8(std::string_view text) {
    std::vector<std::string> tokens;
    tokens.reserve(text.size() / 4 + 1);
    size_t cursor = 0;
    std::string ascii_buffer;
    auto flush_ascii = [&] {
        if (!ascii_buffer.empty()) {
            tokens.push_back(std::move(ascii_buffer));
            ascii_buffer.clear();
        }
    };
    while (cursor < text.size()) {
        const size_t before = cursor;
        const uint32_t codepoint = decode_utf8_codepoint(text, cursor);
        if (codepoint == 0) {
            break;
        }
        if (codepoint < 0x80) {
            const unsigned char byte = static_cast<unsigned char>(codepoint);
            if (is_ascii_word_byte(byte)) {
                if (byte >= 'A' && byte <= 'Z') {
                    ascii_buffer.push_back(
                        static_cast<char>(byte - 'A' + 'a'));
                } else {
                    ascii_buffer.push_back(static_cast<char>(byte));
                }
            } else {
                flush_ascii();
            }
        } else if (is_cjk_codepoint(codepoint)) {
            flush_ascii();
            std::string glyph;
            append_codepoint_utf8(codepoint, glyph);
            tokens.push_back(std::move(glyph));
        } else {
            // Other non-word code points (Latin-1 punctuation, dingbats, etc.)
            // are treated as separators.  Advance past them without emitting.
            (void)before;
            flush_ascii();
        }
    }
    flush_ascii();
    return tokens;
}

// Case-insensitive ASCII lowercase (in place) — used when matching query
// tokens against agent text.  CJK code points are already normalised because
// we tokenise the agent text with the same routine.
std::string ascii_lowercase(std::string_view value) {
    std::string result(value);
    for (auto& ch : result) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return result;
}

// Wall-clock milliseconds since the Unix epoch — same helper the
// workflow_engine / conversation_store copies use.  Kept file-local so we
// don't drag <chrono> into the public header.
int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Detect which bucket an agent falls into for the include* filter flags.
// Built-ins are the hard-coded five defined in this file.  Market presets
// come from assets/ai_editor/agents/*.json (scope="market").  Anything else
// is "user" (workspace / system / plugin scopes saved via agents.save_def).
uint32_t agent_scope_flag(const AgentDefinition& agent) noexcept {
    if (agent.builtin) {
        return kRecommendIncludeBuiltin;
    }
    if (agent.scope == "market") {
        return kRecommendIncludeMarket;
    }
    return kRecommendIncludeUser;
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

bool AgentRegistry::is_builtin(std::string_view id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = agents_.find(std::string(id));
    return found != agents_.end() && found->second.builtin;
}

int32_t AgentRegistry::export_all(std::string_view scope,
                                  Json& result) const {
    if (scope != "workspace" && scope != "system" && scope != "all" &&
        scope != "builtin" && scope != "market") {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    Json agents = Json::array();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& [id, agent] : agents_) {
            (void)id;
            if (scope == "builtin") {
                if (!agent.builtin) {
                    continue;
                }
            } else if (scope == "market") {
                // Market presets are stored with builtin=false + scope=="market"
                // (see reload()).  Exclude everything else.
                if (agent.builtin || agent.scope != "market") {
                    continue;
                }
            } else if (scope == "workspace" || scope == "system") {
                if (agent.builtin) {
                    continue;
                }
                // Match the raw scope key stored on the definition; for
                // plugin-scoped items this is "plugin:<id>" so a plain
                // "workspace"/"system" export skips them, which mirrors
                // workflow.export scope semantics.
                if (agent.scope != scope) {
                    continue;
                }
            }
            // scope == "all" → include everything (builtin + market + user +
            // plugin:*).
            agents.push_back(agent.to_json());
        }
    }
    const size_t count = agents.size();
    result = Json{{"format", "sao-agents/1"},
                  {"agents", std::move(agents)},
                  {"count", count},
                  {"exportedAt", unix_milliseconds()}};
    return SAO_AI_EDITOR_OK;
}

int32_t AgentRegistry::import_agent(const Json& agent, std::string_view scope,
                                    std::string_view plugin_id, bool overwrite,
                                    const ScopeStore& scopes,
                                    std::string& out_id) {
    if (!agent.is_object()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    AgentDefinition definition = AgentDefinition::from_json(agent);
    if (!valid_simple_id(definition.id) || definition.name.empty()) {
        return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
    }
    // Built-in ids are reserved — importing over them would either replace
    // the compile-time definition on next reload or silently promote a user
    // copy to look like a built-in.  Refuse both cases regardless of
    // `overwrite`.
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto existing = agents_.find(definition.id);
        if (existing != agents_.end() && existing->second.builtin) {
            return SAO_AI_EDITOR_ERR_PERMISSION_DENIED;
        }
        if (existing != agents_.end() && !overwrite) {
            return SAO_AI_EDITOR_ERR_INVALID_ARGUMENT;
        }
    }
    // Drop transient flags so the on-disk payload matches what
    // AgentRegistry::save produces.
    definition.builtin = false;
    const int32_t status = save(definition, scopes, scope, plugin_id);
    if (status != SAO_AI_EDITOR_OK) {
        return status;
    }
    out_id = definition.id;
    return SAO_AI_EDITOR_OK;
}

int32_t AgentRegistry::recommend(std::string_view query,
                                  int top_k,
                                  uint32_t scope_flags,
                                  const std::vector<std::string>& boost_tags,
                                  Json& result) const {
    // Effective knobs — sanitise caller input up front so downstream logic
    // can assume sensible defaults.  A zero/negative topK falls back to 3;
    // an empty scope mask means "everything on".
    const int effective_top_k = (top_k <= 0) ? 3 : top_k;
    const uint32_t effective_flags =
        (scope_flags == 0U) ? kRecommendIncludeAll : scope_flags;

    // Tokenise the query once — we consult the same token list for every
    // agent so this hot loop stays cheap even with dozens of agents.
    const std::vector<std::string> query_tokens =
        tokenize_utf8(query);
    // Unique tokens drive term-overlap counting; the ordered list drives
    // phrase (bigram+) matching.
    std::unordered_set<std::string> query_term_set(query_tokens.begin(),
                                                    query_tokens.end());

    // Lowercase boostTags once — CJK tokens are already normal, ASCII gets
    // lowercased so the "boost tag appears in agent text" comparison is
    // case-insensitive on both sides.
    std::vector<std::string> normalised_boost_tags;
    normalised_boost_tags.reserve(boost_tags.size());
    for (const auto& tag : boost_tags) {
        if (tag.empty()) {
            continue;
        }
        normalised_boost_tags.push_back(ascii_lowercase(tag));
    }

    // Snapshot the current agent map under the mutex, then release before we
    // do the expensive scoring loop so read-only callers don't stall the
    // save/delete critical section.
    std::vector<AgentDefinition> agents;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        agents.reserve(agents_.size());
        for (const auto& [id, agent] : agents_) {
            (void)id;
            agents.push_back(agent);
        }
    }

    struct Candidate {
        std::string agent_id;
        std::string agent_name;
        double raw_score = 0.0;
        std::string reason;
        std::vector<std::string> matches;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(agents.size());

    for (const auto& agent : agents) {
        // Scope gate first so we don't waste tokenisation work on agents the
        // caller explicitly opted out of.
        const uint32_t agent_flag = agent_scope_flag(agent);
        if ((agent_flag & effective_flags) == 0U) {
            continue;
        }

        // Per-field token sets.  Storing each field separately (rather than
        // concatenating) is what lets us apply different weights.
        auto tokens_for = [](std::string_view text) {
            const std::vector<std::string> ordered = tokenize_utf8(text);
            std::unordered_set<std::string> unique(ordered.begin(),
                                                    ordered.end());
            return std::make_pair(std::move(ordered), std::move(unique));
        };
        const auto [name_ordered, name_set] = tokens_for(agent.name);
        const auto [wtu_ordered, wtu_set] =
            tokens_for(agent.when_to_use);
        const auto [desc_ordered, desc_set] =
            tokens_for(agent.description);
        // Tools are already discrete strings; each becomes its own token
        // (lowercased for ASCII, CJK untouched).  Nested tokenisation
        // preserves multi-word tool ids like "readFile" → "readfile".
        std::unordered_set<std::string> tools_set;
        std::vector<std::string> tools_ordered;
        for (const auto& tool : agent.tools) {
            std::vector<std::string> tool_tokens = tokenize_utf8(tool);
            for (auto& token : tool_tokens) {
                tools_set.insert(token);
                tools_ordered.push_back(std::move(token));
            }
        }

        // Combined ordered list for phrase matching — we walk the query in
        // 2-token windows and look for the same bigram anywhere in the
        // agent's text.
        std::vector<std::string> combined_ordered;
        combined_ordered.reserve(name_ordered.size() + wtu_ordered.size() +
                                  desc_ordered.size() + tools_ordered.size());
        combined_ordered.insert(combined_ordered.end(),
                                 name_ordered.begin(), name_ordered.end());
        combined_ordered.insert(combined_ordered.end(),
                                 wtu_ordered.begin(), wtu_ordered.end());
        combined_ordered.insert(combined_ordered.end(),
                                 desc_ordered.begin(), desc_ordered.end());
        combined_ordered.insert(combined_ordered.end(),
                                 tools_ordered.begin(),
                                 tools_ordered.end());
        std::unordered_set<std::string> combined_set;
        combined_set.insert(name_set.begin(), name_set.end());
        combined_set.insert(wtu_set.begin(), wtu_set.end());
        combined_set.insert(desc_set.begin(), desc_set.end());
        combined_set.insert(tools_set.begin(), tools_set.end());

        Candidate candidate;
        candidate.agent_id = agent.id;
        candidate.agent_name = agent.name;

        // Term-overlap scoring.  A term can score in multiple fields (rare
        // but legitimate — e.g. "SQL" appears in both name and description
        // for sql-expert); we sum those to reflect the stronger signal.
        std::set<std::string> matched_ordered;
        for (const auto& term : query_term_set) {
            if (term.empty()) {
                continue;
            }
            double term_score = 0.0;
            bool hit = false;
            if (name_set.count(term)) {
                term_score += 3.0;
                hit = true;
            }
            if (wtu_set.count(term)) {
                term_score += 2.0;
                hit = true;
            }
            if (desc_set.count(term)) {
                term_score += 1.5;
                hit = true;
            }
            if (tools_set.count(term)) {
                term_score += 1.0;
                hit = true;
            }
            if (hit) {
                candidate.raw_score += term_score;
                matched_ordered.insert(term);
            }
        }

        // Phrase bonus: every contiguous query bigram that also appears
        // adjacent in the agent's ordered text is worth +2.  We only credit
        // the first occurrence per bigram to avoid inflating scores when a
        // phrase repeats.
        std::set<std::string> phrase_bonus_seen;
        for (size_t i = 0; i + 1 < query_tokens.size(); ++i) {
            const std::string& a = query_tokens[i];
            const std::string& b = query_tokens[i + 1];
            if (a.empty() || b.empty()) {
                continue;
            }
            const std::string phrase = a + "|" + b;
            if (phrase_bonus_seen.count(phrase)) {
                continue;
            }
            for (size_t j = 0; j + 1 < combined_ordered.size(); ++j) {
                if (combined_ordered[j] == a &&
                    combined_ordered[j + 1] == b) {
                    candidate.raw_score += 2.0;
                    phrase_bonus_seen.insert(phrase);
                    break;
                }
            }
        }

        // Boost tags: caller-supplied keywords that should nudge specific
        // agents up.  We check the agent's combined text so users can e.g.
        // pass ["performance"] to prefer the optimizer without needing a
        // dedicated tags field.
        std::vector<std::string> boost_hits;
        for (const auto& tag : normalised_boost_tags) {
            if (combined_set.count(tag)) {
                candidate.raw_score += 1.0;
                boost_hits.push_back(tag);
            }
        }

        if (candidate.raw_score <= 0.0) {
            continue;
        }

        // Human-readable reason string — enumerate up to 3 matched terms and
        // note the boost tag / phrase bonuses so the caller can display it
        // in a tooltip.
        std::vector<std::string> reason_parts;
        if (!matched_ordered.empty()) {
            std::string joined = "matched: ";
            size_t i = 0;
            for (const auto& term : matched_ordered) {
                if (i > 0) {
                    joined += ", ";
                }
                joined += "'" + term + "'";
                if (++i >= 3) {
                    break;
                }
            }
            if (matched_ordered.size() > 3) {
                joined += ", +" + std::to_string(matched_ordered.size() - 3);
            }
            reason_parts.push_back(std::move(joined));
        }
        if (!phrase_bonus_seen.empty()) {
            reason_parts.push_back(
                std::to_string(phrase_bonus_seen.size()) +
                " phrase overlap");
        }
        if (!boost_hits.empty()) {
            std::string boosted = "boost tags: ";
            for (size_t i = 0; i < boost_hits.size(); ++i) {
                if (i > 0) {
                    boosted += ", ";
                }
                boosted += boost_hits[i];
            }
            reason_parts.push_back(std::move(boosted));
        }
        if (reason_parts.empty()) {
            candidate.reason = "matched search text";
        } else {
            for (size_t i = 0; i < reason_parts.size(); ++i) {
                if (i > 0) {
                    candidate.reason += "; ";
                }
                candidate.reason += reason_parts[i];
            }
        }
        candidate.matches.assign(matched_ordered.begin(),
                                  matched_ordered.end());
        candidates.push_back(std::move(candidate));
    }

    // Normalise scores against the batch max so downstream callers see a
    // stable 0..1 range regardless of how weight-heavy the query happened to
    // be.  Avoid division-by-zero when nothing scored.
    double max_score = 0.0;
    for (const auto& candidate : candidates) {
        max_score = std::max(max_score, candidate.raw_score);
    }

    // Sort by (score desc, id asc) — id acts as the tie-break for
    // deterministic ordering across runs.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (lhs.raw_score != rhs.raw_score) {
                      return lhs.raw_score > rhs.raw_score;
                  }
                  return lhs.agent_id < rhs.agent_id;
              });

    Json recommendations = Json::array();
    const size_t take = std::min<size_t>(candidates.size(),
                                          static_cast<size_t>(
                                              effective_top_k));
    for (size_t i = 0; i < take; ++i) {
        const auto& candidate = candidates[i];
        const double score = (max_score > 0.0)
                                 ? candidate.raw_score / max_score
                                 : 0.0;
        Json matches = Json::array();
        for (const auto& term : candidate.matches) {
            matches.push_back(term);
        }
        recommendations.push_back(Json{{"agentId", candidate.agent_id},
                                        {"agentName", candidate.agent_name},
                                        {"score", score},
                                        {"reason", candidate.reason},
                                        {"matches", std::move(matches)}});
    }

    result = Json{{"query", std::string(query)},
                  {"recommendations", std::move(recommendations)},
                  {"total", static_cast<int64_t>(take)}};
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
