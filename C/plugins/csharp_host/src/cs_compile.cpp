// Legacy reserved ABI. Generic loading is implemented by cs_component.cpp and
// accepts precompiled DLLs only.

#include "sao/plugins/csharp_host/cs_compile.h"

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_compile_source(cs_domain_handle_t domain,
                                  const wchar_t* /*source_dir*/,
                                  const wchar_t* /*entry_relative*/,
                                  const cs_compile_options* /*opts*/,
                                  cs_assembly_handle_t* out_assembly) {
    if (out_assembly != nullptr) *out_assembly = nullptr;
    if (domain == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_assembly(cs_domain_handle_t domain,
                                 const wchar_t* /*assembly_path*/,
                                 cs_assembly_handle_t* out_assembly) {
    if (out_assembly != nullptr) *out_assembly = nullptr;
    if (domain == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unload_assembly(cs_assembly_handle_t /*assembly*/) {
    // Reserved compile path creates no assembly, so unload is a no-op.
    return SAO_OK;
}

} // namespace sao::plugins::csharp_host
