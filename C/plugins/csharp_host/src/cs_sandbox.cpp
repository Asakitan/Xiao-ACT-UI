// cs_sandbox.cpp — stub
#include "sao/plugins/csharp_host/cs_sandbox.h"

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_sandbox_arm(cs_domain_handle_t /*domain*/,
                               const cs_sandbox_config* /*cfg*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::csharp_host
