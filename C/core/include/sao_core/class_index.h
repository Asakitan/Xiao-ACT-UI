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
