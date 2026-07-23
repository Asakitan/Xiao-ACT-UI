#include "sao/plugins/csharp_host/cs_module_bridge.h"

#include "cs_sdk_bridge_internal.h"

#include <cstddef>
#include <cstring>

namespace sao::plugins::csharp_host {

// Flat function-pointer projection of the compiled cs_managed_sdk_table.
// The projection derives directly from the table struct layout: the two
// leading uint32_t fields (struct_size and abi_version) are skipped, and every
// subsequent slot is exposed as a void*. Appending a slot in the header grows
// this projection automatically without editing per-slot copy statements.
namespace {
constexpr size_t kSdkTablePointerPrefix = sizeof(uint32_t) * 2;
constexpr size_t kSdkTablePointerCount =
    (sizeof(cs_managed_sdk_table) - kSdkTablePointerPrefix) / sizeof(void*);
static_assert(kSdkTablePointerCount * sizeof(void*) + kSdkTablePointerPrefix ==
                  sizeof(cs_managed_sdk_table),
              "cs_managed_sdk_table layout must be pointer-aligned after the version prefix");
static_assert(kSdkTablePointerCount ==
                  (SAO_CSHOST_SDK_TABLE_CURRENT_SIZE - kSdkTablePointerPrefix) / sizeof(void*),
              "flat pointer projection must mirror the current append-only table size");
} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_inject_ctx(cs_domain_handle_t /*domain*/, void* /*ctx_handle*/) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_table(void** out_fn_ptrs, size_t* inout_count) {
    if (inout_count == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const size_t required = kSdkTablePointerCount;
    if (out_fn_ptrs == nullptr || *inout_count < required) {
        *inout_count = required;
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    const auto* table = cshost_sdk_bridge_table();
    // Copy the pointer window after the version prefix as a single block so
    // future appends to cs_managed_sdk_table lift here without editing per-slot
    // statements. The managed side reads struct_size and iterates using
    // safe_readable_extent to skip slots it was not compiled to know about.
    std::memcpy(out_fn_ptrs,
                reinterpret_cast<const std::byte*>(table) + kSdkTablePointerPrefix,
                required * sizeof(void*));
    *inout_count = required;
    return SAO_OK;
}

} // namespace sao::plugins::csharp_host
