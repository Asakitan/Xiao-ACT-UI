#pragma once

#include "sao/ui/menu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace sao::ui::menu_visual {

inline constexpr size_t kNameCapacity = 128;
inline constexpr size_t kIconCapacity = 32;

struct ChildRowSnapshot {
    std::array<char, kNameCapacity> name_utf8{};
    std::array<char, kIconCapacity> icon_utf8{};
    int32_t action_id{-1};
    bool can_activate{};
    SaoUiMenuBtnState state{SAO_UI_MENU_BTN_IDLE};
    int32_t visible_width_px{};
    float hover_t{};
    float stagger_t{};
    float radial_t{};
    float selection_trail_t{};
    float pressed_pulse_t{};
};

struct RootRowSnapshot {
    bool can_activate{};
    SaoUiMenuBtnState state{SAO_UI_MENU_BTN_IDLE};
    float hover_t{};
    float fisheye_t{};
    float stagger_t{};
    float radial_t{};
    float selection_trail_t{};
    float pressed_pulse_t{};
};

struct ChildMenuRowSnapshot {
    std::string name_utf8;
    std::string icon_utf8;
    int32_t action_id{-1};
    bool can_activate{};
    SaoUiMenuBtnState state{SAO_UI_MENU_BTN_IDLE};
    int32_t visible_width_px{};
    float hover_t{};
};

struct ChildMenuSnapshot {
    bool exists{};
    SaoUiMenuPhase phase{SAO_UI_MENU_PHASE_CLOSED};
    int32_t phase_elapsed_ms{};
    std::string displayed_parent_name;
    std::string pending_parent_name;
    int32_t child_hover_idx{-1};
    int32_t child_slide_elapsed_ms{};
    float child_fade_t{1.0F};
    uint64_t visual_revision{};
    std::vector<ChildMenuRowSnapshot> rows;
};

struct Snapshot {
    int32_t active_root_idx{-1};
    int32_t displayed_parent_idx{-1};
    int32_t child_hover_idx{-1};
    SaoUiMenuPhase phase{SAO_UI_MENU_PHASE_CLOSED};
    float fade_t{1.0F};
    std::array<char, kNameCapacity> active_root_name_utf8{};
    std::array<char, kNameCapacity> displayed_parent_name_utf8{};
    std::vector<RootRowSnapshot> roots;
    std::vector<ChildRowSnapshot> rows;
    uint64_t revision{};
    float transition_eased_t{};
    float center_diffusion_t{};
    float close_suction_t{};
    int32_t selection_trail_idx{-1};
    float selection_trail_t{};
    int32_t pressed_pulse_idx{-1};
    float pressed_pulse_t{};
    float child_rail_glow_t{};
    float backdrop_lens_t{};
    float ambient_phase_t{};
    float open_spark_t{};
    float selection_spark_t{};
    uint32_t open_spark_count{};
    uint32_t selection_spark_count{};
    bool reduced_motion{};
    bool fps_pressure{};
};

SAO_UI_API sao_status_t SAO_UI_CALL set_child_hover(sao_ui_menu_handle_t handle,
                                                     int32_t parent_menu_idx,
                                                     int32_t child_idx);

SAO_UI_API sao_status_t SAO_UI_CALL get_snapshot(sao_ui_menu_handle_t handle,
                                                  Snapshot* out_snapshot);

sao_status_t get_child_menu_snapshot(sao_ui_menu_handle_t handle,
                                     const char* parent_name_utf8,
                                     ChildMenuSnapshot* out_snapshot);

sao_status_t children_match(sao_ui_menu_handle_t handle,
                            const char* parent_name_utf8,
                            const SaoUiMenuItem* items,
                            size_t item_count,
                            bool* out_match) noexcept;

sao_status_t restore_child_menu_snapshot(sao_ui_menu_handle_t handle,
                                         const char* parent_name_utf8,
                                         const ChildMenuSnapshot& snapshot);

sao_status_t activate_child(sao_ui_menu_handle_t handle, int32_t parent_menu_idx,
                            int32_t child_idx, bool* out_activated, int32_t* out_action_id);

sao_status_t activate_root(sao_ui_menu_handle_t handle, int32_t menu_idx,
                           bool emit_interaction);

} // namespace sao::ui::menu_visual

namespace sao::ui::menu_visual {

inline constexpr int32_t kVisualButtonBaseSize = 54;
inline constexpr int32_t kVisualButtonMaxSize = 70;
inline constexpr int32_t kVisualFisheyeNeighbors = 2;
inline constexpr int32_t kVisualMaxVisible = 9;
inline constexpr int32_t kVisualDefaultSlotSize = 70;
inline constexpr int32_t kVisualDefaultChildRadius = 240;
inline constexpr int32_t kVisualChildColumnGap = 25;
inline constexpr int32_t kVisualChildListX = 27;

inline int32_t visual_button_diameter(const SaoUiMenuLayout& layout, float focus_t) noexcept {
    const int32_t base_size = layout.button_size > 0 ? layout.button_size : kVisualButtonBaseSize;
    const int32_t max_size = layout.button_max_size > 0 ? layout.button_max_size : kVisualButtonMaxSize;
    const float focus = focus_t < 0.0F ? 0.0F : focus_t > 1.0F ? 1.0F : focus_t;
    const float size = static_cast<float>(base_size) +
                       static_cast<float>(max_size - base_size) * focus;
    return std::max(1, static_cast<int32_t>(std::lround(size)));
}

inline int32_t visual_child_row_x(int32_t center_x, int32_t slot_size,
                                  int32_t child_ring_radius) noexcept {
    const int32_t slot = slot_size > 0 ? slot_size : kVisualDefaultSlotSize;
    const int32_t root_left = center_x - slot / 2;
    const int32_t ring_offset = std::clamp(
        (child_ring_radius - kVisualDefaultChildRadius) / 4, -16, 16);
    return root_left + slot + kVisualChildColumnGap + kVisualChildListX + ring_offset;
}

inline float unified_fisheye_focus(int32_t distance) noexcept {
    const int32_t absolute = distance < 0 ? -distance : distance;
    if (absolute > kVisualFisheyeNeighbors)
        return 0.0F;
    const float normalized = static_cast<float>(absolute) /
                             static_cast<float>(kVisualFisheyeNeighbors + 1);
    const float focus = 1.0F - normalized * normalized;
    return focus < 0.0F ? 0.0F : focus;
}

inline float root_fisheye_focus(int32_t root_index, int32_t hovered_root_index,
                                float hover_t) noexcept {
    if (hovered_root_index < 0 || hover_t <= 0.0F)
        return 0.0F;
    const float envelope = std::clamp(hover_t, 0.0F, 1.0F);
    return unified_fisheye_focus(root_index - hovered_root_index) * envelope;
}

} // namespace sao::ui::menu_visual
