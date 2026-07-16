// py_call.cpp — legacy stub 保留 (sao_plugins_pyhost_call_hook 已被 py_host.cpp
// Wave 7 真实装; 这里只留 load_script / unload_script 旧接口)。
#include "sao/plugins/python_host/py_call.h"

namespace sao::plugins::python_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_load_script(const wchar_t* /*plugin_dir*/,
                               const char* /*entry_relative*/,
                               const char* /*plugin_id_utf8*/,
                               py_plugin_handle_t* out_plugin) {
    if (out_plugin != nullptr) *out_plugin = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unload_script(py_plugin_handle_t /*plugin*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::python_host
