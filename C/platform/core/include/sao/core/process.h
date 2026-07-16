// SAO Auto — process open/enumerate/info.
//
// 1:1 rewrite of `mem_probe/process.py` in native C.  The handle is opaque
// so the driver-mode backend can substitute a lease-based struct without
// breaking callers.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_core_process_s* sao_core_process_handle_t;

// Requested access rights.  READ is the platform's normal posture; INFO
// is for enumerators that just need module lists.
enum sao_process_access_e : uint32_t {
    SAO_PROCESS_ACCESS_INFO = 1u << 0,
    SAO_PROCESS_ACCESS_READ = 1u << 1,
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_process_open(
    uint32_t pid,
    uint32_t access_flags,
    sao_core_process_handle_t* out_handle);

SAO_CORE_API void SAO_CORE_CALL sao_core_process_close(
    sao_core_process_handle_t handle);

// Enumerate live processes.  out_pids may be null on a size query.  When
// out_pids is null, *out_pid_count receives the required count.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_process_enumerate(
    uint32_t* out_pids,
    size_t max_pids,
    size_t* out_pid_count);

// Find the first live process whose image name matches the given
// wide-char base name (case-insensitive), e.g. L"star_resonance.exe".
// Returns SAO_STATUS_ERR_NOT_FOUND if none exists.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_process_find_by_name(
    const wchar_t* image_base_name, uint32_t* out_pid);

struct SaoProcessInfo {
    uint32_t pid;
    uint32_t parent_pid;
    uint64_t start_time_100ns; // FILETIME encoding
    uint32_t session_id;
    uint32_t _pad;
    // image_path_utf8[] is filled in-place — callers pass a buffer of at
    // least SAO_PROCESS_IMAGE_PATH_MAX bytes.
};

enum : size_t { SAO_PROCESS_IMAGE_PATH_MAX = 1024 };

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_process_get_info(
    sao_core_process_handle_t process,
    SaoProcessInfo* out_info,
    char* out_image_path_utf8,
    size_t image_path_capacity);

// Enumerate loaded modules.  Base names are written back-to-back into
// out_names_utf8 as null-terminated strings.  out_bases[i] pairs with
// the i-th name.
struct SaoModuleEntry {
    uint64_t base_address;
    uint64_t module_size;
    uint32_t name_offset;  // offset into out_names_utf8
    uint32_t _pad;
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_process_enum_modules(
    sao_core_process_handle_t process,
    SaoModuleEntry* out_entries,
    size_t max_entries,
    size_t* out_entry_count,
    char* out_names_utf8,
    size_t names_capacity,
    size_t* out_names_used);

#ifdef __cplusplus
}  // extern "C"
#endif
