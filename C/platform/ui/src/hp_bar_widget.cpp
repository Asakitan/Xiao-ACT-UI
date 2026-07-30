#include "hp_bar_widget_internal.h"

#include "widget_raster_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace {

using namespace sao::ui::raster;

uint32_t blend_argb(uint32_t from, uint32_t to, float amount) {
    const float t = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [t](uint32_t left, uint32_t right) {
        return static_cast<uint32_t>(std::lround(
            static_cast<float>(left) + (static_cast<float>(right) - static_cast<float>(left)) * t));
    };
    return (channel((from >> 24u) & 0xffu, (to >> 24u) & 0xffu) << 24u) |
           (channel((from >> 16u) & 0xffu, (to >> 16u) & 0xffu) << 16u) |
           (channel((from >> 8u) & 0xffu, (to >> 8u) & 0xffu) << 8u) |
           channel(from & 0xffu, to & 0xffu);
}

bool inside_rounded_rect(float pixel_x, float pixel_y, Rect rect, float radius) {
    radius = std::clamp(radius, 0.0F, std::min(rect.width, rect.height) * 0.5F);
    if (radius <= 0.0F)
        return pixel_x >= rect.x && pixel_y >= rect.y && pixel_x < rect.x + rect.width &&
               pixel_y < rect.y + rect.height;
    const float nearest_x = std::clamp(pixel_x, rect.x + radius, rect.x + rect.width - radius);
    const float nearest_y = std::clamp(pixel_y, rect.y + radius, rect.y + rect.height - radius);
    const float dx = pixel_x - nearest_x;
    const float dy = pixel_y - nearest_y;
    return dx * dx + dy * dy <= radius * radius;
}

bool inside_skew_fill(float pixel_x, float pixel_y, Rect rect, float ratio, float skew) {
    const float clamped_ratio = std::clamp(ratio, 0.0F, 1.0F);
    if (clamped_ratio <= 0.0F)
        return false;
    const float fill_end = rect.x + rect.width * clamped_ratio;
    const float normalized_y =
        rect.height <= 0.0F ? 0.0F : std::clamp((pixel_y - rect.y) / rect.height, 0.0F, 1.0F);
    const float effective_skew = std::min(std::max(0.0F, skew), rect.width * clamped_ratio);
    const float front = fill_end - effective_skew * (1.0F - normalized_y);
    return pixel_x >= rect.x && pixel_x < front;
}

} // namespace

uint32_t sao::ui::detail::hp_bar_ramp_color(uint32_t low_argb, uint32_t mid_argb,
                                            uint32_t high_argb, float ratio) noexcept {
    const float clamped = std::clamp(ratio, 0.0F, 1.0F);
    if (clamped < 0.25F)
        return blend_argb(low_argb, mid_argb, clamped / 0.25F);
    if (clamped < 0.50F)
        return blend_argb(mid_argb, high_argb, (clamped - 0.25F) / 0.25F);
    return high_argb;
}

sao_status_t sao::ui::detail::paint_hp_bar(sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                                           int32_t width, int32_t height, float fill_ratio,
                                           float trail_ratio, uint32_t low_argb, uint32_t mid_argb,
                                           uint32_t high_argb, uint32_t trail_argb,
                                           int32_t radius_px, int32_t leading_skew_px) noexcept {
    if (context == nullptr || context->raster == nullptr || width <= 0 || height <= 0 ||
        !std::isfinite(fill_ratio) || !std::isfinite(trail_ratio) || radius_px < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->raster->mutex);
        const Rect bounds{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                          static_cast<float>(height)};
        const Rect draw = intersect(bounds, clip_bounds(*context));
        if (!valid_rect(draw.width, draw.height))
            return SAO_STATUS_OK;
        const float fill = std::clamp(fill_ratio, 0.0F, 1.0F);
        const float trail = std::clamp(std::max(fill, trail_ratio), 0.0F, 1.0F);
        const float radius = static_cast<float>(radius_px);
        const float skew = static_cast<float>(std::max(0, leading_skew_px));
        const BgraPixel trail_color =
            premultiply(apply_opacity(trail_argb, current_opacity(*context)));
        const int32_t left = static_cast<int32_t>(std::floor(draw.x));
        const int32_t top = static_cast<int32_t>(std::floor(draw.y));
        const int32_t right = static_cast<int32_t>(std::ceil(draw.x + draw.width));
        const int32_t bottom = static_cast<int32_t>(std::ceil(draw.y + draw.height));
        for (int32_t py = top; py < bottom; ++py) {
            for (int32_t px = left; px < right; ++px) {
                const float pixel_x = static_cast<float>(px) + 0.5F;
                const float pixel_y = static_cast<float>(py) + 0.5F;
                if (!inside_rounded_rect(pixel_x, pixel_y, bounds, radius))
                    continue;
                const bool in_fill = inside_skew_fill(pixel_x, pixel_y, bounds, fill, skew);
                const bool in_trail = inside_skew_fill(pixel_x, pixel_y, bounds, trail, skew);
                if (!in_fill && in_trail && trail_color.a != 0u) {
                    blend_pixel(*context->raster, px, py, trail_color);
                }
                if (!in_fill)
                    continue;
                const float gradient_ratio =
                    std::clamp((pixel_x - bounds.x) / bounds.width, 0.0F, 1.0F);
                const uint32_t color =
                    hp_bar_ramp_color(low_argb, mid_argb, high_argb, gradient_ratio);
                blend_pixel(*context->raster, px, py,
                            premultiply(apply_opacity(color, current_opacity(*context))));
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
