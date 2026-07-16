#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/sao_status.h"

// mask byte 0x00 = wildcard, 0xFF = must match (standard AOB-scan-with-mask convention).
extern "C" SAO_CORE_API int32_t SAO_CORE_CALL sao_core_scan_find_pattern(
    const uint8_t* haystack,
    size_t haystack_len,
    const uint8_t* pattern,
    const uint8_t* mask,
    size_t pattern_len,
    size_t* out_offset);

extern "C" SAO_CORE_API int32_t SAO_CORE_CALL sao_core_scan_find_aligned_u64(
    const uint8_t* haystack,
    size_t haystack_len,
    const uint64_t* target_values,
    size_t target_count,
    size_t* out_offsets,
    size_t max_out_offsets,
    size_t* out_match_count);
