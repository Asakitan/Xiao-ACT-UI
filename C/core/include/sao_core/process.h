#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/sao_status.h"

typedef struct sao_legacy_core_process_s* sao_legacy_core_process_handle_t;

struct SaoLegacyCoreProcessInfo {
    uint32_t pid;
    uint32_t parent_pid;
    uint64_t start_time_100ns;
    uint32_t session_id;
    uint32_t reserved;
};

enum : size_t { SAO_LEGACY_CORE_PROCESS_IMAGE_PATH_MAX = 1024 };

// sao_legacy_core_process_close() is the matching close for this open.
extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_process_open(uint32_t pid, sao_legacy_core_process_handle_t* out_handle);

extern "C" SAO_LEGACY_CORE_API void SAO_LEGACY_CORE_CALL
sao_legacy_core_process_close(sao_legacy_core_process_handle_t handle);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_process_get_info(
    sao_legacy_core_process_handle_t handle, SaoLegacyCoreProcessInfo* out_info,
    char* out_image_path_utf8, size_t image_path_capacity);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_read_bytes(sao_legacy_core_process_handle_t handle, uint64_t address,
                           uint8_t* out_buffer, size_t buffer_len, size_t* out_bytes_read);
