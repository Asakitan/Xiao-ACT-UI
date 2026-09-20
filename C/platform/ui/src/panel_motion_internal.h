#pragma once

#include "sao/ui/compositor.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace sao::ui::detail {

struct PanelMotionReturnFocus {
    uint64_t scope_token{};
    std::string source_id;
    int32_t origin_x{};
    int32_t origin_y{};
};

// Same-DLL, same-thread scope; the first successful hidden-to-visible transition consumes it.
class ScopedPanelMotionOrigin final {
  public:
    ScopedPanelMotionOrigin(sao_ui_compositor_handle_t compositor, int32_t host_x,
                            int32_t host_y, std::string_view source_id = {},
                            uint64_t scope_token = 0) noexcept;
    ~ScopedPanelMotionOrigin();
    ScopedPanelMotionOrigin(const ScopedPanelMotionOrigin&) = delete;
    ScopedPanelMotionOrigin& operator=(const ScopedPanelMotionOrigin&) = delete;

  private:
    friend struct PanelMotionOriginAccess;
    sao_ui_compositor_handle_t compositor_{};
    PanelMotionReturnFocus source_;
    ScopedPanelMotionOrigin* previous_{};
    bool consumed_{};
};

// Call once per compositor frame on its owner thread, outside the entity mutex.
sao_status_t tick_panel_motion(sao_ui_compositor_handle_t compositor, uint32_t dt_ms) noexcept;

// Cues expire after one second and are invalidated by reopening or destroying the panel.
bool take_panel_motion_return_focus(sao_ui_compositor_handle_t compositor, uint64_t scope_token,
                                   PanelMotionReturnFocus* out_cue) noexcept;

} // namespace sao::ui::detail
