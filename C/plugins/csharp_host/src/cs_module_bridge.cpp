// cs_module_bridge.cpp — stub
#include "sao/plugins/csharp_host/cs_module_bridge.h"

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_inject_ctx(cs_domain_handle_t /*domain*/, void* /*ctx_handle*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::csharp_host
