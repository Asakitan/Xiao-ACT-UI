// as_plugin_lifecycle.cpp — Wave 8 / Agent d Phase 8 实装
//
// SDK 3 条 C 函数被注册到 asIScriptEngine 全局命名空间, 用 asCALL_CDECL.
// 每条内部通过 asGetActiveContext()->GetUserData() 拿到 as_plugin_s* 更新计数器.

#include "sao/plugins/angel_host/as_plugin_lifecycle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(SAO_HAS_ANGELSCRIPT)
#include <angelscript.h>
#endif

namespace sao::plugins::angel_host {

#if defined(SAO_HAS_ANGELSCRIPT)

// ── 极简 JSON reader: 只支持读顶层 {"key": "value"} 拿 entry 字段 ────
namespace {
std::string extract_json_string_field(const std::string& src, const std::string& key) {
    // 找 "key" — 简单朴素: 找 "key" 后接可选 whitespace + ':' 后接可选 ws + '"...'"
    std::string needle = "\"" + key + "\"";
    auto pos = src.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
    if (pos >= src.size() || src[pos] != ':') return {};
    ++pos;
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
    if (pos >= src.size() || src[pos] != '"') return {};
    ++pos;
    std::string out;
    while (pos < src.size() && src[pos] != '"') {
        char c = src[pos++];
        if (c == '\\' && pos < src.size()) {
            char e = src[pos++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: out += e; break;
            }
        } else out += c;
    }
    return out;
}

std::string parent_dir(const std::string& p) {
    auto pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return p.substr(0, pos);
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

struct as_plugin_s {
    as_host_handle_t host = nullptr;
    asIScriptModule* module = nullptr;
    asIScriptContext* ctx = nullptr;
    std::string plugin_id;
    std::string entry_path;
    as_sdk_counters counters;
    bool sdk_registered = false;
    bool loaded_ok = false;
};

// ── SDK 3 条 C 函数 (asCALL_CDECL, 全局命名空间) ─────────────
//
// 用 asGetActiveContext()->GetUserData() 拿到当前跑 script 的 as_plugin_s*
// 用于记账.  这个 pattern 比给每条 SDK API 挂 asIScriptContext user data 简单.

static void as_sdk_log_info(const std::string& msg) {
    asIScriptContext* ctx = asGetActiveContext();
    if (ctx == nullptr) return;
    auto* plugin = static_cast<as_plugin_s*>(ctx->GetUserData(0x5A05DB01));
    if (plugin == nullptr) return;
    ++plugin->counters.log_info_calls;
    size_t n = msg.size();
    if (n >= sizeof(plugin->counters.last_log_utf8))
        n = sizeof(plugin->counters.last_log_utf8) - 1;
    std::memcpy(plugin->counters.last_log_utf8, msg.data(), n);
    plugin->counters.last_log_utf8[n] = '\0';
}

static void as_sdk_register_ui_panel(const std::string& panel_id) {
    asIScriptContext* ctx = asGetActiveContext();
    if (ctx == nullptr) return;
    auto* plugin = static_cast<as_plugin_s*>(ctx->GetUserData(0x5A05DB01));
    if (plugin == nullptr) return;
    ++plugin->counters.register_ui_panel_calls;
    size_t n = panel_id.size();
    if (n >= sizeof(plugin->counters.last_panel_id_utf8))
        n = sizeof(plugin->counters.last_panel_id_utf8) - 1;
    std::memcpy(plugin->counters.last_panel_id_utf8, panel_id.data(), n);
    plugin->counters.last_panel_id_utf8[n] = '\0';
}

static void as_sdk_register_hotkey(const std::string& hotkey_id,
                                   const std::string& default_key) {
    asIScriptContext* ctx = asGetActiveContext();
    if (ctx == nullptr) return;
    auto* plugin = static_cast<as_plugin_s*>(ctx->GetUserData(0x5A05DB01));
    if (plugin == nullptr) return;
    ++plugin->counters.register_hotkey_calls;
    size_t n = hotkey_id.size();
    if (n >= sizeof(plugin->counters.last_hotkey_id_utf8))
        n = sizeof(plugin->counters.last_hotkey_id_utf8) - 1;
    std::memcpy(plugin->counters.last_hotkey_id_utf8, hotkey_id.data(), n);
    plugin->counters.last_hotkey_id_utf8[n] = '\0';
    n = default_key.size();
    if (n >= sizeof(plugin->counters.last_hotkey_key_utf8))
        n = sizeof(plugin->counters.last_hotkey_key_utf8) - 1;
    std::memcpy(plugin->counters.last_hotkey_key_utf8, default_key.data(), n);
    plugin->counters.last_hotkey_key_utf8[n] = '\0';
}

// std::string addon (最小实装).  用 AS_MAX_PORTABILITY 编译 AS →
// asCALL_CDECL 等 native call 不可用, 所有绑定用 asCALL_GENERIC.
// 每条 SDK / string method 都用一个 void(*)(asIScriptGeneric*) wrapper.
namespace {
struct as_str {
    std::string data;
};

static void gen_str_construct(asIScriptGeneric* gen) {
    new (gen->GetObject()) as_str();
}
static void gen_str_copy_construct(asIScriptGeneric* gen) {
    const as_str* other = static_cast<const as_str*>(gen->GetArgAddress(0));
    new (gen->GetObject()) as_str(*other);
}
static void gen_str_destruct(asIScriptGeneric* gen) {
    static_cast<as_str*>(gen->GetObject())->~as_str();
}
static void gen_str_assign(asIScriptGeneric* gen) {
    const as_str* other = static_cast<const as_str*>(gen->GetArgAddress(0));
    as_str* self = static_cast<as_str*>(gen->GetObject());
    self->data = other->data;
    gen->SetReturnAddress(self);
}
static void gen_str_add_assign(asIScriptGeneric* gen) {
    const as_str* other = static_cast<const as_str*>(gen->GetArgAddress(0));
    as_str* self = static_cast<as_str*>(gen->GetObject());
    self->data += other->data;
    gen->SetReturnAddress(self);
}
static void gen_str_eq(asIScriptGeneric* gen) {
    const as_str* a = static_cast<const as_str*>(gen->GetObject());
    const as_str* b = static_cast<const as_str*>(gen->GetArgAddress(0));
    gen->SetReturnByte(a->data == b->data ? 1 : 0);
}
static void gen_str_add(asIScriptGeneric* gen) {
    const as_str* a = static_cast<const as_str*>(gen->GetObject());
    const as_str* b = static_cast<const as_str*>(gen->GetArgAddress(0));
    // Return by value — AS generic 要求 SetReturnObject
    as_str result;
    result.data = a->data + b->data;
    // Copy to return slot
    void* ret_slot = gen->GetAddressOfReturnLocation();
    new (ret_slot) as_str(std::move(result));
}

// v2.38 起 RegisterStringFactory 需要 IStringFactory 接口
class as_string_factory : public asIStringFactory {
public:
    const void* GetStringConstant(const char* data, asUINT length) override {
        auto* s = new as_str();
        if (data && length > 0) s->data.assign(data, length);
        return s;
    }
    int ReleaseStringConstant(const void* p) override {
        delete static_cast<const as_str*>(p);
        return 0;
    }
    int GetRawStringData(const void* p, char* data, asUINT* length) const override {
        const as_str* s = static_cast<const as_str*>(p);
        if (length) *length = static_cast<asUINT>(s->data.size());
        if (data) std::memcpy(data, s->data.data(), s->data.size());
        return 0;
    }
};
static as_string_factory g_str_factory;

// SDK 3 条 dispatcher, asCALL_GENERIC 版本
static void gen_sdk_log_info(asIScriptGeneric* gen) {
    const as_str* msg = static_cast<const as_str*>(gen->GetArgAddress(0));
    as_sdk_log_info(msg ? msg->data : std::string());
}
static void gen_sdk_register_ui_panel(asIScriptGeneric* gen) {
    const as_str* id = static_cast<const as_str*>(gen->GetArgAddress(0));
    as_sdk_register_ui_panel(id ? id->data : std::string());
}
static void gen_sdk_register_hotkey(asIScriptGeneric* gen) {
    const as_str* id = static_cast<const as_str*>(gen->GetArgAddress(0));
    const as_str* key = static_cast<const as_str*>(gen->GetArgAddress(1));
    as_sdk_register_hotkey(id ? id->data : std::string(),
                           key ? key->data : std::string());
}
} // namespace

// 注册 string 类型 + 3 SDK 函数 到 engine
static int32_t register_sdk_on_engine(asIScriptEngine* engine) {
    if (engine == nullptr) return SAO_ERR_INVALID_ARGUMENT;

    // as_str value type
    int r = engine->RegisterObjectType("string", sizeof(as_str),
        asOBJ_VALUE | asOBJ_APP_CLASS_CDAK);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT, "void f()",
        asFUNCTION(gen_str_construct), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT,
        "void f(const string &in)",
        asFUNCTION(gen_str_copy_construct), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectBehaviour("string", asBEHAVE_DESTRUCT, "void f()",
        asFUNCTION(gen_str_destruct), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterStringFactory("string", &g_str_factory);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectMethod("string",
        "string &opAssign(const string &in)",
        asFUNCTION(gen_str_assign), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectMethod("string",
        "string &opAddAssign(const string &in)",
        asFUNCTION(gen_str_add_assign), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectMethod("string",
        "bool opEquals(const string &in) const",
        asFUNCTION(gen_str_eq), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterObjectMethod("string",
        "string opAdd(const string &in) const",
        asFUNCTION(gen_str_add), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;

    // SDK 3 条全局函数
    r = engine->RegisterGlobalFunction("void log_info(const string &in)",
        asFUNCTION(gen_sdk_log_info), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterGlobalFunction("void register_ui_panel(const string &in)",
        asFUNCTION(gen_sdk_register_ui_panel), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;
    r = engine->RegisterGlobalFunction(
        "void register_hotkey(const string &in, const string &in)",
        asFUNCTION(gen_sdk_register_hotkey), asCALL_GENERIC);
    if (r < 0 && r != asALREADY_REGISTERED) return SAO_ERR_OS_CALL_FAILED;

    return SAO_OK;
}

#endif // SAO_HAS_ANGELSCRIPT

// ── 公开 API 实装 ────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_plugin(as_host_handle_t host,
                               const char* plugin_json_path_utf8,
                               as_plugin_handle_t* out_plugin,
                               char** out_error_utf8) {
    if (out_plugin) *out_plugin = nullptr;
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (host == nullptr || plugin_json_path_utf8 == nullptr) return SAO_ERR_INVALID_ARGUMENT;

#if defined(SAO_HAS_ANGELSCRIPT)
    asIScriptEngine* engine = sao_plugins_ashost_engine(host);
    if (engine == nullptr) return SAO_ERR_HANDLE_INVALID;

    std::string manifest_path = plugin_json_path_utf8;
    std::string manifest_body = read_file(manifest_path);
    if (manifest_body.empty()) {
        const char* m = "cannot read plugin.json";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string plugin_id = extract_json_string_field(manifest_body, "id");
    std::string entry = extract_json_string_field(manifest_body, "entry");
    if (entry.empty()) entry = "main.as";

    std::string dir = parent_dir(manifest_path);
    std::string entry_path = dir + "/" + entry;
    std::string entry_body = read_file(entry_path);
    if (entry_body.empty()) {
        std::string m = "cannot read entry script: " + entry_path;
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
        return SAO_ERR_HANDLE_INVALID;
    }

    // 注册 SDK 到 engine (幂等: 若 already registered 从 module 视角看 SDK 名字已存在)
    // AngelScript 里同名重注册会返 error, 我们靠 as_host_s 里的 sdk_registered
    // 标志避免. 但 as_host_s 是私有的, 这里改用简单策略: 尝试注册, 忽略"already exists"错误码.
    (void)register_sdk_on_engine(engine); // 幂等; 二次 fail 视为已经装过, ok

    // 建 module — 每 plugin 一个独立 module
    std::string module_name = plugin_id.empty() ? std::string("hello_angel") : plugin_id;
    asIScriptModule* mod = engine->GetModule(module_name.c_str(), asGM_ALWAYS_CREATE);
    if (mod == nullptr) return SAO_ERR_OS_CALL_FAILED;
    int r = mod->AddScriptSection(entry.c_str(), entry_body.c_str(), entry_body.size());
    if (r < 0) {
        const char* m = "AddScriptSection failed";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_INVALID_ARGUMENT;
    }
    r = mod->Build();
    if (r < 0) {
        const char* m = "AS Build failed (see as_host message log)";
        if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1)); if (*out_error_utf8) std::strcpy(*out_error_utf8, m); }
        return SAO_ERR_INVALID_ARGUMENT;
    }

    auto* plugin = new as_plugin_s();
    plugin->host = host;
    plugin->module = mod;
    plugin->plugin_id = std::move(plugin_id);
    plugin->entry_path = entry_path;

    // 分配 ctx (每 plugin 一个 context, 复用调 on_load/on_tick/on_unload)
    plugin->ctx = engine->CreateContext();
    if (plugin->ctx == nullptr) {
        delete plugin;
        return SAO_ERR_OS_CALL_FAILED;
    }
    plugin->ctx->SetUserData(plugin, 0x5A05DB01);

    // 调 on_load()
    asIScriptFunction* on_load = mod->GetFunctionByName("on_load");
    if (on_load) {
        plugin->ctx->Prepare(on_load);
        int rc = plugin->ctx->Execute();
        if (rc != asEXECUTION_FINISHED) {
            std::string m = "on_load did not finish (rc=" + std::to_string(rc) + ")";
            if (rc == asEXECUTION_EXCEPTION) {
                const char* ex = plugin->ctx->GetExceptionString();
                if (ex) { m += ": "; m += ex; }
            }
            if (out_error_utf8) { *out_error_utf8 = static_cast<char*>(std::malloc(m.size()+1)); if (*out_error_utf8) { std::memcpy(*out_error_utf8, m.data(), m.size()); (*out_error_utf8)[m.size()] = '\0'; } }
            plugin->ctx->Release();
            delete plugin;
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    plugin->loaded_ok = true;
    *out_plugin = plugin;
    return SAO_OK;
#else
    (void)host; (void)plugin_json_path_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_tick_plugin(as_plugin_handle_t plugin,
                               char** out_error_utf8) {
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (plugin->module == nullptr || plugin->ctx == nullptr) return SAO_ERR_HANDLE_INVALID;
    asIScriptFunction* on_tick = plugin->module->GetFunctionByName("on_tick");
    if (on_tick == nullptr) return SAO_ERR_HANDLE_INVALID;
    plugin->ctx->Prepare(on_tick);
    int rc = plugin->ctx->Execute();
    if (rc != asEXECUTION_FINISHED) {
        if (out_error_utf8) {
            const char* m = "on_tick did not finish";
            *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m)+1));
            if (*out_error_utf8) std::strcpy(*out_error_utf8, m);
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_plugin(as_plugin_handle_t plugin,
                                 char** out_error_utf8) {
    if (out_error_utf8) *out_error_utf8 = nullptr;
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (plugin->module && plugin->ctx) {
        asIScriptFunction* on_unload = plugin->module->GetFunctionByName("on_unload");
        if (on_unload) {
            plugin->ctx->Prepare(on_unload);
            plugin->ctx->Execute();
        }
    }
    if (plugin->ctx) { plugin->ctx->Release(); plugin->ctx = nullptr; }
    if (plugin->module) {
        asIScriptEngine* engine = plugin->module->GetEngine();
        if (engine) engine->DiscardModule(plugin->module->GetName());
        plugin->module = nullptr;
    }
    delete plugin;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_get_sdk_counters(as_plugin_handle_t plugin,
                                    as_sdk_counters* out_counters) {
    if (plugin == nullptr || out_counters == nullptr) return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    *out_counters = plugin->counters;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_read_global_int(as_plugin_handle_t plugin,
                                   const char* global_var_name_utf8,
                                   int32_t* out_value) {
    if (plugin == nullptr || global_var_name_utf8 == nullptr || out_value == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_value = 0;
#if defined(SAO_HAS_ANGELSCRIPT)
    if (plugin->module == nullptr) return SAO_ERR_HANDLE_INVALID;
    int idx = plugin->module->GetGlobalVarIndexByName(global_var_name_utf8);
    if (idx < 0) return SAO_ERR_HANDLE_INVALID;
    void* addr = plugin->module->GetAddressOfGlobalVar(idx);
    if (addr == nullptr) return SAO_ERR_HANDLE_INVALID;
    // 假设是 int32
    *out_value = *static_cast<int32_t*>(addr);
    return SAO_OK;
#else
    (void)global_var_name_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::angel_host
