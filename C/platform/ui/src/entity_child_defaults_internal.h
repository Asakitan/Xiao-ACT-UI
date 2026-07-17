#pragma once

#include "sao/ui/entity_shell.h"
#include "sao/ui/menu.h"

#include <array>

namespace sao::ui::entity_child_defaults {

inline sao_status_t apply(sao_ui_menu_handle_t menu) noexcept {
    const std::array<SaoUiMenuItem, 5> roots{{
        {"Control", "C", 10, true, {false, false, false}},
        {"Tools", "T", 11, true, {false, false, false}},
        {"Plugins", "P", 12, true, {false, false, false}},
        {"Skins", "S", 13, true, {false, false, false}},
        {"About", "?", SAO_UI_ENTITY_ACTION_OPEN_ABOUT, true, {false, false, false}},
    }};
    sao_status_t status = sao_ui_menu_set_items(menu, roots.data(), roots.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 8> control{{
        {"置顶: OFF", "⬆", SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, true, {false, false, false}},
        {"NervGear: ON", "◈", SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR, true, {false, false, false}},
        {"──────────", "─", -1, false, {false, false, false}},
        {"Streaming Mode: OFF", "◈", SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE, true,
         {false, false, false}},
        {"鱼眼背景: 程序生成", "◆", SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL, true,
         {false, false, false}},
        {"鱼眼背景: 实时截屏", "◇", SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE, true,
         {false, false, false}},
        {"──────────", "─", -1, false, {false, false, false}},
        {"保存设置", "✓", SAO_UI_ENTITY_ACTION_SAVE_SETTINGS, true, {false, false, false}},
    }};
    status = sao_ui_menu_set_children(menu, "Control", control.data(), control.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 3> tools{{
        {"AI Editor (LLM)", "✦", SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, true, {false, false, false}},
        {"Workshop", "◇", SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, true, {false, false, false}},
        {"Process Selector", "⚙", SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR, true,
         {false, false, false}},
    }};
    status = sao_ui_menu_set_children(menu, "Tools", tools.data(), tools.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 3> plugins{{
        {"插件管理面板 Manage", "⚙", SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER, true,
         {false, false, false}},
        {"重载全部插件 Reload", "↻", SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, true,
         {false, false, false}},
        {"无已启用面板插件 (去 Manage 启用)", "·", -1, false, {false, false, false}},
    }};
    status = sao_ui_menu_set_children(menu, "Plugins", plugins.data(), plugins.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 2> skins{{
        {"全部 Light", "🎨", SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, true, {false, false, false}},
        {"全部 Dark", "🌙", SAO_UI_ENTITY_ACTION_SET_ALL_DARK, true, {false, false, false}},
    }};
    return sao_ui_menu_set_children(menu, "Skins", skins.data(), skins.size());
}

} // namespace sao::ui::entity_child_defaults
