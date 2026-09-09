#pragma once

// Shared paint internals for the software raster, immutable recorder, and
// direct D2D backends.  The pixel helpers remain the explicit offscreen/export
// path; widget primitives dispatch through the backend table.  Not public ABI.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "sao/core/status.h"

namespace sao::ui::raster {

struct BgraPixel {
    uint8_t b{};
    uint8_t g{};
    uint8_t r{};
    uint8_t a{};
};

struct Rect {
    float x{};
    float y{};
    float width{};
    float height{};
};

struct GpuPaintDispatch {
    void (*destroy)(void*) noexcept {};
    sao_status_t (*begin_frame)(void*) noexcept {};
    sao_status_t (*end_frame)(void*) noexcept {};
    sao_status_t (*push_clip)(void*, Rect) noexcept {};
    sao_status_t (*pop_clip)(void*) noexcept {};
    bool (*fill_rect)(void*, Rect, uint32_t, float) noexcept {};
    bool (*fill_rounded_rect)(void*, Rect, float, uint32_t, float) noexcept {};
    bool (*stroke_rounded_rect)(void*, Rect, float, float, uint32_t, float) noexcept {};
    bool (*fill_ellipse)(void*, Rect, uint32_t, float) noexcept {};
    bool (*stroke_line)(void*, float, float, float, float, float, uint32_t, float) noexcept {};
    bool (*fill_polygon)(void*, const int32_t*, size_t, uint32_t, float) noexcept {};
    bool (*draw_utf8)(void*, float, float, const char*, float, uint32_t, float, uint8_t,
                      uint8_t) noexcept {};
    bool (*blit_bgra)(void*, const uint8_t*, uint32_t, uint32_t, uint32_t, Rect, float) noexcept {};
};

} // namespace sao::ui::raster

// The opaque public structs are forward-declared in the global namespace by
// the public header (sao/ui/d2d_widgets.h); define them in the global
// namespace so the public handle types resolve to a single definition.
struct sao_ui_offscreen_raster_s {
    uint32_t width{};
    uint32_t height{};
    std::vector<sao::ui::raster::BgraPixel> pixels;
    std::mutex mutex;
};

struct sao_ui_paint_ctx_s {
    sao_ui_offscreen_raster_s* raster{};
    void* gpu_state{};
    const sao::ui::raster::GpuPaintDispatch* gpu_dispatch{};
    uint32_t target_width{};
    uint32_t target_height{};
    sao_status_t backend_status{SAO_STATUS_OK};
    std::vector<sao::ui::raster::Rect> clips;
    std::vector<float> opacity_stack{1.0F};
    bool in_frame{};
};

namespace sao::ui::raster {

inline BgraPixel premultiply(uint32_t argb) {
    const uint32_t alpha = (argb >> 24U) & 0xffU;
    const uint32_t red = (argb >> 16U) & 0xffU;
    const uint32_t green = (argb >> 8U) & 0xffU;
    const uint32_t blue = argb & 0xffU;
    return {static_cast<uint8_t>((blue * alpha + 127U) / 255U),
            static_cast<uint8_t>((green * alpha + 127U) / 255U),
            static_cast<uint8_t>((red * alpha + 127U) / 255U), static_cast<uint8_t>(alpha)};
}

inline float current_opacity(const sao_ui_paint_ctx_s& context) {
    return context.opacity_stack.empty() ? 1.0F : context.opacity_stack.back();
}

inline uint32_t apply_opacity(uint32_t argb, float opacity) {
    const uint32_t alpha = (argb >> 24U) & 0xffU;
    const auto scaled_alpha = static_cast<uint32_t>(
        std::lround(static_cast<float>(alpha) * std::clamp(opacity, 0.0F, 1.0F)));
    return (argb & 0x00ffffffU) | (scaled_alpha << 24U);
}

inline BgraPixel apply_opacity(BgraPixel pixel, float opacity) {
    const float clamped = std::clamp(opacity, 0.0F, 1.0F);
    pixel.b = static_cast<uint8_t>(std::lround(pixel.b * clamped));
    pixel.g = static_cast<uint8_t>(std::lround(pixel.g * clamped));
    pixel.r = static_cast<uint8_t>(std::lround(pixel.r * clamped));
    pixel.a = static_cast<uint8_t>(std::lround(pixel.a * clamped));
    return pixel;
}

inline bool valid_rect(float width, float height) {
    return std::isfinite(width) && std::isfinite(height) && width > 0.0F && height > 0.0F;
}

inline bool valid_paint_rect(Rect rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && valid_rect(rect.width, rect.height) &&
           std::isfinite(rect.x + rect.width) && std::isfinite(rect.y + rect.height);
}

inline Rect intersect(Rect first, Rect second) {
    const float x = std::max(first.x, second.x);
    const float y = std::max(first.y, second.y);
    const float right = std::min(first.x + first.width, second.x + second.width);
    const float bottom = std::min(first.y + first.height, second.y + second.height);
    return {x, y, std::max(0.0F, right - x), std::max(0.0F, bottom - y)};
}

inline Rect clip_bounds(const sao_ui_paint_ctx_s& context) {
    if (context.raster == nullptr && context.gpu_state == nullptr)
        return {};
    const uint32_t width = context.raster != nullptr ? context.raster->width : context.target_width;
    const uint32_t height =
        context.raster != nullptr ? context.raster->height : context.target_height;
    Rect result{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
    for (const Rect& clip : context.clips)
        result = intersect(result, clip);
    return result;
}

inline void blend_pixel(sao_ui_offscreen_raster_s& raster, int32_t x, int32_t y, BgraPixel source) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= raster.width ||
        static_cast<uint32_t>(y) >= raster.height)
        return;
    BgraPixel& destination =
        raster.pixels[static_cast<size_t>(y) * raster.width + static_cast<uint32_t>(x)];
    const uint32_t inverse_alpha = 255U - source.a;
    destination.b = static_cast<uint8_t>(
        source.b + (static_cast<uint32_t>(destination.b) * inverse_alpha + 127U) / 255U);
    destination.g = static_cast<uint8_t>(
        source.g + (static_cast<uint32_t>(destination.g) * inverse_alpha + 127U) / 255U);
    destination.r = static_cast<uint8_t>(
        source.r + (static_cast<uint32_t>(destination.r) * inverse_alpha + 127U) / 255U);
    destination.a = static_cast<uint8_t>(
        source.a + (static_cast<uint32_t>(destination.a) * inverse_alpha + 127U) / 255U);
}

inline void fill_rect(sao_ui_paint_ctx_s& context, Rect rect, uint32_t argb) {
    // Internal widget painters historically treated empty/invalid fills as a
    // no-op on the software raster. Apply that rule before dispatch too, so a
    // recorder never serializes a zero-width command that replay's public API
    // correctly rejects (for example a progress bar at value 0).
    if (!valid_paint_rect(rect))
        return;
    if (context.gpu_state != nullptr && context.gpu_dispatch != nullptr &&
        context.gpu_dispatch->fill_rect != nullptr) {
        if (!context.gpu_dispatch->fill_rect(context.gpu_state, rect, argb,
                                             current_opacity(context)) &&
            context.backend_status == SAO_STATUS_OK)
            context.backend_status = SAO_STATUS_ERR_UNKNOWN;
        return;
    }
    if (context.raster == nullptr)
        return;
    rect = intersect(rect, clip_bounds(context));
    if (!valid_rect(rect.width, rect.height))
        return;
    const BgraPixel color = premultiply(apply_opacity(argb, current_opacity(context)));
    const int32_t left = static_cast<int32_t>(std::floor(rect.x));
    const int32_t top = static_cast<int32_t>(std::floor(rect.y));
    const int32_t right = static_cast<int32_t>(std::ceil(rect.x + rect.width));
    const int32_t bottom = static_cast<int32_t>(std::ceil(rect.y + rect.height));
    for (int32_t y = top; y < bottom; ++y) {
        for (int32_t x = left; x < right; ++x)
            blend_pixel(*context.raster, x, y, color);
    }
}

} // namespace sao::ui::raster
