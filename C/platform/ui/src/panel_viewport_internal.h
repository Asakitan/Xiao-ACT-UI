#pragma once
#include "sao/ui/panel_layout.h"
#include <vector>

namespace sao::ui::detail {
struct ViewportBar {
    SaoUiRect track{};
    SaoUiRect thumb{};
    SaoUiRect clip{};
    bool active{};
};
void layout_set_viewport(sao_ui_layout_node_handle_t node, const char* id, int axis, bool wheel,
                         bool bar);
bool layout_visual_geometry(sao_ui_layout_node_handle_t node, SaoUiRect& rect, SaoUiRect& clip);
bool layout_scroll_at(sao_ui_layout_node_handle_t root, int x, int y, int dx, int dy);
bool layout_scrollbar_pointer(sao_ui_layout_node_handle_t root, int x, int y, int phase);
std::vector<ViewportBar> layout_scrollbars(sao_ui_layout_node_handle_t root);
void layout_restore_viewports(sao_ui_layout_node_handle_t previous,
                              sao_ui_layout_node_handle_t next);
bool layout_replace_widget(sao_ui_layout_node_handle_t node, sao_ui_widget_handle_t widget);
} // namespace sao::ui::detail
