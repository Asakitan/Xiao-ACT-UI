// as_sandbox.cpp — stub
#include "sao/plugins/angel_host/as_sandbox.h"

namespace sao::plugins::angel_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_arm(asIScriptEngine* /*engine*/,
                               const as_sandbox_config* /*cfg*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::angel_host
