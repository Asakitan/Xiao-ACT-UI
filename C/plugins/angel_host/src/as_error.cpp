// as_error.cpp — stub
#include "sao/plugins/angel_host/as_error.h"

namespace sao::plugins::angel_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_take_exception(asIScriptContext* /*ctx*/, char** out_utf8) {
    if (out_utf8 != nullptr) *out_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::angel_host
