#pragma once

#include "sao/ui/menu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sao::ui::menu_visual {

inline constexpr size_t kNameCapacity = 128;
inline constexpr size_t kIconCapacity = 32;

struct ChildRowSnapshot {
    std::array<char, kNameCapacity> name_utf8{};
    std::array<char, kIconCapacity> icon_utf8{};
    int32_t visible_width_px{};
    float hover_t{};
};

struct Snapshot {
    int32_t active_root_idx{-1};
    int32_t displayed_parent_idx{-1};
    int32_t child_hover_idx{-1};
    SaoUiMenuPhase phase{SAO_UI_MENU_PHASE_CLOSED};
    float fade_t{1.0F};
    std::array<char, kNameCapacity> active_root_name_utf8{};
    std::array<char, kNameCapacity> displayed_parent_name_utf8{};
    std::vector<ChildRowSnapshot> rows;
    uint64_t revision{};
};

SAO_UI_API sao_status_t SAO_UI_CALL set_child_hover(sao_ui_menu_handle_t handle,
                                                     int32_t parent_menu_idx,
                                                     int32_t child_idx);

SAO_UI_API sao_status_t SAO_UI_CALL get_snapshot(sao_ui_menu_handle_t handle,
                                                  Snapshot* out_snapshot);

sao_status_t activate_child(sao_ui_menu_handle_t handle, int32_t parent_menu_idx,
                            int32_t child_idx, bool* out_activated, int32_t* out_action_id);

} // namespace sao::ui::menu_visual
