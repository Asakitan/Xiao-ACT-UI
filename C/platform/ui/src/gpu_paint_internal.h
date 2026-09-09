#pragma once

#include "sao/ui/d2d_widgets.h"

namespace sao::ui::detail {

sao_status_t create_borrowed_d2d_paint_context(void* d2d_render_target, void* dwrite_factory,
                                               sao_ui_paint_ctx_handle_t* out_context) noexcept;

} // namespace sao::ui::detail
