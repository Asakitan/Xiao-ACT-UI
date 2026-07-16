// as_module_bridge.cpp — stub
#include "sao/plugins/angel_host/as_module_bridge.h"

namespace sao::plugins::angel_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_register_sdk(asIScriptEngine* /*engine*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_bind_ctx(asIScriptEngine* /*engine*/,
                            void* /*ctx_handle*/,
                            const char* /*module_name*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::angel_host
