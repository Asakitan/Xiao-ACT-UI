// lua_call.cpp — Lua 生命周期 hook 分派 (stub, load_script 需要文件, wave3 通过 lua_host_execute 走)
//
// 完整实装留给后续 wave; wave3 只测 execute / call_function 便利入口 (在 lua_host.cpp 里)。

#include "sao/plugins/lua_host/lua_call.h"

namespace sao::plugins::lua_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_load_script(lua_State* /*L*/,
                                const wchar_t* /*plugin_dir*/,
                                const char* /*entry_relative*/,
                                const char* /*plugin_id_utf8*/,
                                void* /*ctx_ptr*/,
                                lua_plugin_handle_t* out_plugin) {
    if (out_plugin != nullptr) *out_plugin = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_load(lua_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_enable(lua_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_disable(lua_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_unload(lua_plugin_handle_t /*plugin*/, bool* out_allow_unload) {
    if (out_allow_unload) *out_allow_unload = true;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_hook(lua_plugin_handle_t /*plugin*/,
                              const char* /*hook_name*/,
                              const char* /*args_json_utf8*/,
                              char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_has_hook(lua_plugin_handle_t /*plugin*/, const char* /*hook_name*/) {
    return false;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_unload_script(lua_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function_by_ref(lua_State* /*L*/,
                                         int /*registry_ref*/,
                                         const char* /*args_json_utf8*/,
                                         char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::lua_host
