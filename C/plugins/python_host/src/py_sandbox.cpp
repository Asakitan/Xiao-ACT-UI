// py_sandbox.cpp — stub
#include "sao/plugins/python_host/py_sandbox.h"

namespace sao::plugins::python_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_arm(const char* /*plugin_id_utf8*/,
                               const py_sandbox_config* /*cfg*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_sandbox_disarm(const char* /*plugin_id_utf8*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::python_host
