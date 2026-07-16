// as_stdlib.cpp — stub
#include "sao/plugins/angel_host/as_stdlib.h"

namespace sao::plugins::angel_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_install_stdlib(asIScriptEngine* /*engine*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::angel_host
