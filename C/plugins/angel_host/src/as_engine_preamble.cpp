// as_engine_preamble.cpp — generates the `namespace sao_engine` script section
// injected into every plugin module at load time.
//
// sao_plugins_ashost_load_script adds it as the "sao_engine_preamble" section
// between the "sao_module_bridge" `PluginContext@ ctx` declaration and the
// plugin entry section; sao_plugins_ashost_load_plugin does the same on the
// manifest path.  Wrappers therefore take `PluginContext@ ctx` as the first
// parameter instead of relying on a module-global — it compiles in every load
// path regardless of whether the module bridge injected the global.
//
// Per catalog entry the generator emits:
//   string <flat>(PluginContext@ ctx, <typed params>)   — when every arg name
//         classifies cleanly into uint64 / double / bool / string / json
//   string <flat>_raw(PluginContext@ ctx, const string &in args_json)
//         — always; routes verbatim through ctx.engine_call(name, args_json)
//
// Name flattening follows the shared host contract: "mem.read_u64" →
// "mem_read_u64" ('.' → '_').  Identifiers are sanitized against the
// AngelScript keyword set so catalog entries can never shadow a token.
//
// Typed wrappers build args through the registered `json` type — quoting and
// escaping are handled by json.stringify(), never by string concatenation.
// Struct-valued args (spec / descriptor / offsets / ...) arrive as JSON text
// params and are embedded via json.set_json; payloads that still cannot be
// represented cleanly remain reachable through <flat>_raw.

#include "as_plugin_internal.h"

#include "sao/plugins/sdk_binding/binding_engine.h"

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

namespace sdk = sao::plugins::sdk_binding;

#if defined(SAO_HAS_ANGELSCRIPT)
namespace {

// AngelScript reserved words (2.36.x token set plus contextual keywords).
// Flattened catalog names can never legitimately collide ('group.fn' →
// 'group_fn' keeps a '_' inside), but param names come straight from
// arg_names so everything emitted passes through the sanitizer anyway.
const char* const kAsKeywords[] = {
    "abstract", "and",      "auto",      "bool",     "break",   "case",
    "cast",     "catch",    "class",     "const",    "continue","default",
    "delete",   "do",       "double",    "else",     "enum",    "explicit",
    "external", "false",    "final",     "float",    "for",     "from",
    "funcdef",  "get",      "if",        "import",   "in",      "inout",
    "int",      "int8",     "int16",     "int32",    "int64",   "interface",
    "is",       "mixin",    "namespace", "not",      "null",    "or",
    "out",      "override", "property",  "return",   "set",     "shared",
    "string",   "super",    "switch",    "this",     "true",    "try",
    "typedef",  "uint",     "uint8",     "uint16",   "uint32",  "uint64",
    "using",    "void",     "while",     "xor",
};

bool is_as_keyword(const std::string& ident) noexcept {
    for (const char* keyword : kAsKeywords) {
        if (ident == keyword)
            return true;
    }
    return false;
}

// Turns arbitrary catalog text into a legal AS identifier: non [A-Za-z0-9_]
// chars fold to '_', a leading digit or keyword gets a '_' prefix.
std::string sanitize_as_ident(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(u) != 0 || c == '_' ? c : '_');
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())) != 0 ||
        is_as_keyword(out)) {
        out.insert(0, "_");
    }
    return out;
}

std::string flatten_engine_name(const char* name) {
    std::string flat = sanitize_as_ident(name);
    for (char& c : flat) {
        if (c == '.')
            c = '_';
    }
    return flat;
}

// ── arg kind inference ─────────────────────────────────────────────────────
//
// Kind is inferred from the arg name alone, gated on "safely inferable":
// strings and json text funnel back to ctx.engine_call correctly through
// `json.set`/`json.set_json`, so a mis-guess degrades the wrapper's type
// signature, never the dispatch.  Entries whose args do not classify get only
// their `_raw` escape hatch.

enum class preamble_arg_kind { u64, f64, boolean, json_text, str };

bool name_contains(const std::string& name,
                   std::initializer_list<const char*> needles) noexcept {
    for (const char* needle : needles) {
        if (name.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

bool name_ends_with(const std::string& name, const char* suffix) noexcept {
    const size_t len = std::strlen(suffix);
    return name.size() >= len && name.compare(name.size() - len, len, suffix) == 0;
}

// *_id names that are string keys rather than numeric handles — keep the
// wrapper honest for ui/net entries whose ids are plugin-typed names.
bool is_string_id(const std::string& name) noexcept {
    return name == "id" ||
           name_contains(name, {"source_id", "widget_id", "panel_id", "surface_id",
                                "plugin_id", "entry_id", "channel_id", "category_id",
                                "action_id", "event_id", "menu_id", "item_id",
                                "key_id", "rule_id", "name_id"});
}

preamble_arg_kind classify_arg(const char* arg_name) noexcept {
    if (arg_name == nullptr || arg_name[0] == '\0')
        return preamble_arg_kind::str;
    const std::string name(arg_name);
    // base64 fields are plain strings on the wire — check before "*_data".
    if (name_ends_with(name, "_b64") || name.find("b64") != std::string::npos)
        return preamble_arg_kind::str;
    // Struct / array / blob args ride as raw JSON text via json.set_json.
    if (name_contains(name, {"spec", "offsets", "floats", "world_pos", "descriptor",
                             "metadata", "payload", "config", "args", "json",
                             "matrix", "geometry", "rect", "bones"}))
        return preamble_arg_kind::json_text;
    if (name_contains(name, {"enable", "disable", "visible", "callback", "movable",
                             "resizable", "modal", "flat", "remember", "show_",
                             "click_through", "fullscreen", "hidden", "mute",
                             "loop", "active"}))
        return preamble_arg_kind::boolean;
    if (name_contains(name, {"seconds", "duration", "volume", "opacity", "priority",
                             "latitude", "longitude", "ratio", "scale", "confidence",
                             "threshold", "rate", "progress", "alpha", "weight"}))
        return preamble_arg_kind::f64;
    if (is_string_id(name))
        return preamble_arg_kind::str;
    if (name_ends_with(name, "_id") ||
        name_ends_with(name, "_px") || name_ends_with(name, "_ms") ||
        name_ends_with(name, "_ns") || name_ends_with(name, "_hz") ||
        name_ends_with(name, "_w") || name_ends_with(name, "_h") ||
        name_ends_with(name, "_x") || name_ends_with(name, "_y") ||
        name_ends_with(name, "_z") || name_ends_with(name, "_len") ||
        name_ends_with(name, "_sz") || name_ends_with(name, "_szt"))
        return preamble_arg_kind::u64;
    if (name_contains(name, {"address", "size", "count", "token", "handle", "flags",
                             "mask", "index", "max", "offset", "length", "capacity",
                             "tick", "timestamp", "width", "height", "depth", "heap",
                             "pid", "snap", "link_type", "slot", "minor", "major",
                             "tracker", "viewport", "refresh", "priority_q"}))
        return preamble_arg_kind::u64;
    if (name == "x" || name == "y" || name == "z" || name == "w" || name == "h" ||
        name == "pid")
        return preamble_arg_kind::u64;
    return preamble_arg_kind::str;
}

const char* arg_decl_type(preamble_arg_kind kind) noexcept {
    switch (kind) {
    case preamble_arg_kind::u64:
        return "uint64";
    case preamble_arg_kind::f64:
        return "double";
    case preamble_arg_kind::boolean:
        return "bool";
    default:
        return "const string &in";
    }
}

// json.set line for one arg; json_text params embed parsed JSON, scalars go
// through the typed json.set overloads.
std::string arg_set_line(const char* key, preamble_arg_kind kind,
                         const std::string& param) {
    std::string line = "    j.";
    if (kind == preamble_arg_kind::json_text) {
        line += "set_json(\"" + std::string(key) + "\", " + param + ");";
        return line;
    }
    line += "set(\"" + std::string(key) + "\", ";
    if (kind == preamble_arg_kind::u64)
        line += "int64(" + param + ")";
    else
        line += param;
    line += ");";
    return line;
}

// Text emitted inside "..." string literals — reject anything that could
// break out of the literal (quotes, backslashes, control chars).
bool safe_as_string_literal(const char* text) noexcept {
    if (text == nullptr)
        return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0;
         ++p) {
        if (*p == '"' || *p == '\\' || *p < 0x20)
            return false;
    }
    return true;
}

// Emits `string <fn>(ctx, args_json)` — the always-correct escape hatch;
// `fn` is the already-deduped flattened name with its "_raw" suffix.
void emit_raw(std::string& out, const sdk::sdk_engine_function_desc& desc,
              const std::string& fn) {
    out += "// " + std::string(desc.name) + " — raw JSON passthrough\n";
    out += "string " + fn + "(PluginContext@ ctx, const string &in args_json) {\n";
    out += "    return ctx.engine_call(\"" + std::string(desc.name) + "\", args_json);\n";
    out += "}\n\n";
}

// Emits `string <flat>(ctx, <typed params>)` when every catalog arg
// classifies; returns false when a signature cannot be represented cleanly
// (untyped null arg names, duplicate sanitized params) → caller keeps _raw.
bool emit_typed(std::string& out, const sdk::sdk_engine_function_desc& desc,
                const std::string& flat) {
    struct emitted_arg {
        const char* key;
        preamble_arg_kind kind;
        std::string param;
    };
    std::vector<emitted_arg> args;
    std::unordered_set<std::string> used_params;
    for (uint32_t index = 0; index < desc.arg_count; ++index) {
        const char* key = desc.arg_names != nullptr ? desc.arg_names[index] : nullptr;
        if (key == nullptr || key[0] == '\0')
            return false;
        std::string param = sanitize_as_ident(key);
        if (!used_params.insert(param).second) {
            param += "_" + std::to_string(index);
            if (!used_params.insert(param).second)
                return false;
        }
        args.push_back(emitted_arg{key, classify_arg(key), std::move(param)});
    }

    std::string sig = "string " + flat + "(PluginContext@ ctx";
    for (const auto& arg : args) {
        sig += ", ";
        sig += arg_decl_type(arg.kind);
        sig += " ";
        sig += arg.param;
    }
    sig += ") {";

    out += "// " + std::string(desc.name) + "(";
    for (uint32_t index = 0; index < desc.arg_count; ++index) {
        if (index != 0)
            out += ", ";
        out += desc.arg_names[index];
    }
    out += ")\n";
    out += sig + "\n";
    if (args.empty()) {
        out += "    return ctx.engine_call(\"" + std::string(desc.name) + "\", \"{}\");\n";
        out += "}\n\n";
        return true;
    }
    out += "    json@ j = json();\n";
    for (const auto& arg : args) {
        out += arg_set_line(arg.key, arg.kind, arg.param);
        out.push_back('\n');
    }
    out += "    return ctx.engine_call(\"" + std::string(desc.name) +
           "\", j.stringify());\n";
    out += "}\n\n";
    return true;
}

} // namespace
#endif // SAO_HAS_ANGELSCRIPT

std::string sao_as_engine_preamble(asIScriptEngine* engine) {
    std::string out;
    out.reserve(64 * 1024);
    out += "// sao_engine preamble — generated by sao_as_engine_preamble();\n";
    out += "// catalog order follows sdk_engine_catalog_at; do not edit.\n";
#if defined(SAO_HAS_ANGELSCRIPT)
    // Emit the full surface whenever PluginContext exists; probing
    // GetMethodByName("engine_call") is order-dependent (methods land when
    // the ctx surface installs), which made the namespace flakily empty.
    if (engine == nullptr || engine->GetTypeInfoByName("PluginContext") == nullptr) {
        // ctx surface (and with it json/funcdefs) was never installed on this
        // engine — emit a compilable empty namespace and fail closed.
        out += "namespace sao_engine {}\n";
        return out;
    }
    const bool has_json = engine->GetTypeInfoByName("json") != nullptr;
    const bool has_channel_cb = engine->GetTypeInfoByName("engine_channel_cb") != nullptr;

    out += "namespace sao_engine {\n\n";
    out += "string call_raw(PluginContext@ ctx, const string &in name,\n";
    out += "                const string &in args_json) {\n";
    out += "    return ctx.engine_call(name, args_json);\n}\n\n";
    out += "string list(PluginContext@ ctx) {\n    return ctx.engine_list();\n}\n\n";
    if (has_channel_cb) {
        out += "bool on(PluginContext@ ctx, const string &in channel,\n";
        out += "          engine_channel_cb@ cb) {\n";
        out += "    return ctx.engine_on(channel, cb);\n}\n\n";
        out += "bool off(PluginContext@ ctx, const string &in channel) {\n";
        out += "    return ctx.engine_off(channel);\n}\n\n";
    }

    std::unordered_set<std::string> used{"call_raw", "list", "on", "off"};
    const size_t count = sdk::sdk_engine_catalog_size();
    for (size_t index = 0; index < count; ++index) {
        const sdk::sdk_engine_function_desc* desc = sdk::sdk_engine_catalog_at(index);
        if (desc == nullptr || !safe_as_string_literal(desc->name) || desc->name[0] == '\0')
            continue;
        std::string flat = flatten_engine_name(desc->name);
        while (!used.insert(flat).second)
            flat += "_";  // catalog collision after sanitize — disambiguate.
        std::string raw_name = flat + "_raw";
        while (!used.insert(raw_name).second)
            raw_name += "_";
        emit_raw(out, *desc, raw_name);
        if (has_json)
            (void)emit_typed(out, *desc, flat);
    }
    out += "}\n";
#else
    (void)engine;
    out += "// sao_engine preamble unavailable: AngelScript runtime missing\n";
#endif
    return out;
}

} // namespace sao::plugins::angel_host
