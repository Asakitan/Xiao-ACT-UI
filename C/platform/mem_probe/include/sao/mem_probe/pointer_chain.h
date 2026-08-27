// mem_probe/pointer_chain.h — BFS pointer-chain backtrace + resolve.
//
// Port of python/mem_probe/pointer_chain.py.
//   backtrace(process, target_addr, depth) → chain (module + static_offset + deref_offsets)
//   resolve(process, chain) → current final address (after process restart / module rebase)
//   verify(process, chain) → chain still resolves + reads back a readable value

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
// resolve 对每个 deref offset 读取并进入子层；最后再读取 root 层指针并应用 final_offset。
// module_name_utf8 是 caller 提供 buffer (至少 128 字节)。这是旧 ABI 布局；
// 扩展字段只能放入 sao_memprobe_ptr_chain_v2_t。
typedef struct sao_memprobe_ptr_chain_s {
    char module_name_utf8[128];
    uint64_t module_base_at_find;
    int64_t static_offset;
    int32_t deref_offsets[16];
    uint32_t deref_count;
    int32_t final_offset;
} sao_memprobe_ptr_chain_t;

enum : uint32_t {
    SAO_MEMPROBE_PTR_CHAIN_V2_ABI_VERSION = 2u,
    SAO_MEMPROBE_PTR_CHAIN_ABI_VERSION = SAO_MEMPROBE_PTR_CHAIN_V2_ABI_VERSION,
    SAO_MEMPROBE_PTR_CHAIN_PROVENANCE_MAX = 64u,
};

typedef struct sao_memprobe_ptr_chain_v2_s {
    char module_name_utf8[128];
    uint64_t module_base_at_find;
    int64_t static_offset;
    int32_t deref_offsets[16];
    uint32_t deref_count;
    int32_t final_offset;
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t process_pid_at_find;
    uint32_t _reserved;
    uint64_t module_fingerprint;
    uint64_t anchor_address_at_find;
    char provenance_utf8[SAO_MEMPROBE_PTR_CHAIN_PROVENANCE_MAX];
    uint32_t validation_state;
    uint32_t _reserved2;
} sao_memprobe_ptr_chain_v2_t;

enum : uint32_t {
    SAO_MEMPROBE_PTR_CHAIN_VALIDATION_CANDIDATE = 0u,
    SAO_MEMPROBE_PTR_CHAIN_VALIDATION_VALIDATED = 1u,
};

typedef sao_memprobe_ptr_chain_v2_t SaoPointerChainV2;

typedef struct sao_memprobe_backtrace_config_s {
    uint32_t max_depth;             // default 3
    uint32_t max_ptr_per_level;     // default 8192
    uint64_t max_scan_bytes_budget; // default 256 MiB
} sao_memprobe_backtrace_config_t;

enum : uint32_t {
    SAO_MEMPROBE_BACKTRACE_CONFIG_V2_ABI_VERSION = 2u,
};

typedef struct sao_memprobe_backtrace_config_v2_s {
    uint32_t max_depth;
    uint32_t max_ptr_per_level;
    uint64_t max_scan_bytes_budget;
    uint32_t max_read_count;
    uint32_t max_duration_ms;
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t _reserved;
    uint32_t _reserved2;
} sao_memprobe_backtrace_config_v2_t;

// BFS backtrace from heap target_addr to PE static-data sections.
// Candidates are diagnostic output; only sentinel-validated chains persist.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_backtrace(
    sao_core_process_handle_t process, uint64_t target_addr,
    const sao_memprobe_backtrace_config_t* cfg, sao_memprobe_ptr_chain_t* out_chain);

// Resolve a saved chain against current process state.
SAO_CORE_API sao_status_t SAO_CORE_CALL
sao_memprobe_ptr_chain_resolve(sao_core_process_handle_t process,
                               const sao_memprobe_ptr_chain_t* chain, uint64_t* out_final_addr);

// Verify chain shape, module fingerprint, same-PID anchor provenance when available, and a readable
// final value.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_verify(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_t* chain);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_backtrace_v2(
    sao_core_process_handle_t process, uint64_t target_addr,
    const sao_memprobe_backtrace_config_v2_t* cfg, sao_memprobe_ptr_chain_v2_t* out_chain);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_resolve_v2(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_v2_t* chain,
    uint64_t* out_final_addr);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_ptr_chain_verify_v2(
    sao_core_process_handle_t process, const sao_memprobe_ptr_chain_v2_t* chain);

#ifdef __cplusplus
} // extern "C"
#endif