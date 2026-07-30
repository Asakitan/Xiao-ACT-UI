#pragma once

#include "sao/ui/widget_data.h"

#include <cstdint>

namespace sao::ui::detail {

uint32_t hp_bar_ramp_color(uint32_t low_argb, uint32_t mid_argb, uint32_t high_argb,
                           float ratio) noexcept;

sao_status_t paint_hp_bar(sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y, int32_t width,
                          int32_t height, float fill_ratio, float trail_ratio, uint32_t low_argb,
                          uint32_t mid_argb, uint32_t high_argb, uint32_t trail_argb,
                          int32_t radius_px, int32_t leading_skew_px) noexcept;

} // namespace sao::ui::detail
