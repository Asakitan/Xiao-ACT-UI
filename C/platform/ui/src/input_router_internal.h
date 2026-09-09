#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "sao/core/status.h"
#include "sao/ui/overlay_host.h"

namespace sao::ui::input_router_detail {

inline bool legacy_tk_input_enabled() noexcept {
    // Frozen Tk/Python input compatibility is opt-in and defaults off.
    const char* value = std::getenv("SAO_UI_LEGACY_TK_INPUT");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

struct LayerInputState;

enum class LayerInputActionKind : uint8_t {
    cursor,
    leave,
    button,
    scroll,
    raw_mouse,
};

struct LayerInputAction {
    LayerInputActionKind kind{};
    void* layer{};
    float x{};
    float y{};
    float scroll_dx{};
    float scroll_dy{};
    int32_t button{-1};
    int32_t action{};
    uint32_t message{};
    uint32_t key_state{};
    int32_t wheel_delta{};
    bool coordinates_are_host{};
};

LayerInputState* create_layer_input_state() noexcept;
void destroy_layer_input_state(LayerInputState* state) noexcept;

bool layer_event_uses_coordinates(uint32_t message) noexcept;

sao_status_t route_layer_input(LayerInputState* state, uint32_t message, int32_t host_x,
                               int32_t host_y, int32_t button, int32_t wheel_delta,
                               uint32_t key_state, void* hit_layer, bool hit_uses_raw_input,
                               float hit_x, float hit_y,
                               LayerInputAction* out_actions, size_t action_capacity,
                               size_t* out_action_count) noexcept;

bool layer_input_references(const LayerInputState* state, const void* layer) noexcept;
bool layer_input_last_pointer(const LayerInputState* state, int32_t* out_x,
                              int32_t* out_y) noexcept;

sao_status_t invalidate_layer_input(LayerInputState* state, void* layer,
                                    bool still_accepts_at_pointer,
                                    LayerInputAction* out_actions, size_t action_capacity,
                                    size_t* out_action_count) noexcept;

void reset_layer_input(LayerInputState* state) noexcept;
uint64_t layer_input_writer_revision(const LayerInputState* state) noexcept;

sao_status_t apply_host_input_regions(sao_ui_overlay_host_handle_t host,
                                      const SaoOverlayHostInputRect* rects,
                                      size_t rect_count) noexcept;
sao_status_t apply_host_input_passthrough(sao_ui_overlay_host_handle_t host,
                                          bool passthrough) noexcept;
sao_status_t reset_host_input(sao_ui_overlay_host_handle_t host) noexcept;

} // namespace sao::ui::input_router_detail
