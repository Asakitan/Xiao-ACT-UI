#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/process.h"
#include "sao_core/sao_status.h"

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_class_index_resolve(
    sao_legacy_core_process_handle_t handle, const char* class_name_utf8, uint64_t* out_class_ptr);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_resolve_field_offset(sao_legacy_core_process_handle_t handle,
                                                 uint64_t class_ptr, const char* field_name_utf8,
                                                 uint32_t* out_offset);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_register(const char* class_name_utf8, uint32_t* out_index);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_find(const char* class_name_utf8, uint32_t* out_index);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_get_name(uint32_t index, char* out_class_name_utf8,
                                     size_t class_name_capacity, size_t* out_required_size);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_count(size_t* out_count);

// This provider-table ABI is independent from the legacy core export ABI in abi.h. Version 1.0
// describes the current retain/release and process-bound token lifecycle contract.
#define SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION_MAJOR 1u
#define SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION_MINOR 0u
#define SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION                                \
    ((SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION_MAJOR << 16u) |                  \
     SAO_LEGACY_CORE_CLASS_METADATA_PROVIDER_ABI_VERSION_MINOR)

typedef uint64_t sao_legacy_core_class_token_t;

// Provider class tokens are opaque provider-owned values. A successful resolve transfers one
// token lease to the core. The core calls release_class until one call succeeds; a failed or
// throwing cleanup keeps the token quarantined for explicit retry and never counts as release.
// The process handle is borrowed only for the duration of each callback. Providers must keep
// returned tokens valid until a successful release and support callbacks from multiple threads.
struct SaoLegacyCoreClassMetadataProvider {
    uint32_t struct_size;
    uint32_t abi_version;
    void* user_data;
    void(SAO_LEGACY_CORE_CALL* retain)(void* user_data);
    void(SAO_LEGACY_CORE_CALL* release)(void* user_data);
    int32_t(SAO_LEGACY_CORE_CALL* resolve_class)(
        void* user_data, sao_legacy_core_process_handle_t process, const char* class_name_utf8,
        uint64_t* out_provider_class_token);
    int32_t(SAO_LEGACY_CORE_CALL* resolve_field_offset)(
        void* user_data, sao_legacy_core_process_handle_t process, uint64_t provider_class_token,
        const char* field_name_utf8, uint32_t* out_offset);
    void(SAO_LEGACY_CORE_CALL* release_class)(void* user_data,
                                              uint64_t provider_class_token);
};

// Installs a copied provider table and retains its owner. Passing nullptr clears the current
// provider. Replacement retains the validated candidate, retires the old provider for new calls,
// drains in-flight callbacks, releases old class tokens, and releases the old retained owner.
// Cleanup failure keeps old ownership quarantined, rolls the candidate retain back (quarantining
// that owner too if rollback fails), and can be retried by calling this function again. No
// replacement is installed until every old-owner cleanup callback succeeds.
extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_configure_provider(
    const SaoLegacyCoreClassMetadataProvider* provider);

// Releases one process-bound opaque token returned by class_index_resolve. If release_class fails,
// the same handle/token pair remains valid only for another release attempt; resolution through
// that token stays fail-closed. Callers release every token before closing its process handle. The
// token is not a target address and must never be dereferenced or used with process memory APIs.
extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_class_index_release(sao_legacy_core_process_handle_t handle,
                                    sao_legacy_core_class_token_t class_token);
