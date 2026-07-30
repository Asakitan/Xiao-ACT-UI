// Per-layer shader-pass pipeline (14.5.93) — CPU fallback path.
//
// The CPU path is the deterministic reference implementation: it always
// produces the correct visual result and is used under WARP / headless /
// software raster.  A D3D11 pixel-shader path can be layered on top via
// sao_ui_shader_pass_native_available without changing the visual contract.
//
// All passes operate on premultiplied BGRA (B8G8R8A8) pixels in place.

#include "sao/ui/shader_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int32_t kPresetMenu = 0;
constexpr int32_t kPresetPopup = 1;
constexpr int32_t kPresetBanner = 2;

uint8_t clamp_byte(float v) {
    return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

float pixel_luminance(const uint8_t* p) {
    // Rec.601 luma on 0..1.
    return (0.299F * p[2] + 0.587F * p[1] + 0.114F * p[0]) / 255.0F;
}

// Separable box-blur approximation of a gaussian over the luminance-extracted
// glow mask; three passes approximate a gaussian closely enough for glow.
void glow_blur(std::vector<float>& mask, uint32_t width, uint32_t height, int radius) {
    if (radius <= 0) return;
    std::vector<float> tmp(mask.size());
    const int r = std::min(radius, 32);
    const float inv = 1.0F / static_cast<float>(2 * r + 1);
    for (int pass = 0; pass < 3; ++pass) {
        // Horizontal.
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                float sum = 0.0F;
                for (int k = -r; k <= r; ++k) {
                    const int xx = std::clamp(static_cast<int>(x) + k, 0, static_cast<int>(width) - 1);
                    sum += mask[static_cast<size_t>(y) * width + xx];
                }
                tmp[static_cast<size_t>(y) * width + x] = sum * inv;
            }
        }
        // Vertical.
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                float sum = 0.0F;
                for (int k = -r; k <= r; ++k) {
                    const int yy = std::clamp(static_cast<int>(y) + k, 0, static_cast<int>(height) - 1);
                    sum += tmp[static_cast<size_t>(yy) * width + x];
                }
                mask[static_cast<size_t>(y) * width + x] = sum * inv;
            }
        }
    }
}

void apply_bloom(const SaoUiShaderPass* pass, uint8_t* pixels, uint32_t width, uint32_t height,
                 uint32_t stride) {
    std::vector<float> glow(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            const float lum = pixel_luminance(p);
            glow[static_cast<size_t>(y) * width + x] =
                lum > pass->bloom_threshold ? (lum - pass->bloom_threshold) : 0.0F;
        }
    }
    glow_blur(glow, width, height, static_cast<int>(std::lround(pass->bloom_radius_px)));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            const float g = glow[static_cast<size_t>(y) * width + x] * pass->bloom_intensity * 255.0F;
            p[0] = clamp_byte(static_cast<float>(p[0]) + g);
            p[1] = clamp_byte(static_cast<float>(p[1]) + g);
            p[2] = clamp_byte(static_cast<float>(p[2]) + g);
        }
    }
}

void apply_vignette(const SaoUiShaderPass* pass, uint8_t* pixels, uint32_t width, uint32_t height,
                    uint32_t stride) {
    const float cx = width * 0.5F;
    const float cy = height * 0.5F;
    const float max_dist = std::sqrt(cx * cx + cy * cy);
    const float softness = std::max(pass->vignette_softness, 0.001F);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float dx = (static_cast<float>(x) - cx) / cx;
            const float dy = (static_cast<float>(y) - cy) / cy;
            const float dist = std::sqrt(dx * dx + dy * dy) * 0.7071F; // normalize corner to ~1
            float dark = std::clamp((dist - (1.0F - softness)) / softness, 0.0F, 1.0F);
            dark *= pass->vignette_strength;
            if (dark <= 0.0F) continue;
            uint8_t* p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            const float keep = 1.0F - dark;
            p[0] = static_cast<uint8_t>(std::lround(p[0] * keep));
            p[1] = static_cast<uint8_t>(std::lround(p[1] * keep));
            p[2] = static_cast<uint8_t>(std::lround(p[2] * keep));
            (void)max_dist;
        }
    }
}

void apply_hue_shift(const SaoUiShaderPass* pass, uint8_t* pixels, uint32_t width, uint32_t height,
                     uint32_t stride) {
    const float rad = pass->hue_shift_deg * 0.01745329251F;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    // YIQ rotation matrix for hue shift.
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            const float r = p[2], g = p[1], b = p[0];
            const float nr = (0.299F + 0.701F * c + 0.168F * s) * r +
                             (0.587F - 0.587F * c + 0.330F * s) * g +
                             (0.114F - 0.114F * c - 0.497F * s) * b;
            const float ng = (0.299F - 0.299F * c - 0.328F * s) * r +
                             (0.587F + 0.413F * c + 0.035F * s) * g +
                             (0.114F - 0.114F * c + 0.292F * s) * b;
            const float nb = (0.299F - 0.300F * c + 1.250F * s) * r +
                             (0.587F - 0.588F * c - 1.050F * s) * g +
                             (0.114F + 0.886F * c - 0.203F * s) * b;
            p[2] = clamp_byte(nr);
            p[1] = clamp_byte(ng);
            p[0] = clamp_byte(nb);
        }
    }
}

void apply_edge_feather(const SaoUiShaderPass* pass, uint8_t* pixels, uint32_t width, uint32_t height,
                        uint32_t stride) {
    const int feather = std::max(0, static_cast<int>(std::lround(pass->feather_px)));
    if (feather <= 0) return;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const int edge = std::min(std::min(static_cast<int>(x), static_cast<int>(width - 1 - x)),
                                      std::min(static_cast<int>(y), static_cast<int>(height - 1 - y)));
            if (edge >= feather) continue;
            const float scale = static_cast<float>(edge) / static_cast<float>(feather);
            uint8_t* p = pixels + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            p[3] = static_cast<uint8_t>(std::lround(p[3] * scale));
        }
    }
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_shader_pass_init(int32_t preset,
                                                            SaoUiShaderPass* out_pass) {
    if (out_pass == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_pass = {};
    out_pass->struct_size = SAO_UI_SHADER_PASS_V1_SIZE;
    switch (preset) {
    case kPresetMenu:
        out_pass->flags = SAO_UI_SHADER_PASS_BLOOM | SAO_UI_SHADER_PASS_VIGNETTE;
        out_pass->bloom_threshold = 0.7F;
        out_pass->bloom_intensity = 0.35F;
        out_pass->bloom_radius_px = 6.0F;
        out_pass->vignette_strength = 0.25F;
        out_pass->vignette_softness = 0.6F;
        break;
    case kPresetPopup:
        out_pass->flags = SAO_UI_SHADER_PASS_EDGE_FEATHER | SAO_UI_SHADER_PASS_VIGNETTE;
        out_pass->feather_px = 2.0F;
        out_pass->vignette_strength = 0.12F;
        out_pass->vignette_softness = 0.5F;
        break;
    case kPresetBanner:
        out_pass->flags = SAO_UI_SHADER_PASS_BLOOM | SAO_UI_SHADER_PASS_HUE_SHIFT;
        out_pass->bloom_threshold = 0.6F;
        out_pass->bloom_intensity = 0.5F;
        out_pass->bloom_radius_px = 8.0F;
        out_pass->hue_shift_deg = 0.0F;
        break;
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_shader_pass_apply_cpu(
    const SaoUiShaderPass* pass, uint8_t* pixels, uint32_t width, uint32_t height,
    uint32_t stride_bytes) {
    if (pass == nullptr || pixels == nullptr || width == 0 || height == 0 ||
        stride_bytes < width * 4 || pass->struct_size < SAO_UI_SHADER_PASS_V1_SIZE) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        if ((pass->flags & SAO_UI_SHADER_PASS_HUE_SHIFT) != 0)
            apply_hue_shift(pass, pixels, width, height, stride_bytes);
        if ((pass->flags & SAO_UI_SHADER_PASS_BLOOM) != 0)
            apply_bloom(pass, pixels, width, height, stride_bytes);
        if ((pass->flags & SAO_UI_SHADER_PASS_VIGNETTE) != 0)
            apply_vignette(pass, pixels, width, height, stride_bytes);
        if ((pass->flags & SAO_UI_SHADER_PASS_EDGE_FEATHER) != 0)
            apply_edge_feather(pass, pixels, width, height, stride_bytes);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_shader_pass_native_available(void* d3d11_device_ptr,
                                                                        bool* out_available) {
    if (out_available == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    // GPU pixel-shader path is layered later; CPU path is the reference.
    *out_available = d3d11_device_ptr != nullptr;
    return SAO_STATUS_OK;
}
