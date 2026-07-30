#pragma once

// Shared software-raster internals between d2d_widgets.cpp and the
// DirectWrite text backend (widget_text_render_win.cpp).  Not part of the
// public ABI.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
            static_cast<uint8_t>((red * alpha + 127U) / 255U),
            static_cast<uint8_t>(alpha)};
}

inline float current_opacity(const sao_ui_paint_ctx_s& context) {
    return context.opacity_stack.empty() ? 1.0F
                                         : context.opacity_stack.back();
}

inline uint32_t apply_opacity(uint32_t argb, float opacity) {
    const uint32_t alpha = (argb >> 24U) & 0xffU;
    const auto scaled_alpha = static_cast<uint32_t>(std::lround(
        static_cast<float>(alpha) * std::clamp(opacity, 0.0F, 1.0F)));
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

inline Rect intersect(Rect first, Rect second) {
    const float x = std::max(first.x, second.x);
    const float y = std::max(first.y, second.y);
    const float right = std::min(first.x + first.width, second.x + second.width);
    const float bottom = std::min(first.y + first.height, second.y + second.height);
    return {x, y, std::max(0.0F, right - x), std::max(0.0F, bottom - y)};
}

inline Rect clip_bounds(const sao_ui_paint_ctx_s& context) {
    if (context.raster == nullptr) return {};
    Rect result{0.0F, 0.0F, static_cast<float>(context.raster->width), static_cast<float>(context.raster->height)};
    for (const Rect& clip : context.clips) result = intersect(result, clip);
    return result;
}

inline void blend_pixel(sao_ui_offscreen_raster_s& raster, int32_t x, int32_t y, BgraPixel source) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= raster.width || static_cast<uint32_t>(y) >= raster.height) return;
    BgraPixel& destination = raster.pixels[static_cast<size_t>(y) * raster.width + static_cast<uint32_t>(x)];
    const uint32_t inverse_alpha = 255U - source.a;
    destination.b = static_cast<uint8_t>(source.b + (static_cast<uint32_t>(destination.b) * inverse_alpha + 127U) / 255U);
    destination.g = static_cast<uint8_t>(source.g + (static_cast<uint32_t>(destination.g) * inverse_alpha + 127U) / 255U);
    destination.r = static_cast<uint8_t>(source.r + (static_cast<uint32_t>(destination.r) * inverse_alpha + 127U) / 255U);
    destination.a = static_cast<uint8_t>(source.a + (static_cast<uint32_t>(destination.a) * inverse_alpha + 127U) / 255U);
}

inline void fill_rect(sao_ui_paint_ctx_s& context, Rect rect, uint32_t argb) {
    if (context.raster == nullptr || !valid_rect(rect.width, rect.height)) return;
    rect = intersect(rect, clip_bounds(context));
    if (!valid_rect(rect.width, rect.height)) return;
    const BgraPixel color = premultiply(
        apply_opacity(argb, current_opacity(context)));
    const int32_t left = static_cast<int32_t>(std::floor(rect.x));
    const int32_t top = static_cast<int32_t>(std::floor(rect.y));
    const int32_t right = static_cast<int32_t>(std::ceil(rect.x + rect.width));
    const int32_t bottom = static_cast<int32_t>(std::ceil(rect.y + rect.height));
    for (int32_t y = top; y < bottom; ++y) {
        for (int32_t x = left; x < right; ++x) blend_pixel(*context.raster, x, y, color);
    }
}

} // namespace sao::ui::raster
