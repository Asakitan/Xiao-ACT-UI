#pragma once

#include "sao/ui/compositor.h"

#include <cstdint>

// Routes one host keyboard event to the highest-z visible dialog owned by
// the compositor.  When a dialog is active, out_consumed is true for every
// keyboard phase so background panels and hotkeys remain behind the modal
// barrier.  Only key-down invokes the dialog's Tab/Enter/Escape dispatcher.
sao_status_t dialog_input_route_key(sao_ui_compositor_handle_t compositor, bool key_down,
                                    uint32_t virtual_key, bool shift_held,
                                    bool* out_consumed) noexcept;
