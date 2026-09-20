#pragma once

#include "sao/ui/compositor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace sao::ui::detail {

struct MenuSceneFrame {
    sao_ui_compositor_handle_t compositor{};
    const void* owner{};
    float menu_x{}, menu_y{}, menu_width{}, menu_height{};
    float viewport_width{}, viewport_height{};
    float theme_progress{}, theme_direction{}, seconds{};
    float menu_visibility{}, child_activity{};
    bool reduced_motion{}, fps_pressure{}, high_contrast{};
};

struct ThemePaintFrame {
    float origin_x{}, origin_y{}, viewport_width{}, viewport_height{}, seconds{};
    float theme_direction{};
    bool reduced_motion{}, fps_pressure{}, high_contrast{};
};

static_assert(std::is_trivially_copyable_v<MenuSceneFrame>);
static_assert(std::is_trivially_copyable_v<ThemePaintFrame>);

// Publication and lookup stay on the compositor owner thread; identities are never dereferenced.
inline thread_local MenuSceneFrame published_menu_scene{};

inline void publish_menu_scene(const MenuSceneFrame& frame) noexcept {
    published_menu_scene = frame;
}

inline void clear_menu_scene(sao_ui_compositor_handle_t compositor, const void* owner) noexcept {
    if (published_menu_scene.compositor == compositor && published_menu_scene.owner == owner)
        published_menu_scene = {};
}

inline bool read_menu_scene(sao_ui_compositor_handle_t compositor, MenuSceneFrame& out) noexcept {
    out = {};
    const auto& frame = published_menu_scene;
    if (!compositor || frame.compositor != compositor || !frame.owner ||
        !std::isfinite(frame.menu_x) || !std::isfinite(frame.menu_y) ||
        !std::isfinite(frame.menu_width) || !std::isfinite(frame.menu_height) ||
        !std::isfinite(frame.viewport_width) || !std::isfinite(frame.viewport_height) ||
        frame.viewport_width <= 0.0F || frame.viewport_height <= 0.0F ||
        frame.menu_width < 0.0F || frame.menu_height < 0.0F ||
        !std::isfinite(frame.theme_progress) || !std::isfinite(frame.theme_direction) ||
        !std::isfinite(frame.seconds) || frame.seconds < 0.0F ||
        !std::isfinite(frame.menu_visibility) || !std::isfinite(frame.child_activity))
        return false;
    out = frame;
    return true;
}

inline bool valid_theme_paint_frame(const ThemePaintFrame& frame) noexcept {
    return std::isfinite(frame.origin_x) && std::isfinite(frame.origin_y) &&
        std::isfinite(frame.viewport_width) && std::isfinite(frame.viewport_height) &&
        frame.viewport_width > 0.0F && frame.viewport_height > 0.0F &&
        std::isfinite(frame.seconds) && frame.seconds >= 0.0F &&
        std::isfinite(frame.theme_direction);
}

inline float menu_theme_hash(uint32_t column, uint32_t salt) noexcept {
    uint32_t value = column * 0x9e3779b9u ^ salt * 0x85ebca6bu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    return static_cast<float>(value & 0xffffu) / 65535.0F;
}

// Keep this normalized column front identical to themeFront in fisheye_backdrop_ps.hlsl.
inline float menu_theme_front(uint32_t column, float progress, float seconds) noexcept {
    const float start = menu_theme_hash(column, 13u) * 0.24F;
    const float finish = 0.68F + menu_theme_hash(column, 37u) * 0.32F;
    const float local = std::clamp((progress - start) / (finish - start), 0.0F, 1.0F);
    const float travel = std::pow(local, 1.10F + menu_theme_hash(column, 71u) * 1.20F);
    const float drift = std::sin(seconds * 0.65F + menu_theme_hash(column, 63u) * 6.2831853F);
    return -0.14F + 1.28F * travel + drift * 0.0225F * (4.0F * local * (1.0F - local));
}

} // namespace sao::ui::detail
