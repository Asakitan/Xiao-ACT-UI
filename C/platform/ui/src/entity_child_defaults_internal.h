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
        {"置顶: OFF", "⬆", 100, true, {false, false, false}},
        {"NervGear: ON", "◈", 101, true, {false, false, false}},
        {"──────────", "─", -1, false, {false, false, false}},
        {"Streaming Mode: OFF", "◈", 102, true, {false, false, false}},
        {"鱼眼背景: 程序生成", "◆", 104, true, {false, false, false}},
        {"鱼眼背景: 实时截屏", "◇", 105, true, {false, false, false}},
        {"──────────", "─", -1, false, {false, false, false}},
        {"保存设置", "✓", 103, true, {false, false, false}},
    }};
    status = sao_ui_menu_set_children(menu, "Control", control.data(), control.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 3> tools{{
        {"AI Editor (LLM)", "✦", 110, true, {false, false, false}},
        {"Workshop", "◇", 111, true, {false, false, false}},
        {"Process Selector", "⚙", 112, true, {false, false, false}},
    }};
    status = sao_ui_menu_set_children(menu, "Tools", tools.data(), tools.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 3> plugins{{
        {"插件管理面板 Manage", "⚙", 120, true, {false, false, false}},
        {"重载全部插件 Reload", "↻", 121, true, {false, false, false}},
        {"无已启用面板插件 (去 Manage 启用)", "·", 122, true, {false, false, false}},
    }};
    status = sao_ui_menu_set_children(menu, "Plugins", plugins.data(), plugins.size());
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 2> skins{{
        {"全部 Light", "🎨", 130, true, {false, false, false}},
        {"全部 Dark", "🌙", 131, true, {false, false, false}},
    }};
    return sao_ui_menu_set_children(menu, "Skins", skins.data(), skins.size());
}

} // namespace sao::ui::entity_child_defaults
