#pragma once

#include "sao/ui/widget_text.h"
#include <functional>
#include <string>

namespace sao::ui::detail {
struct TextEditSnapshot {
    std::string text;
    std::string composition;
    size_t selection_start{};
    size_t selection_end{};
    int32_t max_length{65536};
    bool password{};
    bool readonly{};
    bool multiline{};
};
bool text_edit_snapshot(sao_ui_widget_handle_t widget, TextEditSnapshot& out) noexcept;
sao_status_t text_edit_update(sao_ui_widget_handle_t widget,
                              const TextEditSnapshot& value) noexcept;
enum class TextEditPhase { Change, Commit, Cancel, Submit };
using TextEditAction = std::function<void(const std::string&, TextEditPhase)>;
bool begin_native_text_edit(void* hwnd, sao_ui_widget_handle_t widget, int x, int y, int width,
                            int height, TextEditAction action) noexcept;
void end_native_text_edit(void* hwnd, bool commit) noexcept;
void end_native_text_edit_for_widget(void* hwnd, sao_ui_widget_handle_t widget,
                                     bool commit) noexcept;
} // namespace sao::ui::detail
