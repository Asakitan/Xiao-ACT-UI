#include "sao_core/pixels.h"

#include "logging.h"

#include <cstring>
#include <limits>

namespace {

constexpr char kComponent[] = "core.pixels";

int32_t fail(int32_t status, const char* message) noexcept {
    sao::legacy_core::emit_log(sao::legacy_core::kLogLevelError, kComponent, status, message);
    return status;
}

bool checked_multiply(size_t left, size_t right, size_t* out) noexcept {
    if (left != 0 && right > std::numeric_limits<size_t>::max() / left) {
        return false;
    }
    *out = left * right;
    return true;
}

bool validate_layout(uint32_t width, uint32_t height, uint32_t stride_bytes) noexcept {
    size_t row_bytes = 0;
    return width != 0 && height != 0 &&
           checked_multiply(static_cast<size_t>(width), 4, &row_bytes) && row_bytes <= stride_bytes;
}

uint8_t blend_channel(uint8_t source, uint8_t destination, uint32_t alpha) noexcept {
    const uint32_t inverse_alpha = 255u - alpha;
    return static_cast<uint8_t>((static_cast<uint32_t>(source) * alpha +
                                 static_cast<uint32_t>(destination) * inverse_alpha + 127u) /
                                255u);
}

int32_t validate_region_impl(size_t buffer_len, uint32_t width, uint32_t height,
                             uint32_t stride_bytes, uint32_t x, uint32_t y, uint32_t region_width,
                             uint32_t region_height, size_t* out_first_byte_offset,
                             size_t* out_required_end_offset) noexcept {
    if (out_first_byte_offset != nullptr) {
        *out_first_byte_offset = 0;
    }
    if (out_required_end_offset != nullptr) {
        *out_required_end_offset = 0;
    }
    if (out_first_byte_offset == nullptr || out_required_end_offset == nullptr ||
        !validate_layout(width, height, stride_bytes) || region_width == 0 || region_height == 0 ||
        x >= width || y >= height || region_width > width - x || region_height > height - y) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    size_t first_row_offset = 0;
    size_t last_row_offset = 0;
    size_t first_pixel_offset = 0;
    size_t region_end_in_row = 0;
    if (!checked_multiply(static_cast<size_t>(y), stride_bytes, &first_row_offset) ||
        !checked_multiply(static_cast<size_t>(y + region_height - 1), stride_bytes,
                          &last_row_offset) ||
        !checked_multiply(static_cast<size_t>(x), 4, &first_pixel_offset) ||
        !checked_multiply(static_cast<size_t>(x + region_width), 4, &region_end_in_row) ||
        first_row_offset > std::numeric_limits<size_t>::max() - first_pixel_offset ||
        last_row_offset > std::numeric_limits<size_t>::max() - region_end_in_row) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    const size_t first_offset = first_row_offset + first_pixel_offset;
    const size_t required_end = last_row_offset + region_end_in_row;
    if (required_end > buffer_len) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    *out_first_byte_offset = first_offset;
    *out_required_end_offset = required_end;
    return SAO_OK;
}

} // namespace

extern "C" int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_pixels_premultiply_blend(const uint8_t* src_bgra, uint8_t* dst_bgra, uint32_t width,
                                         uint32_t height, uint32_t stride_bytes) {
    if (src_bgra == nullptr || dst_bgra == nullptr ||
        !validate_layout(width, height, stride_bytes)) {
        return fail(SAO_ERR_INVALID_ARGUMENT,
                    "pixels_premultiply_blend received invalid arguments");
    }

    for (uint32_t y = 0; y < height; ++y) {
        const size_t row_offset = static_cast<size_t>(y) * stride_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel_offset = row_offset + static_cast<size_t>(x) * 4;
            const uint32_t alpha = src_bgra[pixel_offset + 3];
            dst_bgra[pixel_offset + 0] =
                blend_channel(src_bgra[pixel_offset + 0], dst_bgra[pixel_offset + 0], alpha);
            dst_bgra[pixel_offset + 1] =
                blend_channel(src_bgra[pixel_offset + 1], dst_bgra[pixel_offset + 1], alpha);
            dst_bgra[pixel_offset + 2] =
                blend_channel(src_bgra[pixel_offset + 2], dst_bgra[pixel_offset + 2], alpha);
            const uint32_t inverse_alpha = 255u - alpha;
            dst_bgra[pixel_offset + 3] = static_cast<uint8_t>(
                alpha +
                (static_cast<uint32_t>(dst_bgra[pixel_offset + 3]) * inverse_alpha + 127u) / 255u);
        }
    }
    return SAO_OK;
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_pixels_find_alpha_spans(
    const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t row_index,
    uint32_t* out_spans_x0x1, uint32_t max_spans, uint32_t* out_span_count) {
    if (out_span_count != nullptr) {
        *out_span_count = 0;
    }
    if (max_spans > std::numeric_limits<size_t>::max() / (2 * sizeof(*out_spans_x0x1))) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "pixels_find_alpha_spans output capacity overflows");
    }
    if (out_spans_x0x1 != nullptr && max_spans != 0) {
        std::memset(out_spans_x0x1, 0, static_cast<size_t>(max_spans) * 2 * sizeof(uint32_t));
    }
    if (out_span_count == nullptr || bgra == nullptr ||
        !validate_layout(width, height, stride_bytes) || row_index >= height ||
        (out_spans_x0x1 == nullptr && max_spans != 0)) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "pixels_find_alpha_spans received invalid arguments");
    }

    const size_t row_offset = static_cast<size_t>(row_index) * stride_bytes;
    uint32_t span_count = 0;
    bool in_span = false;
    for (uint32_t x = 0; x < width; ++x) {
        const bool opaque = bgra[row_offset + static_cast<size_t>(x) * 4 + 3] != 0;
        if (opaque && !in_span) {
            ++span_count;
            in_span = true;
        } else if (!opaque) {
            in_span = false;
        }
    }
    *out_span_count = span_count;
    if (out_spans_x0x1 == nullptr) {
        return SAO_OK;
    }
    if (max_spans < span_count) {
        return fail(SAO_ERR_BUFFER_TOO_SMALL, "pixels_find_alpha_spans output buffer is too small");
    }

    uint32_t write_index = 0;
    uint32_t span_start = 0;
    in_span = false;
    for (uint32_t x = 0; x <= width; ++x) {
        const bool opaque = x < width && bgra[row_offset + static_cast<size_t>(x) * 4 + 3] != 0;
        if (opaque && !in_span) {
            span_start = x;
            in_span = true;
        } else if (!opaque && in_span) {
            out_spans_x0x1[write_index * 2] = span_start;
            out_spans_x0x1[write_index * 2 + 1] = x;
            ++write_index;
            in_span = false;
        }
    }
    return SAO_OK;
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_pixels_validate_region(
    size_t buffer_len, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t x,
    uint32_t y, uint32_t region_width, uint32_t region_height, size_t* out_first_byte_offset,
    size_t* out_required_end_offset) {
    const int32_t status =
        validate_region_impl(buffer_len, width, height, stride_bytes, x, y, region_width,
                             region_height, out_first_byte_offset, out_required_end_offset);
    return status == SAO_OK ? SAO_OK : fail(status, "pixels_validate_region failed validation");
}

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_pixels_sample_bgra(
    const uint8_t* bgra, size_t buffer_len, uint32_t width, uint32_t height, uint32_t stride_bytes,
    uint32_t x, uint32_t y, SaoLegacyCoreBgraPixel* out_pixel) {
    if (out_pixel != nullptr) {
        *out_pixel = {};
    }
    if (out_pixel == nullptr || bgra == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "pixels_sample_bgra received invalid arguments");
    }

    size_t pixel_offset = 0;
    size_t required_end = 0;
    const int32_t status = validate_region_impl(buffer_len, width, height, stride_bytes, x, y, 1, 1,
                                                &pixel_offset, &required_end);
    if (status != SAO_OK) {
        return fail(status, "pixels_sample_bgra failed validation");
    }

    *out_pixel = SaoLegacyCoreBgraPixel{
        bgra[pixel_offset + 0],
        bgra[pixel_offset + 1],
        bgra[pixel_offset + 2],
        bgra[pixel_offset + 3],
    };
    return SAO_OK;
}
