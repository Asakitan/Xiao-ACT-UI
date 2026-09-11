#pragma once
#include "sao/ui/compositor.h"

namespace sao::ui::fisheye_gpu {
struct Renderer;
sao_status_t create(Renderer** output) noexcept;
void destroy(Renderer* renderer) noexcept;
void update(Renderer* renderer, float seconds, float openness, bool reduced_motion) noexcept;
sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* context, void* user) noexcept;
}
