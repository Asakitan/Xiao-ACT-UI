#pragma once

#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sao_ui_layer_effect_flag_e : uint32_t {
    SAO_UI_LAYER_EFFECT_NONE = 0u,
    SAO_UI_LAYER_EFFECT_BACKDROP_BLUR = 1u << 0u,
    SAO_UI_LAYER_EFFECT_SHADOW = 1u << 1u,
    SAO_UI_LAYER_EFFECT_COLOR_MATRIX = 1u << 2u,
    SAO_UI_LAYER_EFFECT_MODAL_BACKDROP = 1u << 3u,
};

enum SaoUiLayerEffectPreset : int32_t {
    SAO_UI_LAYER_EFFECT_PRESET_MENU = 0,
    SAO_UI_LAYER_EFFECT_PRESET_POPUP = 1,
    SAO_UI_LAYER_EFFECT_PRESET_MODAL = 2,
};

struct SaoUiLayerEffects {
    uint32_t struct_size;
    uint32_t flags;
    float blur_sigma;
    float shadow_sigma;
    float shadow_offset_x;
    float shadow_offset_y;
    uint32_t shadow_argb;
    uint32_t reserved;
    float color_matrix[20];
};

#define SAO_UI_LAYER_EFFECTS_V1_SIZE 112u

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_effects_init(SaoUiLayerEffectPreset preset,
                                                              SaoUiLayerEffects* out_effects);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_effects(sao_ui_layer_handle_t layer,
                                                             const SaoUiLayerEffects* effects);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_get_effects(sao_ui_layer_handle_t layer,
                                                             SaoUiLayerEffects* out_effects);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_d2d_effects_native_available(void* d3d11_device_ptr,
                                                                        bool* out_available);

#ifdef __cplusplus
}

static_assert(sizeof(SaoUiLayerEffects) == SAO_UI_LAYER_EFFECTS_V1_SIZE);
#endif
