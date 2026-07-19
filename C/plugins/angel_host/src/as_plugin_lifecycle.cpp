// as_plugin_lifecycle.cpp — Wave 8 / Agent d Phase 8 实装
//
// SDK 3 条 C 函数被注册到 asIScriptEngine 全局命名空间, 用 asCALL_CDECL.
// 每条内部通过 asGetActiveContext()->GetUserData() 拿到 as_plugin_s* 更新计数器.

#include "sao/plugins/angel_host/as_plugin_lifecycle.h"

#include "as_plugin_internal.h"

#include "sao/plugins/angel_host/as_call.h"
#include "sao/plugins/angel_host/as_error.h"
#include "sao/plugins/angel_host/as_module_bridge.h"
#include "sao/plugins/angel_host/as_stdlib.h"

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
    if (pos == std::string::npos)
        return {};
    pos += needle.size();
    while (pos < src.size() &&
           (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
    if (pos >= src.size() || src[pos] != ':')
        return {};
    ++pos;
    while (pos < src.size() &&
           (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
    if (pos >= src.size() || src[pos] != '"')
        return {};
    ++pos;
    std::string out;
    while (pos < src.size() && src[pos] != '"') {
        char c = src[pos++];
        if (c == '\\' && pos < src.size()) {
            char e = src[pos++];
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case '\\':
                out += '\\';
                break;
            case '"':
                out += '"';
                break;
            default:
                out += e;
                break;
            }
        } else
            out += c;
    }
    return out;
}

std::string parent_dir(const std::string& p) {
    auto pos = p.find_last_of("\\/");
    if (pos == std::string::npos)
        return ".";
    return p.substr(0, pos);
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open())
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int32_t copy_error(const std::string& message, char** output, int32_t status) {
    if (output == nullptr)
        return status;
    auto* buffer = static_cast<char*>(std::malloc(message.size() + 1));
    if (buffer == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(buffer, message.c_str(), message.size() + 1);
    *output = buffer;
    return status;
}

int32_t take_execution_error(asIScriptContext* context, char** output, const char* fallback) {
    if (context != nullptr && context->GetState() == asEXECUTION_EXCEPTION && output != nullptr) {
        return sao_plugins_ashost_take_exception(context, output);
    }
    return copy_error(fallback, output, SAO_ERR_OS_CALL_FAILED);
}

void release_plugin_runtime(as_plugin_s* plugin) noexcept {
    if (plugin == nullptr)
        return;
    if (plugin->context != nullptr) {
        plugin->context->SetUserData(nullptr, kPluginContextUserDataSlot);
        plugin->context->Release();
        plugin->context = nullptr;
    }
    if (plugin->module != nullptr && plugin->engine != nullptr) {
        plugin->engine->DiscardModule(plugin->module_name.c_str());
        plugin->module = nullptr;
    }
}
} // namespace

// 注册 string 类型 + 3 SDK 函数 到 engine
static int32_t register_sdk_on_engine(asIScriptEngine* engine) {
    return sao_plugins_ashost_register_sdk(engine);
}

#endif // SAO_HAS_ANGELSCRIPT

// ── 公开 API 实装 ────────────────────────────────

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_plugin(as_host_handle_t host, const char* plugin_json_path_utf8,
                               as_plugin_handle_t* out_plugin, char** out_error_utf8) {
    if (out_plugin)
        *out_plugin = nullptr;
    if (out_error_utf8)
        *out_error_utf8 = nullptr;
    if (host == nullptr || plugin_json_path_utf8 == nullptr || out_plugin == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

#if defined(SAO_HAS_ANGELSCRIPT)
    asIScriptEngine* engine = sao_plugins_ashost_engine(host);
    if (engine == nullptr)
        return SAO_ERR_HANDLE_INVALID;

    std::string manifest_path = plugin_json_path_utf8;
    std::string manifest_body = read_file(manifest_path);
    if (manifest_body.empty()) {
        const char* m = "cannot read plugin.json";
        if (out_error_utf8) {
            *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (*out_error_utf8)
                std::strcpy(*out_error_utf8, m);
        }
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string plugin_id = extract_json_string_field(manifest_body, "id");
    std::string entry = extract_json_string_field(manifest_body, "entry");
    if (entry.empty())
        entry = "main.as";

    std::string dir = parent_dir(manifest_path);
    std::string entry_path = dir + "/" + entry;
    std::string entry_body = read_file(entry_path);
    if (entry_body.empty()) {
        std::string m = "cannot read entry script: " + entry_path;
        if (out_error_utf8) {
            *out_error_utf8 = static_cast<char*>(std::malloc(m.size() + 1));
            if (*out_error_utf8) {
                std::memcpy(*out_error_utf8, m.data(), m.size());
                (*out_error_utf8)[m.size()] = '\0';
            }
        }
        return SAO_ERR_HANDLE_INVALID;
    }

    const int32_t registration_status = register_sdk_on_engine(engine);
    if (registration_status != SAO_OK)
        return registration_status;
    const int32_t stdlib_status = sao_plugins_ashost_install_stdlib(engine);
    if (stdlib_status != SAO_OK && stdlib_status != SAO_ERR_NOT_IMPLEMENTED) {
        return stdlib_status;
    }

    // 建 module — 每 plugin 一个独立 module
    std::string module_name = plugin_id.empty() ? std::string("hello_angel") : plugin_id;
    asIScriptModule* mod = engine->GetModule(module_name.c_str(), asGM_ALWAYS_CREATE);
    if (mod == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    int r = mod->AddScriptSection(entry.c_str(), entry_body.c_str(), entry_body.size());
    if (r < 0) {
        const char* m = "AddScriptSection failed";
        if (out_error_utf8) {
            *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (*out_error_utf8)
                std::strcpy(*out_error_utf8, m);
        }
        engine->DiscardModule(module_name.c_str());
        return SAO_ERR_INVALID_ARGUMENT;
    }
    r = mod->Build();
    if (r < 0) {
        const char* m = "AS Build failed (see as_host message log)";
        if (out_error_utf8) {
            *out_error_utf8 = static_cast<char*>(std::malloc(std::strlen(m) + 1));
            if (*out_error_utf8)
                std::strcpy(*out_error_utf8, m);
        }
        engine->DiscardModule(module_name.c_str());
        return SAO_ERR_INVALID_ARGUMENT;
    }

    auto plugin = std::make_shared<as_plugin_s>();
    plugin->engine = engine;
    plugin->module = mod;
    plugin->plugin_id = std::move(plugin_id);
    plugin->module_name = module_name;

    // 分配 ctx (每 plugin 一个 context, 复用调 on_load/on_tick/on_unload)
    plugin->context = engine->CreateContext();
    if (plugin->context == nullptr) {
        release_plugin_runtime(plugin.get());
        return SAO_ERR_OS_CALL_FAILED;
    }
    plugin->context->SetUserData(plugin.get(), kPluginContextUserDataSlot);

    int32_t status = register_plugin_state(plugin);
    if (status != SAO_OK) {
        release_plugin_runtime(plugin.get());
        return status;
    }
    status = sao_plugins_ashost_call_on_load(plugin.get());
    if (status != SAO_OK) {
        const std::string error =
            plugin_error_text(plugin.get(), "on_load", "AngelScript on_load did not finish");
        (void)copy_error(error, out_error_utf8, status);
        (void)sao_plugins_ashost_unload_script(plugin.get());
        return status;
    }

    *out_plugin = plugin.get();
    return SAO_OK;
#else
    (void)host;
    (void)plugin_json_path_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_tick_plugin(as_plugin_handle_t plugin, char** out_error_utf8) {
    if (out_error_utf8)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    const shared_plugin_state state = acquire_plugin_state(plugin);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    std::lock_guard lock(state->call_mutex);
    if (state->lifecycle != plugin_runtime_state::ready || state->module == nullptr ||
        state->context == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    asIScriptFunction* on_tick = state->module->GetFunctionByName("on_tick");
    if (on_tick == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    clear_plugin_error(*state, "on_tick");
    const int prepare_status = state->context->Prepare(on_tick);
    const int rc = prepare_status < 0 ? prepare_status : state->context->Execute();
    if (rc != asEXECUTION_FINISHED) {
        retain_plugin_error(*state, "on_tick", SAO_ERR_OS_CALL_FAILED,
                            "AngelScript on_tick did not finish", state->context);
        return take_execution_error(state->context, out_error_utf8,
                                    "AngelScript on_tick did not finish");
    }
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_plugin(as_plugin_handle_t plugin, char** out_error_utf8) {
    if (out_error_utf8)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    bool allow_unload = true;
    const int32_t hook_status = sao_plugins_ashost_call_on_unload(plugin, &allow_unload);
    if (hook_status != SAO_OK) {
        const std::string error =
            plugin_error_text(plugin, "on_unload", "AngelScript on_unload did not finish");
        (void)copy_error(error, out_error_utf8, hook_status);
        return hook_status;
    }
    if (!allow_unload) {
        (void)copy_error("AngelScript on_unload vetoed unload", out_error_utf8,
                         sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    const int32_t unload_status = sao_plugins_ashost_unload_script(plugin);
    return unload_status;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_get_sdk_counters(as_plugin_handle_t plugin, as_sdk_counters* out_counters) {
    if (plugin == nullptr || out_counters == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_ANGELSCRIPT)
    const shared_plugin_state state = acquire_plugin_state(plugin);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    std::lock_guard lock(state->call_mutex);
    if (state->lifecycle != plugin_runtime_state::ready)
        return SAO_ERR_HANDLE_INVALID;
    *out_counters = state->counters;
    return SAO_OK;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_read_global_int(
    as_plugin_handle_t plugin, const char* global_var_name_utf8, int32_t* out_value) {
    if (plugin == nullptr || global_var_name_utf8 == nullptr || out_value == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_value = 0;
#if defined(SAO_HAS_ANGELSCRIPT)
    const shared_plugin_state state = acquire_plugin_state(plugin);
    if (!state)
        return SAO_ERR_HANDLE_INVALID;
    std::lock_guard lock(state->call_mutex);
    if (state->lifecycle != plugin_runtime_state::ready || state->module == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    int idx = state->module->GetGlobalVarIndexByName(global_var_name_utf8);
    if (idx < 0)
        return SAO_ERR_HANDLE_INVALID;
    void* addr = state->module->GetAddressOfGlobalVar(idx);
    if (addr == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    // 假设是 int32
    *out_value = *static_cast<int32_t*>(addr);
    return SAO_OK;
#else
    (void)global_var_name_utf8;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

} // namespace sao::plugins::angel_host
