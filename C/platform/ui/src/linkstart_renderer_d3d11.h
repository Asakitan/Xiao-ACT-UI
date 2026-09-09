#pragma once

#include "sao/core/status.h"
#include "sao/ui/compositor.h"

#include <cstdint>

namespace sao::ui::linkstart_gpu {

struct Renderer;

struct FrameState {
    float elapsed_seconds{};
    float phase_progress{};
    float phase{};
    float connected_alpha{};
    float startup_prelude{};
    float p1_end{};
    float p2_start{};
    float p2_end{};
    float p3_start{};
    float p3_end{};
    float p4_start{};
    float p4_hold_end{};
    float p4_fade_end{};
    float total_duration{};
    uint32_t seed{};
    bool reduced_motion{};
    bool scene_timeline{};
};

sao_status_t create(Renderer** out_renderer) noexcept;
void destroy(Renderer* renderer) noexcept;
void update(Renderer* renderer, const FrameState& frame) noexcept;
sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* context,
                                void* user_data) noexcept;

} // namespace sao::ui::linkstart_gpu
