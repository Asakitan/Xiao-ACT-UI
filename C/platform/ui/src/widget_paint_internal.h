#pragma once

#include "sao/ui/d2d_widgets.h"

#include <cstdint>

namespace sao::ui::detail {

sao_status_t paint_rounded_rect(sao_ui_paint_ctx_handle_t context, float x, float y,
                                float width, float height, float radius,
                                uint32_t argb) noexcept;

sao_status_t paint_rounded_rect_stroke(sao_ui_paint_ctx_handle_t context, float x, float y,
                                       float width, float height, float radius,
                                       float stroke_width, uint32_t argb) noexcept;

sao_status_t paint_focus_ring(sao_ui_paint_ctx_handle_t context, float x, float y,
                              float width, float height, float radius, bool rounded,
                              uint32_t argb) noexcept;

sao_status_t paint_elevation_shadow(sao_ui_paint_ctx_handle_t context, float x, float y,
                                    float width, float height, float radius,
                                    int32_t elevation, uint32_t argb) noexcept;

} // namespace sao::ui::detail
