// as_call.cpp — AngelScript 生命周期 hook (stub, wave3 走 as_host_execute 便利路径)

#include "sao/plugins/angel_host/as_call.h"

namespace sao::plugins::angel_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_load_script(asIScriptEngine* /*engine*/,
                               const wchar_t* /*plugin_dir*/,
                               const char* /*entry_relative*/,
                               const char* /*plugin_id_utf8*/,
                               void* /*ctx_ptr*/,
                               as_plugin_handle_t* out_plugin) {
    if (out_plugin != nullptr) *out_plugin = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_load(as_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_enable(as_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_disable(as_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_on_unload(as_plugin_handle_t /*plugin*/, bool* out_allow_unload) {
    if (out_allow_unload) *out_allow_unload = true;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_hook(as_plugin_handle_t /*plugin*/,
                             const char* /*hook_name*/,
                             const char* /*args_json_utf8*/,
                             char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_has_hook(as_plugin_handle_t /*plugin*/, const char* /*hook_name*/) {
    return false;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_call_function(asIScriptEngine* /*engine*/,
                                 asIScriptFunction* /*fn*/,
                                 const char* /*args_json_utf8*/,
                                 char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unload_script(as_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API asIScriptModule* SAO_PLUGINS_CALL
sao_plugins_ashost_get_module(as_plugin_handle_t /*plugin*/) {
    return nullptr;
}

} // namespace sao::plugins::angel_host
