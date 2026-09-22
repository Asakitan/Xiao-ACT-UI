// as_call.cpp — persistent module and separated AngelScript hook calls.

#include "sao/plugins/angel_host/as_call.h"

#include "as_generic_bindings_internal.h"
#include "as_plugin_internal.h"

#include "sao/plugins/angel_host/as_error.h"
#include "sao/plugins/angel_host/as_module_bridge.h"
#include "sao/plugins/angel_host/as_stdlib.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/sdk_binding/binding_angel.h"

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

namespace {

std::mutex g_plugin_registry_mutex;
std::mutex g_plugin_error_mutex;
std::unordered_map<as_plugin_handle_t, shared_plugin_state> g_plugin_registry;
std::atomic_uint64_t g_next_module_generation{1};

std::string next_module_name(const std::string& plugin_id) {
    uint64_t generation = g_next_module_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0)
        generation = g_next_module_generation.fetch_add(1, std::memory_order_relaxed);
    return "sao_plugin_" + plugin_id + "_g" + std::to_string(generation);
}

std::string format_retained_error(const retained_script_error& error) {
    std::ostringstream stream;
    stream << error.phase << ": " << error.message;
    if (!error.function.empty())
        stream << "\nfunction: " << error.function;
    if (!error.section.empty() || error.line != 0 || error.column != 0) {
        stream << "\nlocation: " << (error.section.empty() ? "<unknown>" : error.section) << ':'
               << error.line << ':' << error.column;
    }
    return stream.str();
}

size_t skip_space(const std::string& source, size_t position) {
    while (position < source.size() &&
           std::isspace(static_cast<unsigned char>(source[position])))
        ++position;
    return position;
}

// `array@` shorthand — infer the element subtype from the initializer (or the
// first `return {<expr>}` when `array@` fronts a function signature).
std::string infer_array_subtype(const std::string& source, size_t after_token) {
    size_t pos = skip_space(source, after_token);
    size_t ident_start = pos;
    while (pos < source.size() &&
           (std::isalnum(static_cast<unsigned char>(source[pos])) || source[pos] == '_'))
        ++pos;
    if (pos == ident_start)
        return "dictionary";
    pos = skip_space(source, pos);
    if (pos >= source.size())
        return "dictionary";
    if (source[pos] == '=') {
        pos = skip_space(source, pos + 1);
        if (pos < source.size() && source[pos] == '{')
            pos = skip_space(source, pos + 1);
        if (pos < source.size()) {
            const char first = source[pos];
            if (first == '{')
                return "dictionary";
            if (first == '\'' || first == '"')
                return "string";
            if (std::isdigit(static_cast<unsigned char>(first)) || first == '-' ||
                first == '+')
                return "int64";
        }
        return "dictionary";
    }
    if (source[pos] == '(') {
        // function return type — scan for the first `return {`-shaped
        // initializer inside the body.
        const size_t body = source.find('{', pos);
        if (body == std::string::npos)
            return "dictionary";
        static const std::regex return_pattern(R"(\breturn\s*\{\s*(.))");
        std::smatch m;
        if (std::regex_search(source.cbegin() + static_cast<std::ptrdiff_t>(body),
                              source.cend(), m, return_pattern)) {
            const char first = m[1].str().empty() ? '\0' : m[1].str().front();
            if (first == '{')
                return "dictionary";
            if (first == '\'' || first == '"')
                return "string";
            if (first != '\0' && (std::isdigit(static_cast<unsigned char>(first)) ||
                                  first == '-' || first == '+'))
                return "int64";
        }
        return "dictionary";
    }
    return "dictionary";
}

std::string rewrite_legacy_arrays(const std::string& source) {
    static const std::regex pattern(R"(\barray\s*@(?!\s*<))");
    std::string output;
    size_t pos = 0;
    for (std::sregex_iterator it(source.cbegin(), source.cend(), pattern), end; it != end;
         ++it) {
        const size_t start = static_cast<size_t>(it->position());
        const size_t token_end = start + static_cast<size_t>(it->length());
        output.append(source, pos, start - pos);
        output += "array<" + infer_array_subtype(source, token_end) + ">@";
        pos = token_end;
    }
    output.append(source, pos, std::string::npos);
    return output;
}

// `helper.member(args)` sugar on LocalModule → `helper.call("member", args)`.
// Locals bound from `ctx.load_local(...)` are dynamic in the old runtime; the
// native surface exposes `json@ call(const string &in, ?&in)` instead.
std::string rewrite_local_module_calls(const std::string& source) {
    static const std::regex capture_pattern(
        R"(\b(?:auto|LocalModule\s*@)\s+([A-Za-z_]\w*)\s*=\s*ctx\s*\.\s*load_local\s*\()");
    std::unordered_set<std::string> locals;
    for (std::sregex_iterator it(source.cbegin(), source.cend(), capture_pattern), end;
         it != end; ++it)
        locals.emplace((*it)[1].str());
    if (locals.empty())
        return source;
    static const std::unordered_set<std::string> passthrough = {"call", "get", "module_id",
                                                               "member_names"};
    std::string result = source;
    for (const std::string& name : locals) {
        const std::regex call_pattern("\\b" + name + "\\s*\\.\\s*([A-Za-z_]\\w*)\\s*\\(");
        std::string rewritten;
        size_t pos = 0;
        for (std::sregex_iterator it(result.cbegin(), result.cend(), call_pattern), end;
             it != end; ++it) {
            const std::string member = (*it)[1].str();
            const size_t start = static_cast<size_t>(it->position());
            const size_t token_end = start + static_cast<size_t>(it->length());
            rewritten.append(result, pos, start - pos);
            if (passthrough.count(member) != 0) {
                rewritten.append(result, start, token_end - start);
                pos = token_end;
                continue;
            }
            size_t args = skip_space(result, token_end);
            rewritten += name + ".call(\"" + member + "\"";
            if (args < result.size() && result[args] != ')')
                rewritten += ", ";
            pos = args;
        }
        rewritten.append(result, pos, std::string::npos);
        result = std::move(rewritten);
    }
    return result;
}

// `"key": value` inside initializer braces — the legacy runtime accepted
// JSON-style colons; the scriptdictionary addon needs `{{key, value}, ...}`
// pair lists. Inside an initializer `{` (a brace whose previous significant
// token is `=`, `(`, `,`, `[`, `{`, `return`, or another pair `:`) every
// `"key": expr` becomes `{"key", expr}`.
bool is_init_brace(const std::string& source, size_t brace) {
    size_t j = brace;
    while (j > 0) {
        --j;
        if (!std::isspace(static_cast<unsigned char>(source[j])))
            break;
    }
    if (j >= source.size())
        return false;
    const char c = source[j];
    if (c == '=' || c == '(' || c == ',' || c == '[' || c == '{' || c == ':')
        return true;
    // `return {`, `else {`-style case labels are rare in dict init; check the
    // preceding identifier word.
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        size_t k = j + 1;
        while (k > 0 && (std::isalnum(static_cast<unsigned char>(source[k - 1])) ||
                         source[k - 1] == '_'))
            --k;
        const std::string word = source.substr(k, j + 1 - k);
        return word == "return";
    }
    return false;
}

size_t dict_value_end(const std::string& source, size_t colon) {
    // Scan from after `:` until a `,` or `}` at nesting depth 0.
    size_t i = skip_space(source, colon + 1);
    int parens = 0, brackets = 0, braces = 0;
    bool in_string = false, in_line_comment = false, in_block_comment = false;
    for (; i < source.size(); ++i) {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (in_line_comment) {
            if (c == '\n')
                in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '(')
            ++parens;
        else if (c == ')')
            --parens;
        else if (c == '[')
            ++brackets;
        else if (c == ']')
            --brackets;
        else if (c == '{')
            ++braces;
        else if (c == '}') {
            if (braces == 0)
                break;
            --braces;
        } else if (c == ',' && parens == 0 && brackets == 0 && braces == 0) {
            break;
        }
        if (parens < 0 || brackets < 0)
            break;
    }
    return i;
}

std::string rewrite_dict_colons(const std::string& source) {
    struct pair_span {
        size_t open;
        size_t colon;
        size_t vend;
    };
    std::vector<pair_span> pairs;
    std::vector<bool> init_stack;
    bool in_string = false, in_heredoc = false, in_line_comment = false,
         in_block_comment = false;
    size_t string_open = std::string::npos;
    for (size_t i = 0; i < source.size();) {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (in_line_comment) {
            if (c == '\n')
                in_line_comment = false;
            ++i;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = false;
                i += 2;
            } else {
                ++i;
            }
            continue;
        }
        if (in_heredoc) {
            if (c == '"' && next == '"' && i + 2 < source.size() &&
                source[i + 2] == '"') {
                in_heredoc = false;
                i += 3;
            } else {
                ++i;
            }
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '"') {
                in_string = false;
                size_t j = i + 1;
                while (j < source.size() &&
                       (source[j] == ' ' || source[j] == '\t'))
                    ++j;
                if (j < source.size() && source[j] == ':' &&
                    (j + 1 >= source.size() || source[j + 1] != ':') &&
                    !init_stack.empty() && init_stack.back()) {
                    pairs.push_back({string_open, j,
                                     dict_value_end(source, j)});
                }
            }
            ++i;
            continue;
        }
        if (c == '/' && next == '/') {
            in_line_comment = true;
            i += 2;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = true;
            i += 2;
            continue;
        }
        if (c == '"' && next == '"' && i + 2 < source.size() &&
            source[i + 2] == '"') {
            in_heredoc = true;
            i += 3;
            continue;
        }
        if (c == '"') {
            in_string = true;
            string_open = i;
            ++i;
            continue;
        }
        if (c == '{')
            init_stack.push_back(is_init_brace(source, i));
        else if (c == '}' && !init_stack.empty())
            init_stack.pop_back();
        ++i;
    }
    if (pairs.empty())
        return source;
    // Splice: `{` before the key string, `,` for the `:`, `}` at value end.
    std::vector<std::pair<size_t, char>> inserts;
    for (const auto& pair : pairs) {
        inserts.emplace_back(pair.open, '{');
        inserts.emplace_back(pair.vend, '}');
    }
    std::sort(inserts.begin(), inserts.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::unordered_set<size_t> colons;
    for (const auto& pair : pairs)
        colons.insert(pair.colon);
    std::string output;
    output.reserve(source.size() + pairs.size() * 2);
    size_t insert_index = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        while (insert_index < inserts.size() && inserts[insert_index].first == i) {
            output += inserts[insert_index].second;
            ++insert_index;
        }
        output += colons.count(i) != 0 ? ',' : source[i];
    }
    while (insert_index < inserts.size()) {
        output += inserts[insert_index].second;
        ++insert_index;
    }
    return output;
}

// `auto name = { ... }` cannot deduce an anonymous init list; promote it to a
// `dictionary@` (first element is a `"key":` pair) or `array<dictionary>@`
// (elements are dictionary@-producing ui.* builders / handles).
std::string rewrite_auto_inits(const std::string& source) {
    static const std::regex pattern(R"(\bauto\s+[A-Za-z_]\w*\s*=\s*\{)");
    std::string output;
    size_t pos = 0;
    for (std::sregex_iterator it(source.cbegin(), source.cend(), pattern), end; it != end;
         ++it) {
        const size_t start = static_cast<size_t>(it->position());
        const size_t token_end = start + static_cast<size_t>(it->length());
        const size_t brace = source.rfind('{', token_end - 1);
        bool is_dictionary = false;
        if (brace != std::string::npos) {
            for (size_t j = brace + 1; j < source.size(); ++j) {
                const char c = source[j];
                if (std::isspace(static_cast<unsigned char>(c)) || c == '\n')
                    continue;
                if (c == '"') {
                    size_t k = j + 1;
                    while (k < source.size() && source[k] != '"' && source[k] != '\n') {
                        if (source[k] == '\\')
                            ++k;
                        ++k;
                    }
                    size_t l = k + 1;
                    while (l < source.size() &&
                           (source[l] == ' ' || source[l] == '\t'))
                        ++l;
                    is_dictionary = l < source.size() && source[l] == ':';
                }
                break;
            }
        }
        output.append(source, pos, start - pos);
        output += is_dictionary ? "dictionary@" : "array<dictionary>@" ;
        pos = start + 4; // consume just the `auto` keyword
    }
    output.append(source, pos, std::string::npos);
    return output;
}

} // namespace

bool as_script_declares_ctx_global(const std::string& source) {
    static const std::regex pattern(
        R"((?:^|\n)[ \t]*PluginContext\s*@\s*ctx\b)");
    return std::regex_search(source, pattern);
}

std::string as_rewrite_legacy_source(const std::string& source) {
    std::string rewritten = rewrite_local_module_calls(source);
    rewritten = rewrite_auto_inits(rewritten);
    rewritten = rewrite_dict_colons(rewritten);
    return rewrite_legacy_arrays(rewritten);
}

int32_t register_plugin_state(const shared_plugin_state& plugin) {
    if (!plugin)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_plugin_registry_mutex);
    return g_plugin_registry.emplace(plugin.get(), plugin).second
               ? SAO_OK
               : sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
}

shared_plugin_state acquire_plugin_state(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return {};
    std::lock_guard lock(g_plugin_registry_mutex);
    const auto found = g_plugin_registry.find(plugin);
    return found == g_plugin_registry.end() ? shared_plugin_state{} : found->second;
}

shared_plugin_state acquire_plugin_state_by_bound_context(void* bound_context) {
    if (bound_context == nullptr)
        return {};
    std::lock_guard lock(g_plugin_registry_mutex);
    for (const auto& entry : g_plugin_registry) {
        if (entry.second != nullptr && entry.second->bound_context == bound_context)
            return entry.second;
    }
    return {};
}

shared_plugin_state retire_plugin_state(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return {};
    std::lock_guard lock(g_plugin_registry_mutex);
    const auto found = g_plugin_registry.find(plugin);
    if (found == g_plugin_registry.end())
        return {};
    shared_plugin_state state = found->second;
    g_plugin_registry.erase(found);
    return state;
}

int32_t restore_plugin_state(const shared_plugin_state& plugin) {
    if (!plugin)
        return SAO_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_plugin_registry_mutex);
    return g_plugin_registry.emplace(plugin.get(), plugin).second ? SAO_OK : SAO_ERR_HANDLE_INVALID;
}

void retain_plugin_error(as_plugin_s& plugin, const char* phase, int32_t status,
                         const char* message, asIScriptContext* context) {
    retained_script_error error;
    error.status = status;
    error.phase = phase == nullptr ? "AngelScript" : phase;
    error.message = message == nullptr ? "operation failed" : message;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (context != nullptr && context->GetState() == asEXECUTION_EXCEPTION) {
        int column = 0;
        const char* section = nullptr;
        error.line = context->GetExceptionLineNumber(&column, &section);
        error.column = column;
        if (section != nullptr)
            error.section = section;
        if (const char* exception = context->GetExceptionString(); exception != nullptr)
            error.message = std::string("AngelScript exception: ") + exception;
        if (const asIScriptFunction* function = context->GetExceptionFunction();
            function != nullptr) {
            if (const char* declaration = function->GetDeclaration(true, true, true);
                declaration != nullptr) {
                error.function = declaration;
            }
        }
    }
#else
    (void)context;
#endif
    std::lock_guard lock(g_plugin_error_mutex);
    plugin.retained_errors[error.phase] = std::move(error);
}

void clear_plugin_error(as_plugin_s& plugin, const char* phase) {
    std::lock_guard lock(g_plugin_error_mutex);
    if (phase != nullptr)
        plugin.retained_errors.erase(phase);
}

std::string plugin_error_text(as_plugin_handle_t plugin, const char* phase, const char* fallback) {
    const shared_plugin_state state = acquire_plugin_state(plugin);
    if (!state)
        return fallback == nullptr ? std::string{} : std::string(fallback);
    std::lock_guard lock(g_plugin_error_mutex);
    const auto found = state->retained_errors.find(phase == nullptr ? "" : phase);
    return found == state->retained_errors.end()
               ? (fallback == nullptr ? std::string{} : std::string(fallback))
               : format_retained_error(found->second);
}

namespace {

void retain_unexpected_lifecycle_error(as_plugin_handle_t plugin, const char* phase) noexcept {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return;
        std::lock_guard lock(state->call_mutex);
        retain_plugin_error(*state, phase, SAO_ERR_OS_CALL_FAILED,
                            "AngelScript lifecycle raised a C++ exception");
    } catch (...) {
    }
}

#if defined(SAO_HAS_ANGELSCRIPT)

asIScriptFunction* find_hook(as_plugin_handle_t plugin, const char* name) {
    engine_execution_guard engine_lock;
    return plugin != nullptr && plugin->module != nullptr && name != nullptr
               ? plugin->module->GetFunctionByName(name)
               : nullptr;
}

int32_t capture_execution_error(as_plugin_s& plugin, asIScriptContext* context, const char* phase,
                                const char* fallback) {
    retain_plugin_error(plugin, phase, SAO_ERR_OS_CALL_FAILED, fallback, context);
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t read_source(const std::filesystem::path& path, std::string& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return SAO_ERR_HANDLE_INVALID;
    output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.bad() ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(__cpp_char8_t)
    std::u8string encoded;
    while (*value != '\0') {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(*value++)));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::u8path(value);
#endif
}

int32_t invoke_with_new_context(asIScriptEngine* engine, asIScriptFunction* function,
                                const char* args_json, char** output) {
    engine_execution_guard engine_lock;
    asIScriptContext* raw = engine->CreateContext();
    if (raw == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    const std::unique_ptr<asIScriptContext, void (*)(asIScriptContext*)> context(
        raw, [](asIScriptContext* value) { value->Release(); });
    return invoke_generic_function(context.get(), function, args_json, output);
}

int32_t execute_lifecycle(as_plugin_s& plugin, asIScriptFunction* function, const char* phase,
                          bool pass_context, bool* bool_result = nullptr) {
    engine_execution_guard engine_lock;
    if (function == nullptr || plugin.context == nullptr) {
        retain_plugin_error(plugin, phase, SAO_ERR_HANDLE_INVALID,
                            "AngelScript lifecycle function or context is unavailable");
        return SAO_ERR_HANDLE_INVALID;
    }
    if (pass_context) {
        int type_id = 0;
        if (function->GetParamCount() != 1 || function->GetParam(0, &type_id) < 0 ||
            (type_id & asTYPEID_OBJHANDLE) == 0) {
            retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                                "AngelScript lifecycle signature is invalid");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto* type = plugin.engine->GetTypeInfoById(type_id);
        if (type == nullptr || std::strcmp(type->GetName(), "PluginContext") != 0) {
            retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                                "AngelScript lifecycle context type is invalid");
            return SAO_ERR_INVALID_ARGUMENT;
        }
    } else if (function->GetParamCount() != 0) {
        retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                            "AngelScript lifecycle hook must not accept arguments");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int return_type = function->GetReturnTypeId();
    if ((bool_result == nullptr && return_type != asTYPEID_VOID) ||
        (bool_result != nullptr && return_type != asTYPEID_VOID && return_type != asTYPEID_BOOL)) {
        retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                            "AngelScript lifecycle return type is invalid");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (plugin.context->Prepare(function) < 0) {
        retain_plugin_error(plugin, phase, SAO_ERR_OS_CALL_FAILED,
                            "AngelScript context prepare failed");
        return SAO_ERR_OS_CALL_FAILED;
    }
    clear_plugin_error(plugin, phase);
    plugin.context->SetUserData(&plugin, kPluginContextUserDataSlot);
    if (pass_context && plugin.context->SetArgObject(0, plugin.bound_context) < 0) {
        retain_plugin_error(plugin, phase, SAO_ERR_INVALID_ARGUMENT,
                            "AngelScript lifecycle context binding failed");
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int exec_rc = plugin.context->Execute();
    if (exec_rc != asEXECUTION_FINISHED) {
        return capture_execution_error(plugin, plugin.context, phase,
                                       "AngelScript lifecycle did not finish");
    }
    if (bool_result != nullptr && function->GetReturnTypeId() == asTYPEID_BOOL) {
        *bool_result = plugin.context->GetReturnByte() != 0;
    }
    return SAO_OK;
}

int32_t teardown_plugin_locked(as_plugin_s& plugin) {
    engine_execution_guard engine_lock;
    if (plugin.binding != nullptr) {
        const int32_t status = sdk_binding::sao_plugins_binding_angel_deactivate(plugin.binding);
        if (status != SAO_OK) {
            plugin.lifecycle = plugin_runtime_state::cleanup_pending;
            retain_plugin_error(plugin, "teardown", status,
                                "AngelScript sdk_binding deactivate failed");
            return status;
        }
        plugin.binding = nullptr;
    }
    // release ctx-surface callbacks (funcdef refs, provider registrations)
    // while the module + engine are still alive.
    if (plugin.bound_context != nullptr) {
        const int32_t status = ctx_surface_teardown(plugin.bound_context);
        if (status != SAO_OK) {
            plugin.lifecycle = plugin_runtime_state::cleanup_pending;
            retain_plugin_error(plugin, "teardown", status, "AngelScript panel rundown failed");
            return status;
        }
    }
    // Clearing the bound `ctx` global is best-effort: modules loaded via the
    // direct ashost path (no module-bridge ctx) legitimately have no ctx
    // global, and a DiscardModule below clears it anyway. Failure must not
    // abort teardown — otherwise the plugin stays in g_plugin_registry in
    // cleanup_pending state and its destructor re-enters engine teardown at
    // process exit (atexit), which trips a purecall inside asCModule
    // destruction.
    if (plugin.module != nullptr && plugin.engine != nullptr) {
        const int32_t status = sao_plugins_ashost_bind_ctx(
            plugin.engine, nullptr, plugin.module_name.c_str());
        if (status != SAO_OK) {
            retain_plugin_error(plugin, "teardown", status,
                                "AngelScript module context cleanup failed");
        }
    }
    if (plugin.context != nullptr) {
        plugin.context->SetUserData(nullptr, kPluginContextUserDataSlot);
        plugin.context->Release();
        plugin.context = nullptr;
    }
    if (plugin.module != nullptr && plugin.engine != nullptr) {
        plugin.engine->DiscardModule(plugin.module_name.c_str());
        plugin.module = nullptr;
    }
    plugin.bound_context = nullptr;
    plugin.lifecycle = plugin_runtime_state::dead;
    plugin.unload_owner = {};
    plugin.unload_hook_completed = false;
    return SAO_OK;
}

int32_t rollback_failed_load(const shared_plugin_state& plugin, int32_t failure,
                             as_plugin_handle_t* out_plugin) noexcept {
    if (!plugin)
        return failure;
    int32_t cleanup_status = SAO_OK;
    try {
        std::lock_guard lock(plugin->call_mutex);
        cleanup_status = teardown_plugin_locked(*plugin);
    } catch (...) {
        cleanup_status = SAO_ERR_OS_CALL_FAILED;
        plugin->lifecycle = plugin_runtime_state::cleanup_pending;
    }
    if (cleanup_status == SAO_OK)
        return failure;
    const int32_t register_status = register_plugin_state(plugin);
    if (register_status == SAO_OK ||
        register_status == sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS) {
        if (out_plugin != nullptr)
            *out_plugin = plugin.get();
        return cleanup_status;
    }
    return register_status;
}


#endif

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_load_script(
    asIScriptEngine* engine, const wchar_t* plugin_dir, const char* entry_relative,
    const char* plugin_id_utf8, void* ctx_ptr, as_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (engine == nullptr || plugin_dir == nullptr || entry_relative == nullptr ||
        plugin_id_utf8 == nullptr || entry_relative[0] == '\0' or plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    shared_plugin_state plugin;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_host_state host_state = acquire_host_for_engine(engine);
        if (!host_state)
            return SAO_ERR_HANDLE_INVALID;
        host_instance_lease host_instance;
        const int32_t admission_status = host_instance.acquire(host_state);
        if (admission_status != SAO_OK)
            return admission_status;
        engine_execution_guard engine_lock;
        const std::filesystem::path path =
            std::filesystem::path(plugin_dir) / path_from_utf8(entry_relative);
        std::string source;
        int32_t status = read_source(path, source);
        if (status != SAO_OK)
            return status;
        status = sao_plugins_ashost_register_sdk(engine);
        if (status != SAO_OK)
            return status;
        const int32_t stdlib_status = sao_plugins_ashost_install_stdlib(engine);
        if (stdlib_status != SAO_OK and stdlib_status != SAO_ERR_NOT_IMPLEMENTED)
            return stdlib_status;

        plugin = std::make_shared<as_plugin_s>();
        plugin->host_state = host_state;
        plugin->host_instance = std::move(host_instance);
        plugin->engine = engine;
        plugin->bound_context = ctx_ptr;
        plugin->plugin_id = plugin_id_utf8;
        plugin->module_name = next_module_name(plugin->plugin_id);
        plugin->module = engine->GetModule(plugin->module_name.c_str(), asGM_ALWAYS_CREATE);
        if (plugin->module == nullptr)
            return rollback_failed_load(plugin, SAO_ERR_OS_CALL_FAILED, out_plugin);
        constexpr char bridge_section[] = "PluginContext@ ctx;";
        const std::string engine_preamble = sao_as_engine_preamble(engine);
        const std::string rewritten_source = as_rewrite_legacy_source(source);
        // Skip the injected `ctx` global when the script already declares it;
        // the older plugin convention declares `PluginContext@ ctx` itself.
        const bool inject_ctx = !as_script_declares_ctx_global(rewritten_source);
        const bool built =
            !(inject_ctx && plugin->module->AddScriptSection(
                                "sao_module_bridge", bridge_section,
                                sizeof(bridge_section) - 1) < 0) &&
            plugin->module->AddScriptSection("sao_engine_preamble", engine_preamble.c_str(),
                                             engine_preamble.size()) >= 0 &&
            plugin->module->AddScriptSection(entry_relative, rewritten_source.c_str(),
                                             rewritten_source.size()) >= 0 &&
            plugin->module->Build() >= 0;
        if (!built) {
            return rollback_failed_load(plugin, SAO_ERR_INVALID_ARGUMENT, out_plugin);
        }
        status = sao_plugins_ashost_bind_ctx(engine, ctx_ptr, plugin->module_name.c_str());
        if (status != SAO_OK)
            return rollback_failed_load(plugin, status, out_plugin);
        status = sdk_binding::sao_plugins_binding_angel_activate(
            reinterpret_cast<sdk_binding::plugin_context_ptr>(ctx_ptr), engine, &plugin->binding);
        if (status != SAO_OK)
            return rollback_failed_load(plugin, status, out_plugin);
        plugin->context = engine->CreateContext();
        if (plugin->context == nullptr)
            return rollback_failed_load(plugin, SAO_ERR_OS_CALL_FAILED, out_plugin);
        plugin->context->SetUserData(plugin.get(), kPluginContextUserDataSlot);
        status = register_plugin_state(plugin);
        if (status != SAO_OK)
            return rollback_failed_load(plugin, status, out_plugin);
        *out_plugin = plugin.get();
        return SAO_OK;
#else
        (void)ctx_ptr;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return rollback_failed_load(plugin, SAO_ERR_OS_CALL_FAILED, out_plugin);
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_load(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        asIScriptFunction* function = find_hook(state.get(), "on_load");
        if (function == nullptr)
            return SAO_OK;
        return execute_lifecycle(*state, function, "on_load", function->GetParamCount() == 1);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        retain_unexpected_lifecycle_error(plugin, "on_load");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_enable(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* function = find_hook(state.get(), "on_enable");
        return function == nullptr ? SAO_OK
                                   : execute_lifecycle(*state, function, "on_enable", false);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        retain_unexpected_lifecycle_error(plugin, "on_enable");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_disable(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* function = find_hook(state.get(), "on_disable");
        return function == nullptr ? SAO_OK
                                   : execute_lifecycle(*state, function, "on_disable", false);
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        retain_unexpected_lifecycle_error(plugin, "on_disable");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_unload(as_plugin_handle_t plugin, bool* out_allow_unload) {
    if (out_allow_unload == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_allow_unload = true;
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    shared_plugin_state state;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        const std::thread::id owner = std::this_thread::get_id();
        if (state->lifecycle == plugin_runtime_state::ready) {
            state->lifecycle = plugin_runtime_state::unloading;
            state->unload_owner = owner;
            state->unload_hook_completed = false;
        } else if (state->lifecycle == plugin_runtime_state::unloading) {
            if (state->unload_owner != owner)
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            if (state->unload_hook_completed)
                return SAO_OK;
        } else {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        auto* function = find_hook(state.get(), "on_unload");
        if (function == nullptr) {
            state->unload_hook_completed = true;
            return SAO_OK;
        }
        const int32_t status = execute_lifecycle(*state, function, "on_unload", false,
                                                  out_allow_unload);
        if (status != SAO_OK || !*out_allow_unload) {
            state->lifecycle = plugin_runtime_state::ready;
            state->unload_owner = {};
            state->unload_hook_completed = false;
        } else {
            state->unload_hook_completed = true;
        }
        return status;
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        if (state) {
            try {
                std::lock_guard lock(state->call_mutex);
                if (state->unload_owner == std::this_thread::get_id() &&
                    state->lifecycle == plugin_runtime_state::unloading) {
                    state->lifecycle = plugin_runtime_state::ready;
                    state->unload_owner = {};
                    state->unload_hook_completed = false;
                }
            } catch (...) {
            }
        }
        retain_unexpected_lifecycle_error(plugin, "on_unload");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_hook(as_plugin_handle_t plugin, const char* hook_name,
                             const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    if (plugin == nullptr || hook_name == nullptr || hook_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        engine_execution_guard engine_lock;
        if (state->lifecycle != plugin_runtime_state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        auto* function = find_hook(state.get(), hook_name);
        if (function == nullptr)
            return SAO_ERR_HANDLE_INVALID;
        clear_plugin_error(*state, hook_name);
        const int32_t status = invoke_generic_function(state->context, function, args_json_utf8,
                                                       out_result_json_utf8, state.get());
        if (status != SAO_OK && state->context->GetState() == asEXECUTION_EXCEPTION) {
            return capture_execution_error(*state, state->context, hook_name,
                                           "AngelScript hook did not finish");
        }
        if (status != SAO_OK) {
            retain_plugin_error(*state, hook_name, status,
                                "AngelScript hook argument or return conversion failed");
        }
        return status;
#else
        (void)args_json_utf8;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_has_hook(as_plugin_handle_t plugin, const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr)
        return false;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return false;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready &&
               find_hook(state.get(), hook_name) != nullptr;
#else
        return false;
#endif
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function(asIScriptEngine* engine, asIScriptFunction* fn,
                                 const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    if (engine == nullptr || fn == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        return invoke_with_new_context(engine, fn, args_json_utf8, out_result_json_utf8);
#else
        (void)args_json_utf8;
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_script(as_plugin_handle_t plugin) {
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    shared_plugin_state state;
    try {
#if defined(SAO_HAS_ANGELSCRIPT)
        state = acquire_plugin_state(plugin);
        if (!state)
            return SAO_ERR_HANDLE_INVALID;
        std::lock_guard lock(state->call_mutex);
        const std::thread::id owner = std::this_thread::get_id();
        if (state->lifecycle == plugin_runtime_state::ready ||
            state->lifecycle == plugin_runtime_state::cleanup_pending) {
            state->lifecycle = plugin_runtime_state::unloading;
            state->unload_owner = owner;
        } else if (state->lifecycle == plugin_runtime_state::unloading) {
            if (state->unload_owner != owner)
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        } else {
            return SAO_ERR_HANDLE_INVALID;
        }
        const int32_t status = teardown_plugin_locked(*state);
        if (status != SAO_OK) {
            state->lifecycle = plugin_runtime_state::cleanup_pending;
            state->unload_owner = {};
            state->unload_hook_completed = false;
            return status;
        }
        const shared_plugin_state retired = retire_plugin_state(plugin);
        if (!retired)
            return SAO_ERR_HANDLE_INVALID;
        state->host_instance.reset();
        state->host_state.reset();
        return SAO_OK;
#else
        return SAO_ERR_NOT_IMPLEMENTED;
#endif
    } catch (...) {
        if (state) {
            try {
                std::lock_guard lock(state->call_mutex);
                if (state->unload_owner == std::this_thread::get_id() &&
                    state->lifecycle == plugin_runtime_state::unloading) {
                    state->lifecycle = plugin_runtime_state::cleanup_pending;
                    state->unload_owner = {};
                    state->unload_hook_completed = false;
                }
            } catch (...) {
            }
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API asIScriptModule* SAO_PLUGINS_CALL
sao_plugins_ashost_get_module(as_plugin_handle_t plugin) {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return nullptr;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready ? state->module : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_get_engine(as_plugin_handle_t plugin) {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return nullptr;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready ? state->engine : nullptr;
    } catch (...) {
        return nullptr;
    }
}

extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_ashost_get_bound_context(as_plugin_handle_t plugin) {
    try {
        const shared_plugin_state state = acquire_plugin_state(plugin);
        if (!state)
            return nullptr;
        std::lock_guard lock(state->call_mutex);
        return state->lifecycle == plugin_runtime_state::ready ? state->bound_context : nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sao::plugins::angel_host
