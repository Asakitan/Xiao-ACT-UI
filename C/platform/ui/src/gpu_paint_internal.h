#pragma once

#include "sao/ui/d2d_widgets.h"
#include "menu_scene_internal.h"

namespace sao::ui::detail {

sao_status_t create_borrowed_d2d_paint_context(void* d2d_render_target, void* dwrite_factory,
                                               sao_ui_paint_ctx_handle_t* out_context) noexcept;

sao_status_t push_gpu_theme_mask(sao_ui_paint_ctx_handle_t context, float progress,
                                 bool dark, ThemePaintFrame frame = {}) noexcept;
sao_status_t pop_gpu_theme_mask(sao_ui_paint_ctx_handle_t context) noexcept;

} // namespace sao::ui::detail
