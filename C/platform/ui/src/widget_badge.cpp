// SAO Auto — animated badge widget implementation.
//
// Small badge widget (count / status dot) that pulses via the shared
// overlay tick.  Registered in the data widget family (tag 170) so it
// flows through the existing typed paint/apply_props/destroy dispatch.
// Pulse drives a 1.0 -> 1.18 -> 1.0 scale envelope over pulse_ms with
// SAO_UI_CURVE_EASE_IN_OUT, applied to the dot radius.

#include "sao/ui/widget_badge.h"
#include "sao/ui/widget_kit.h"

#include "panel_theme_internal.h"
#include "widget_paint_internal.h"
#include "widget_typed_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr int32_t kAnimatedBadgeTag = 170;  // SAO_UI_WIDGET_ANIMATED_BADGE
constexpr int32_t kDefaultDotRadius = 6;
constexpr int32_t kDefaultPulseMs = 900;
constexpr int32_t kDefaultFontSize = 12;
constexpr int32_t kDefaultPadX = 6;
constexpr int32_t kDefaultPadY = 3;
constexpr int32_t kDefaultRadius = 8;
constexpr float kPulsePeak = 1.18F;

uint32_t resolve_or(uint32_t override_value, SaoUiColorToken token) {
    return override_value != 0 ? override_value
                               : sao::ui::detail::panel_theme_color(token);
}

uint32_t lighten(uint32_t color, float amount) {
    const uint32_t a = (color >> 24U) & 0xffU;
    const uint32_t r = (color >> 16U) & 0xffU;
    const uint32_t g = (color >> 8U) & 0xffU;
    const uint32_t b = color & 0xffU;
    const float scale = std::clamp(amount, 0.0F, 1.0F);
    const uint32_t nr = static_cast<uint32_t>(std::lround(r + (255.0F - r) * scale));
    const uint32_t ng = static_cast<uint32_t>(std::lround(g + (255.0F - g) * scale));
    const uint32_t nb = static_cast<uint32_t>(std::lround(b + (255.0F - b) * scale));
    return (a << 24U) | (nr << 16U) | (ng << 8U) | nb;
}

float ease_in_out(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    return 3.0F * t * t - 2.0F * t * t * t;
}

struct AnimatedBadgeState {
    int32_t tag{kAnimatedBadgeTag};
    SaoUiAnimatedBadgeSpec spec{};
    int32_t pulse_elapsed_ms{0};
    bool pulsing{false};
    mutable std::mutex mtx;
};

struct BadgePropsSnapshot {
    int32_t count{};
    int32_t pulse_ms{};
    bool pulsing{};
};

}  // namespace

template <typename State>
std::shared_ptr<State> as_data(sao_ui_widget_handle_t handle, int32_t kind) {
    return std::static_pointer_cast<State>(sao::ui::detail::acquire_widget_handle(
        handle, sao::ui::detail::WidgetHandleFamily::data, kind));
}

std::shared_ptr<AnimatedBadgeState> as_badge(sao_ui_widget_handle_t handle) {
    return as_data<AnimatedBadgeState>(handle, kAnimatedBadgeTag);
}

sao_status_t publish_badge_state(std::shared_ptr<AnimatedBadgeState> state,
                                 sao_ui_widget_handle_t* out_handle) {
    void* const handle = sao::ui::detail::register_widget_handle(
        sao::ui::detail::WidgetHandleFamily::data, kAnimatedBadgeTag, std::move(state));
    if (handle == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(handle);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_create(
    void* /*d3d_device_ptr*/, const SaoUiAnimatedBadgeSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto state = std::make_shared<AnimatedBadgeState>();
        state->spec = *spec;
        state->spec.dot_radius_px = spec->dot_radius_px > 0 ? spec->dot_radius_px : kDefaultDotRadius;
        state->spec.pulse_ms = spec->pulse_ms;
        state->spec.font_size_px = spec->font_size_px > 0 ? spec->font_size_px : kDefaultFontSize;
        state->spec.pad_x_px = spec->pad_x_px > 0 ? spec->pad_x_px : kDefaultPadX;
        state->spec.pad_y_px = spec->pad_y_px > 0 ? spec->pad_y_px : kDefaultPadY;
        state->spec.radius_px = spec->radius_px > 0 ? spec->radius_px : kDefaultRadius;
        state->pulsing = spec->pulse_ms != 0;
        return publish_badge_state(std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_set_count(
    sao_ui_widget_handle_t handle, int32_t count) {
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.count = count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_get_count(
    sao_ui_widget_handle_t handle, int32_t* out_count) {
    if (out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_count = state->spec.count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_set_pulse(
    sao_ui_widget_handle_t handle, int32_t pulse_ms) {
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.pulse_ms = pulse_ms;
    state->pulsing = pulse_ms != 0;
    state->pulse_elapsed_ms = 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_is_pulsing(
    sao_ui_widget_handle_t handle, bool* out_pulsing) {
    if (out_pulsing == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_pulsing = state->pulsing && state->spec.pulse_ms > 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_apply_props(
    sao_ui_widget_handle_t handle, const uint8_t* props_json_utf8, size_t props_len) {
    if (handle == nullptr || (props_json_utf8 == nullptr && props_len != 0))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        nlohmann::json props = nlohmann::json::parse(
            props_json_utf8, props_json_utf8 + props_len, nullptr, false);
        if (!props.is_object())
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (!sao::ui::detail::widget_props_has_only(props,
                {"count", "dot_radius", "pulse_ms", "font_size", "pad_x", "pad_y",
                 "radius", "fill", "fg", "border", "pulse", "theme"}))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto state = as_badge(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(state->mtx);
        const auto count_it = props.find("count");
        if (count_it != props.end()) {
            int32_t count = 0;
            if (!sao::ui::detail::widget_props_i32(*count_it, &count))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.count = count;
        }
        const auto dot_it = props.find("dot_radius");
        if (dot_it != props.end()) {
            int32_t dot = 0;
            if (!sao::ui::detail::widget_props_i32(*dot_it, &dot) || dot < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.dot_radius_px = dot > 0 ? dot : kDefaultDotRadius;
        }
        const auto pulse_it = props.find("pulse_ms");
        if (pulse_it != props.end()) {
            int32_t pulse = 0;
            if (!sao::ui::detail::widget_props_i32(*pulse_it, &pulse) || pulse < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.pulse_ms = pulse;
            state->pulsing = pulse != 0;
            state->pulse_elapsed_ms = 0;
        }
        const auto font_it = props.find("font_size");
        if (font_it != props.end()) {
            int32_t font = 0;
            if (!sao::ui::detail::widget_props_i32(*font_it, &font) || font < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.font_size_px = font > 0 ? font : kDefaultFontSize;
        }
        const auto padx_it = props.find("pad_x");
        if (padx_it != props.end()) {
            int32_t pad = 0;
            if (!sao::ui::detail::widget_props_i32(*padx_it, &pad) || pad < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.pad_x_px = pad > 0 ? pad : kDefaultPadX;
        }
        const auto pady_it = props.find("pad_y");
        if (pady_it != props.end()) {
            int32_t pad = 0;
            if (!sao::ui::detail::widget_props_i32(*pady_it, &pad) || pad < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.pad_y_px = pad > 0 ? pad : kDefaultPadY;
        }
        const auto radius_it = props.find("radius");
        if (radius_it != props.end()) {
            int32_t r = 0;
            if (!sao::ui::detail::widget_props_i32(*radius_it, &r) || r < 0)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.radius_px = r > 0 ? r : kDefaultRadius;
        }
        const auto fill_it = props.find("fill");
        if (fill_it != props.end()) {
            uint32_t fill = 0;
            if (!sao::ui::detail::widget_props_argb(*fill_it, &fill))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.fill_argb = fill;
        }
        const auto fg_it = props.find("fg");
        if (fg_it != props.end()) {
            uint32_t fg = 0;
            if (!sao::ui::detail::widget_props_argb(*fg_it, &fg))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.fg_argb = fg;
        }
        const auto border_it = props.find("border");
        if (border_it != props.end()) {
            uint32_t border = 0;
            if (!sao::ui::detail::widget_props_argb(*border_it, &border))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.border_argb = border;
        }
        const auto pulse_color_it = props.find("pulse");
        if (pulse_color_it != props.end()) {
            uint32_t pulse = 0;
            if (!sao::ui::detail::widget_props_argb(*pulse_color_it, &pulse))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.pulse_argb = pulse;
        }
        const auto theme_it = props.find("theme");
        if (theme_it != props.end()) {
            int32_t theme = 0;
            if (!sao::ui::detail::widget_props_i32(*theme_it, &theme) ||
                theme < 0 || theme >= SAO_UI_THEME_COUNT)
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            state->spec.theme_override = static_cast<SaoUiThemeId>(theme);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_tick(
    sao_ui_widget_handle_t handle, int32_t dt_ms) {
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dt_ms < 0)
        dt_ms = 0;
    std::lock_guard<std::mutex> lock(state->mtx);
    if (state->pulsing && state->spec.pulse_ms > 0) {
        state->pulse_elapsed_ms = (state->pulse_elapsed_ms + dt_ms) % state->spec.pulse_ms;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_animated_badge_pulse_scale(
    sao_ui_widget_handle_t handle, float* out_scale) {
    if (out_scale == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    if (!state->pulsing || state->spec.pulse_ms <= 0) {
        *out_scale = 1.0F;
        return SAO_STATUS_OK;
    }
    const float phase = static_cast<float>(state->pulse_elapsed_ms) /
                        static_cast<float>(state->spec.pulse_ms);
    // 0.0 -> peak at 0.5 -> 1.0 at 1.0 (triangle, eased).
    const float tri = phase < 0.5F ? phase * 2.0F : (1.0F - phase) * 2.0F;
    const float eased = ease_in_out(tri);
    *out_scale = 1.0F + (kPulsePeak - 1.0F) * eased;
    return SAO_STATUS_OK;
}

// ── Typed-family hooks (called from widget_data.cpp dispatch) ─────
namespace sao::ui::detail {

sao_status_t widget_animated_badge_apply_props(sao_ui_widget_handle_t handle,
                                               const WidgetPropsJson& props,
                                               WidgetPropsSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    try {
        auto snapshot = std::make_shared<BadgePropsSnapshot>();
        auto state = as_badge(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            snapshot->count = state->spec.count;
            snapshot->pulse_ms = state->spec.pulse_ms;
            snapshot->pulsing = state->pulsing;
        }
        // Delegate field parsing to the public apply_props (reuses the
        // validated parser).  We pass the serialized props back through.
        // For the transactional snapshot we only need pre-state; the
        // public apply_props already mutates state.
        // Serialize props back to bytes for the public entry.
        const std::string bytes = props.dump();
        const sao_status_t status = sao_ui_animated_badge_apply_props(
            handle, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (status != SAO_STATUS_OK)
            return status;
        *out_snapshot = std::move(snapshot);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t widget_animated_badge_restore_props(sao_ui_widget_handle_t handle,
                                                 const WidgetPropsSnapshot& snapshot) noexcept {
    const auto previous = std::static_pointer_cast<BadgePropsSnapshot>(snapshot);
    if (previous == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto state = as_badge(handle);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.count = previous->count;
    state->spec.pulse_ms = previous->pulse_ms;
    state->pulsing = previous->pulsing;
    state->pulse_elapsed_ms = 0;
    return SAO_STATUS_OK;
}

sao_status_t widget_animated_badge_paint(sao_ui_widget_handle_t handle,
                                         sao_ui_paint_ctx_handle_t context,
                                         int32_t x, int32_t y,
                                         int32_t width, int32_t height) noexcept {
    try {
        auto state = as_badge(handle);
        if (state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        SaoUiAnimatedBadgeSpec spec{};
        float scale = 1.0F;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            spec = state->spec;
        }
        if (spec.pulse_ms > 0) {
            float s = 1.0F;
            (void)sao_ui_animated_badge_pulse_scale(handle, &s);
            scale = s;
        }
        const uint32_t fill = resolve_or(spec.fill_argb, SAO_UI_TOKEN_APP_ACCENT);
        const uint32_t fg = resolve_or(spec.fg_argb, SAO_UI_TOKEN_WHITE);
        const uint32_t border = resolve_or(spec.border_argb, SAO_UI_TOKEN_APP_BORDER);
        const uint32_t pulse_color = spec.pulse_argb != 0 ? spec.pulse_argb : lighten(fill, 0.30F);
        const float dot_r = static_cast<float>(spec.dot_radius_px) * scale;
        const float cx = static_cast<float>(x) + std::max(2, spec.pad_x_px) + dot_r;
        const float cy = static_cast<float>(y) + static_cast<float>(height) * 0.5F;

        // Pulse halo (ring around the dot).
        if (scale > 1.0F) {
            const float halo_r = dot_r + 2.0F;
            const sao_status_t halo = sao_ui_paint_ctx_fill_ellipse(
                context, cx - halo_r, cy - halo_r, halo_r * 2.0F, halo_r * 2.0F, pulse_color);
            if (halo != SAO_STATUS_OK)
                return halo;
        }
        // Dot.
        const sao_status_t dot_status = sao_ui_paint_ctx_fill_ellipse(
            context, cx - dot_r, cy - dot_r, dot_r * 2.0F, dot_r * 2.0F, fill);
        if (dot_status != SAO_STATUS_OK)
            return dot_status;
        (void)border;  // border used by text pill below when count >= 0.

        // Count text (if count >= 0) — pill to the right of the dot.
        if (spec.count >= 0) {
            const std::string text = std::to_string(spec.count);
            const float text_x = cx + dot_r + static_cast<float>(spec.pad_x_px);
            const float text_y = cy - static_cast<float>(spec.font_size_px) * 0.5F;
            return sao_ui_paint_ctx_draw_utf8(
                context, text_x, text_y, text.c_str(),
                static_cast<float>(spec.font_size_px), fg);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

}  // namespace sao::ui::detail