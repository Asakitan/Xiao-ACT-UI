#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/sao_status.h"

struct SaoLegacyCoreBgraPixel {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
    uint8_t alpha;
};

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_pixels_premultiply_blend(const uint8_t* src_bgra, uint8_t* dst_bgra, uint32_t width,
                                         uint32_t height, uint32_t stride_bytes);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_pixels_find_alpha_spans(
    const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t row_index,
    uint32_t* out_spans_x0x1, uint32_t max_spans, uint32_t* out_span_count);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_pixels_validate_region(
    size_t buffer_len, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t x,
    uint32_t y, uint32_t region_width, uint32_t region_height, size_t* out_first_byte_offset,
    size_t* out_required_end_offset);

extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_pixels_sample_bgra(
    const uint8_t* bgra, size_t buffer_len, uint32_t width, uint32_t height, uint32_t stride_bytes,
    uint32_t x, uint32_t y, SaoLegacyCoreBgraPixel* out_pixel);
