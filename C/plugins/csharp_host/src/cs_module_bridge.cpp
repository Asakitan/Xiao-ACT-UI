#include "sao/plugins/csharp_host/cs_module_bridge.h"

#include "cs_sdk_bridge_internal.h"

namespace sao::plugins::csharp_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_inject_ctx(cs_domain_handle_t /*domain*/, void* /*ctx_handle*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_table(void** out_fn_ptrs, size_t* inout_count) {
    if (inout_count == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    constexpr size_t required = 8;
    if (out_fn_ptrs == nullptr || *inout_count < required) {
        *inout_count = required;
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    const auto* table = cshost_sdk_bridge_table();
    out_fn_ptrs[0] = reinterpret_cast<void*>(table->dispatch);
    out_fn_ptrs[1] = reinterpret_cast<void*>(table->release_callback);
    out_fn_ptrs[2] = reinterpret_cast<void*>(table->log);
    out_fn_ptrs[3] = reinterpret_cast<void*>(table->register_engine);
    out_fn_ptrs[4] = reinterpret_cast<void*>(table->get_engine);
    out_fn_ptrs[5] = reinterpret_cast<void*>(table->register_entity_provider);
    out_fn_ptrs[6] = reinterpret_cast<void*>(table->unregister_entity_provider);
    out_fn_ptrs[7] = reinterpret_cast<void*>(table->last_error);
    *inout_count = required;
    return SAO_OK;
}

} // namespace sao::plugins::csharp_host
