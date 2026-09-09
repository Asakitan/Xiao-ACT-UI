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
    uint32_t seed{};
    bool reduced_motion{};
};

sao_status_t create(Renderer** out_renderer) noexcept;
void destroy(Renderer* renderer) noexcept;
void update(Renderer* renderer, const FrameState& frame) noexcept;
sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* context,
                                void* user_data) noexcept;

} // namespace sao::ui::linkstart_gpu
