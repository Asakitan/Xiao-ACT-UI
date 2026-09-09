#pragma once

#include "sao/ui/compositor.h"
#include "widget_paint_internal.h"

#include <memory>

namespace sao::ui::detail {

// Publish a value-only draw list. This operation may run on a producer thread;
// Direct2D resources are created, replayed and retired by the compositor thread.
// Publication never runs widget callbacks and never retains a panel/widget.
sao_status_t submit_layer_paint(sao_ui_layer_handle_t layer,
                                std::shared_ptr<const PaintDisplayList> commands, uint32_t width,
                                uint32_t height) noexcept;

} // namespace sao::ui::detail
