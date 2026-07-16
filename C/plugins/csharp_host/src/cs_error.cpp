// cs_error.cpp — stub
#include "sao/plugins/csharp_host/cs_error.h"

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_take_error(char** out_utf8) {
    if (out_utf8 != nullptr) *out_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_take_compile_errors(char** out_json_utf8) {
    if (out_json_utf8 != nullptr) *out_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::csharp_host
