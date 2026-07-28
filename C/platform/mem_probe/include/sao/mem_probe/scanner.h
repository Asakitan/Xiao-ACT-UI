// mem_probe/scanner.h — CE-style multi-frame value scanner.
//
// 1:1 port of python/mem_probe/scanner.py:
//   - scan_first(process, dtype, value)   — 全内存首次扫; 返 candidate addr list
//   - scan_narrow(process, prev, dtype, value)   — 上轮 candidate 中 exact match
//   - scan_predicate(process, prev, kind)  — CHANGED/UNCHANGED/INCREASED/DECREASED
//
// 支持 dtype: u8/u16/u32/u64/i8/i16/i32/i64/f32/f64。align 默认 dtype size,
// 可 override 为 1 找非对齐。AVX2 加速 u32/u64/i32/i64。
//
// 依赖 sao::core 的 SaoCoreProcess (VirtualQueryEx region enum + ReadProcessMemory).

#pragma once

#include "sao/core/process.h"
#include "sao/core/status.h"

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// dtype tag — 与 python scanner.py 的字符串对应。
typedef enum sao_memprobe_dtype_e {
    SAO_MEMPROBE_DTYPE_U8 = 0,
    SAO_MEMPROBE_DTYPE_U16,
    SAO_MEMPROBE_DTYPE_U32,
    SAO_MEMPROBE_DTYPE_U64,
    SAO_MEMPROBE_DTYPE_I8,
    SAO_MEMPROBE_DTYPE_I16,
    SAO_MEMPROBE_DTYPE_I32,
    SAO_MEMPROBE_DTYPE_I64,
    SAO_MEMPROBE_DTYPE_F32,
    SAO_MEMPROBE_DTYPE_F64,
} sao_memprobe_dtype_t;

typedef enum sao_memprobe_predicate_e {
    SAO_MEMPROBE_PRED_CHANGED = 0,
    SAO_MEMPROBE_PRED_UNCHANGED,
    SAO_MEMPROBE_PRED_INCREASED,
    SAO_MEMPROBE_PRED_DECREASED,
} sao_memprobe_predicate_t;

// 单个 candidate: 地址 + 上次读到的值 (predicate scan 用)。
typedef struct sao_memprobe_candidate_s {
    uint64_t addr;
    uint64_t last_value; // 对 float/double 走 bit-cast
} sao_memprobe_candidate_t;

typedef struct sao_memprobe_scan_bounds_s {
    uint32_t max_addrs;      // 0 = unlimited
    uint32_t max_duration_ms; // 0 = 30s default
    uint32_t align;          // 0 = dtype-default
} sao_memprobe_scan_bounds_t;

// 首次扫描: 遍历所有 committed private/mapped region, 找 dtype value 出现处。
// out_candidates 由 caller 提供 buffer, cap 是最大写入数; 实际写入到 *out_count。
// 若 cap 满则返回 SAO_ERR_RESOURCE_EXHAUSTED (但已写的 candidates 有效)。
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_scan_first(
    sao_core_process_handle_t process,
    sao_memprobe_dtype_t dtype,
    uint64_t value,
    const sao_memprobe_scan_bounds_t* bounds,
    sao_memprobe_candidate_t* out_candidates,
    uint32_t cap,
    uint32_t* out_count);

// 上轮 candidates 中筛出仍 == value 的 (exact match)。
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_scan_narrow(
    sao_core_process_handle_t process,
    sao_memprobe_dtype_t dtype,
    uint64_t value,
    const sao_memprobe_candidate_t* prev,
    uint32_t prev_count,
    sao_memprobe_candidate_t* out_candidates,
    uint32_t cap,
    uint32_t* out_count);

// 上轮 candidates 中按 predicate 筛。out_candidates 的 last_value 会更新为当前值。
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_scan_predicate(
    sao_core_process_handle_t process,
    sao_memprobe_dtype_t dtype,
    sao_memprobe_predicate_t predicate,
    const sao_memprobe_candidate_t* prev,
    uint32_t prev_count,
    sao_memprobe_candidate_t* out_candidates,
    uint32_t cap,
    uint32_t* out_count);

// dtype size (bytes)。
SAO_CORE_API uint32_t SAO_CORE_CALL sao_memprobe_dtype_size(sao_memprobe_dtype_t dtype);

// AVX2 available flag (runtime detected via CPUID)。
SAO_CORE_API int SAO_CORE_CALL sao_memprobe_has_avx2(void);

// AOB pattern scan with mask. `mask` is `pattern_len` bytes; 0xFF = must match,
// 0x00 = wildcard. Returns up to `cap` hit addresses via `out_addrs`.
// 对齐 python find_pattern_masked。
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_scan_pattern(
    sao_core_process_handle_t process,
    const uint8_t* pattern,
    const uint8_t* mask,
    uint32_t pattern_len,
    const sao_memprobe_scan_bounds_t* bounds,
    uint64_t* out_addrs,
    uint32_t cap,
    uint32_t* out_count);

#ifdef __cplusplus
} // extern "C"
#endif
