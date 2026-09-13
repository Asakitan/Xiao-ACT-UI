#pragma once

#include "sao/ui/compositor.h"

namespace sao::ui::detail {
sao_status_t configure_navigation_layer(sao_ui_layer_handle_t layer, int32_t resting_z) noexcept;
sao_status_t raise_navigation_layers(sao_ui_layer_handle_t anchor, bool* out_was_behind) noexcept;
}
