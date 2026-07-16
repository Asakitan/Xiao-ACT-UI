// SAO Auto — remote process memory read primitives.
//
// This is the platform's answer to `mem_probe/rt_io.py`.  Read-only.  Write
// primitives are deliberately absent — memory writes are a driver-level
// concern that lives under `../security/kernel/`, not here.
//
// Backends:
//   * default: NtReadVirtualMemory resolved from ntdll (user-mode).
//   * optional: kernel driver bridge (registered by Agent 2 during boot;
//     this header does not depend on it — the driver simply overrides the
//     internal read function pointer via a private hook).
//
// All reads accept a process handle from `process.h` and return the
// number of bytes successfully read.  Short reads (page unmapped mid-buffer)
// return SAO_STATUS_ERR_READ_FAULT and leave *out_bytes_read at however far
// the read got.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/process.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_read(
    sao_core_process_handle_t process,
    uint64_t address,
    void* out_buffer,
    size_t buffer_len,
    size_t* out_bytes_read);

// Typed convenience readers — thin wrappers over sao_core_mem_read.
// Fail (SAO_STATUS_ERR_READ_FAULT) rather than returning a garbage value
// on a short read; callers who prefer optionals wrap them.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_read_u32(
    sao_core_process_handle_t process, uint64_t address, uint32_t* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_read_u64(
    sao_core_process_handle_t process, uint64_t address, uint64_t* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_read_i32(
    sao_core_process_handle_t process, uint64_t address, int32_t* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_read_ptr(
    sao_core_process_handle_t process, uint64_t address, uint64_t* out_value);

// Resolves the conventional Python/pymem pointer chain: read a pointer at
// base_address, dereference again after every offset except the last, then
// add the final signed offset.  offset_count == 0 returns base_address.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_read_pointer_chain(
    sao_core_process_handle_t process,
    uint64_t base_address,
    const int32_t* offsets,
    size_t offset_count,
    uint64_t* out_final_address);

// Region enumeration for scan planning.  Consumers walk MEM_COMMIT +
// readable regions only.  See Python `mem_probe/process.py`.
struct SaoMemRegion {
    uint64_t base_address;
    uint64_t region_size;
    uint32_t protect;   // Win32 PAGE_* flags
    uint32_t state;     // Win32 MEM_* flags
    uint32_t type;      // MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED
    uint32_t _pad;
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_mem_enum_regions(
    sao_core_process_handle_t process,
    SaoMemRegion* out_regions,
    size_t max_regions,
    size_t* out_region_count);

#ifdef __cplusplus
}  // extern "C"
#endif
