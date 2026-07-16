// cs_call.cpp — Wave 4 首切片
//
// Wave 4: 只暴露 execute / call_function stub (Roslyn 通路留 Wave 5)。
// 已有 call_method / call_hook 保持 stub。

#include "sao/plugins/csharp_host/cs_call.h"

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_method(cs_assembly_handle_t /*assembly*/,
                               const char* /*class_full_name_utf8*/,
                               const char* /*method_name_utf8*/,
                               const char* /*args_json_utf8*/,
                               char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_call_hook(cs_assembly_handle_t /*assembly*/,
                             const char* /*hook_name*/,
                             const char* /*args_json_utf8*/,
                             char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

} // namespace sao::plugins::csharp_host
