// level_up_effect_overlay.cpp — full-screen LevelUp flash overlay (Phase 8/14
// production). Port of python/utils/sao_sound.py::LevelUpEffect.
//
// Adds a BGRA layer to the compositor with alpha animated: 40ms fade-in,
// 60ms hold at full-white, 200ms fade-out. Layer auto-destroyed after cycle.

#include "sao/ui/compositor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// Global compositor handle. Set by launcher init after compositor create.
std::atomic<sao_ui_compositor_handle_t> g_compositor{nullptr};

// Fill an RGBA buffer with premultiplied white * alpha.
void fill_premul_white(uint8_t* buf, uint32_t width, uint32_t height,
                       uint32_t stride, float alpha01) {
    uint8_t a = static_cast<uint8_t>(alpha01 * 255.0f + 0.5f);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = buf + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            // Premultiplied BGRA: RGB *= A/255 = A (white).
            row[x * 4 + 0] = a; // B
            row[x * 4 + 1] = a; // G
            row[x * 4 + 2] = a; // R
            row[x * 4 + 3] = a; // A
        }
    }
}

} // namespace

extern "C" SAO_UI_API void SAO_UI_CALL sao_ui_level_up_effect_overlay_set_compositor(
    sao_ui_compositor_handle_t comp) {
    g_compositor.store(comp);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_level_up_effect_overlay_flash(uint32_t duration_ms) {
    sao_ui_compositor_handle_t comp = g_compositor.load();
    if (comp == nullptr) return; // no compositor bound yet — silent no-op

    // Baseline timeline: 40ms fade-in, 60ms hold, 200ms fade-out (Python parity).
    // If caller passes duration_ms != 0, scale hold segment proportionally.
    const uint32_t attack_ms = 40;
    const uint32_t release_ms = 200;
    uint32_t hold_ms = 60;
    if (duration_ms > attack_ms + release_ms)
        hold_ms = duration_ms - attack_ms - release_ms;

    // Detach on a background thread so the flash animation doesn't block caller.
    std::thread([comp, attack_ms, hold_ms, release_ms]() {
        constexpr uint32_t kW = 1920;
        constexpr uint32_t kH = 1080;
        constexpr uint32_t kStride = kW * 4;
        SaoLayerConfig config{};
        config.struct_size = sizeof(config);
        config.name_utf8 = "sao_ui_level_up_flash";
        config.width = static_cast<int32_t>(kW);
        config.height = static_cast<int32_t>(kH);
        config.click_through = true;
        sao_ui_layer_handle_t layer = nullptr;
        if (sao_ui_layer_create(comp, &config, &layer) !=
                SAO_STATUS_OK ||
            layer == nullptr)
            return;
        std::vector<uint8_t> buf(kStride * kH);
        auto set_alpha = [&](float a) {
            fill_premul_white(buf.data(), kW, kH, kStride, a);
            (void)sao_ui_layer_update_bgra(layer, buf.data(), kW, kH, kStride);
        };
        // Attack.
        const uint32_t frames_attack = attack_ms / 16;
        for (uint32_t i = 0; i <= frames_attack; ++i) {
            float a = (frames_attack == 0) ? 1.0f
                                            : static_cast<float>(i) / frames_attack;
            set_alpha(a);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        // Hold.
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        // Release.
        const uint32_t frames_release = release_ms / 16;
        for (uint32_t i = 0; i <= frames_release; ++i) {
            float a = (frames_release == 0) ? 0.0f
                                             : 1.0f - static_cast<float>(i) / frames_release;
            set_alpha(a);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        sao_ui_layer_destroy(layer);
    }).detach();
}
