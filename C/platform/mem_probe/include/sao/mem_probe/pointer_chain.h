// mem_probe/pointer_chain.h — BFS pointer-chain backtrace + resolve.
//
// Port of python/mem_probe/pointer_chain.py.
//   backtrace(process, target_addr, depth) → chain (module + static_offset + deref_offsets)
//   resolve(process, chain) → current final address (after process restart / module rebase)
//   verify(process, chain) → chain still resolves + reads back to something != 0

#pragma once

#include "sao/core/process.h"
#include "sao/core/status.h"

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 每层 deref: (source_addr, deref_offset).
typedef struct sao_memprobe_ptr_link_s {
    uint64_t source_addr;
    int32_t offset;
    uint32_t _pad;
} sao_memprobe_ptr_link_t;

// 完整链: module_name + module_base_at_find + static_offset + deref_offsets[] + final_offset。
// module_name_utf8 是 caller 提供 buffer (至少 128 字节)。
typedef struct sao_memprobe_ptr_chain_s {
    char module_name_utf8[128];
    uint64_t module_base_at_find;
    int64_t static_offset;
    int32_t deref_offsets[16];
    uint32_t deref_count;
    int32_t final_offset;
} sao_memprobe_ptr_chain_t;

typedef struct sao_memprobe_backtrace_config_s {
    uint32_t max_depth;             // default 3
    uint32_t max_ptr_per_level;     // default 8192
    uint64_t max_scan_bytes_budget; // default 256 MiB
} sao_memprobe_backtrace_config_t;

// BFS backtrace from heap target_addr to module static section.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_backtrace(
    sao_core_process_handle_t process,
    uint64_t target_addr,
    const sao_memprobe_backtrace_config_t* cfg,
    sao_memprobe_ptr_chain_t* out_chain);

// Resolve a saved chain against current process state.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_resolve(
    sao_core_process_handle_t process,
    const sao_memprobe_ptr_chain_t* chain,
    uint64_t* out_final_addr);

// Sanity check: resolve + read u64 succeed.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_verify(
    sao_core_process_handle_t process,
    const sao_memprobe_ptr_chain_t* chain);

#ifdef __cplusplus
} // extern "C"
#endif
