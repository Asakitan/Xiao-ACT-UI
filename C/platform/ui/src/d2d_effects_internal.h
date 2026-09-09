#pragma once

#include "sao/ui/d2d_effects.h"

#include <cstdint>
#include <vector>

namespace sao::ui::effects {

bool validate(const SaoUiLayerEffects& effects) noexcept;

void apply_precompose(std::vector<uint8_t>& canvas, uint32_t canvas_width, uint32_t canvas_height,
                      const uint8_t* layer_pixels, uint32_t layer_width, uint32_t layer_height,
                      uint32_t layer_stride, int32_t layer_x, int32_t layer_y, float layer_alpha,
                      const SaoUiLayerEffects& effects, void* d3d11_device_ptr) noexcept;

bool native_available(void* d3d11_device_ptr) noexcept;

struct GpuEffectGraph;

// Owner-thread only. All inputs stay in GPU memory; the graph retains device
// resources until reset on compositor resize/device loss/shutdown.
sao_status_t apply_gpu_precompose(GpuEffectGraph** graph, void* d3d11_device, void* d3d11_context,
                                  void* master_texture, void* layer_texture, int32_t layer_x,
                                  int32_t layer_y, float layer_alpha,
                                  const SaoUiLayerEffects& effects) noexcept;
void destroy_gpu_effect_graph(GpuEffectGraph* graph) noexcept;

} // namespace sao::ui::effects
