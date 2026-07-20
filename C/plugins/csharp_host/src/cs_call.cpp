// cs_call.cpp — 保留的程序集调用 ABI
//
// call_method / call_hook 在预编译组件调用路径接入前保持 fail-closed。

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
