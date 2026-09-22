#pragma once
#include "sao/ui/compositor.h"
#include <vector>

namespace sao::ui::fisheye_gpu {
struct Renderer;
struct Scene {
	float menu_rect_uv[4]{};
	float host_uv[4]{0.0F, 0.0F, 1.0F, 1.0F};
	float menu_visibility{}, child_activity{}, fps_pressure{}, high_contrast{};
};
sao_status_t create(Renderer** output) noexcept;
void destroy(Renderer* renderer) noexcept;
void set_source(Renderer* renderer, bool live, bool clear_frame) noexcept;
bool has_live_frame(Renderer* renderer) noexcept;
sao_status_t publish_live(Renderer* renderer, std::vector<uint8_t>& frame,
                          uint32_t width, uint32_t height) noexcept;
void update(Renderer* renderer, float seconds, float openness, bool reduced_motion,
            float darkness, float theme_direction, Scene scene = {}) noexcept;
sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* context, void* user) noexcept;
}
