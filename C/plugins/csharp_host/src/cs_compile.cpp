// cs_compile.cpp — Wave 4 首切片: Roslyn stub
//
// Wave 4 只做通路探测, 真编译留 Wave 5:
//   - sao_plugins_cshost_compile_source: 检查 host 可用性, 返 stub 状态
//   - sao_plugins_cshost_load_assembly: 检查 host 可用性, 返 stub 状态
//   - sao_plugins_cshost_unload_assembly: no-op OK

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
    // Wave 5 会通过 hostfxr get_delegate → load_assembly_and_get_function_pointer
    // 拿到 SaoRoslynHelper.dll 里的 CompileFromSource entrypoint。当前 stub。
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
    // Wave 4: no assembly ever created, treat as OK
    return SAO_OK;
}

} // namespace sao::plugins::csharp_host
