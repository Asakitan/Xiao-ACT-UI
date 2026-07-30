#pragma once

// Per-layer shader-pass pipeline (14.5.93).
//
// Generic post-processing primitives applicable to any compositor layer:
// bloom/glow, radial vignette, hue shift, edge feather.  GPU path (D3D11
// pixel shader) when a device is present; a deterministic CPU fallback keeps
// the same visual result so panels degrade gracefully under WARP or headless
// software raster.  Game-agnostic — plugin skillFX/energy layers consume
// these primitives via their own passes, not built into the platform.

#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sao_ui_shader_pass_flag_e : uint32_t {
    SAO_UI_SHADER_PASS_NONE = 0u,
    SAO_UI_SHADER_PASS_BLOOM = 1u << 0u,        // threshold + gaussian glow add
    SAO_UI_SHADER_PASS_VIGNETTE = 1u << 1u,     // radial darkening toward edges
    SAO_UI_SHADER_PASS_HUE_SHIFT = 1u << 2u,    // rotate hue by degrees
    SAO_UI_SHADER_PASS_EDGE_FEATHER = 1u << 3u, // soften layer edge alpha
};

struct SaoUiShaderPass {
    uint32_t struct_size;
    uint32_t flags;
    float bloom_threshold;   // 0..1 luminance cutoff before glow
    float bloom_intensity;   // 0..N glow add strength
    float bloom_radius_px;   // gaussian radius in pixels
    float vignette_strength; // 0..1 darkening at corners
    float vignette_softness; // 0..1 falloff curve
    float hue_shift_deg;     // -360..360
    float feather_px;        // edge feather width in pixels
    uint32_t reserved[3];    // ABI headroom; keeps struct 8-byte aligned
};

#define SAO_UI_SHADER_PASS_V1_SIZE 48u

// Initialize a pass preset (menu glow, popup soft, banner vivid).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_shader_pass_init(int32_t preset,
                                                            SaoUiShaderPass* out_pass);

// Apply the pass to a premultiplied BGRA pixel buffer in place.
// `pixels` is width*height*4 bytes (B8G8R8A8, premultiplied).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_shader_pass_apply_cpu(
    const SaoUiShaderPass* pass, uint8_t* pixels, uint32_t width, uint32_t height,
    uint32_t stride_bytes);

// Returns true when a D3D11 GPU path is available for the pass.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_shader_pass_native_available(void* d3d11_device_ptr,
                                                                        bool* out_available);

#ifdef __cplusplus
}

static_assert(sizeof(SaoUiShaderPass) == SAO_UI_SHADER_PASS_V1_SIZE);
#endif
