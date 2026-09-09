#include "hp_bar_widget_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

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

// Return the visible vertical interval at one x sample.  This is the analytic
// intersection of the bar's rounded outer silhouette and its slanted leading
// edge.  Painting a bounded set of adjacent strips keeps the HP ramp in the
// shared primitive stream: recording contexts store commands and GPU contexts
// issue D2D draws directly, with no widget-sized CPU bitmap or raster lock.
bool visible_vertical_span(float sample_x, float left, float top, float width, float height,
                           float ratio, float radius, float skew, float* out_top,
                           float* out_bottom) noexcept {
    if (out_top == nullptr || out_bottom == nullptr || ratio <= 0.0F)
        return false;

    const float right = left + width;
    const float bottom = top + height;
    const float fill_end = left + width * ratio;
    if (sample_x < left || sample_x >= fill_end)
        return false;

    float visible_top = top;
    float visible_bottom = bottom;
    if (radius > 0.0F && (sample_x < left + radius || sample_x > right - radius)) {
        const float center_x = sample_x < left + radius ? left + radius : right - radius;
        const float dx = std::clamp(sample_x - center_x, -radius, radius);
        const float dy = std::sqrt(std::max(0.0F, radius * radius - dx * dx));
        visible_top = std::max(visible_top, top + radius - dy);
        visible_bottom = std::min(visible_bottom, bottom - radius + dy);
    }

    const float effective_skew = std::min(std::max(0.0F, skew), width * ratio);
    if (effective_skew > 0.0F) {
        // sample_x < fill_end - skew * (1 - normalized_y)
        const float normalized_threshold =
            (sample_x - (fill_end - effective_skew)) / effective_skew;
        visible_top = std::max(visible_top, top + height * normalized_threshold);
    }
    *out_top = std::clamp(visible_top, top, bottom);
    *out_bottom = std::clamp(visible_bottom, top, bottom);
    return *out_bottom > *out_top;
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
    if (context == nullptr || width <= 0 || height <= 0 || !std::isfinite(fill_ratio) ||
        !std::isfinite(trail_ratio) || radius_px < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        const float fill = std::clamp(fill_ratio, 0.0F, 1.0F);
        const float trail = std::clamp(std::max(fill, trail_ratio), 0.0F, 1.0F);
        const float left = static_cast<float>(x);
        const float top = static_cast<float>(y);
        const float bar_width = static_cast<float>(width);
        const float bar_height = static_cast<float>(height);
        const float radius =
            std::clamp(static_cast<float>(radius_px), 0.0F, std::min(bar_width, bar_height) * 0.5F);
        const float skew = static_cast<float>(std::max(0, leading_skew_px));

        // One strip per pixel for ordinary controls, capped for abnormally wide
        // bars so malformed/SDK content cannot explode the immutable list.
        constexpr int32_t kMaxStrips = 512;
        const int32_t strip_count = std::max(1, std::min(width, kMaxStrips));
        const float strip_width = bar_width / static_cast<float>(strip_count);
        const auto paint_ratio = [&](float ratio, bool ramp, uint32_t solid) -> sao_status_t {
            if (ratio <= 0.0F || (!ramp && (solid & 0xff000000U) == 0U))
                return SAO_STATUS_OK;
            for (int32_t strip = 0; strip < strip_count; ++strip) {
                const float strip_left = left + static_cast<float>(strip) * strip_width;
                const float strip_right = left + static_cast<float>(strip + 1) * strip_width;
                const float sample_x = (strip_left + strip_right) * 0.5F;
                float visible_top = 0.0F;
                float visible_bottom = 0.0F;
                if (!visible_vertical_span(sample_x, left, top, bar_width, bar_height, ratio,
                                           radius, skew, &visible_top, &visible_bottom)) {
                    continue;
                }
                const uint32_t color =
                    ramp ? hp_bar_ramp_color(low_argb, mid_argb, high_argb,
                                             std::clamp((sample_x - left) / bar_width, 0.0F, 1.0F))
                         : solid;
                const sao_status_t status = sao_ui_paint_ctx_fill_rect(
                    context, strip_left, visible_top, strip_right - strip_left,
                    visible_bottom - visible_top, color);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            return SAO_STATUS_OK;
        };

        sao_status_t status = paint_ratio(trail, false, trail_argb);
        if (status == SAO_STATUS_OK)
            status = paint_ratio(fill, true, 0U);
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
