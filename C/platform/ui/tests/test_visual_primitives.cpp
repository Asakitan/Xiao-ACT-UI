#include <catch2/catch_test_macros.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/d2d_effects.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/linkstart_intro.h"
#include "sao/ui/nervegear.h"
#include "sao/ui/particle_system.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct Pixel {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
};

sao_ui_compositor_handle_t make_headless_compositor() {
    SaoCompositorConfig config{};
    config.struct_size = sizeof(config);
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    return compositor;
}

std::vector<uint8_t> compositor_snapshot(sao_ui_compositor_handle_t compositor,
                                         uint32_t* out_width = nullptr,
                                         uint32_t* out_height = nullptr) {
    uint32_t width = 0u;
    uint32_t height = 0u;
    size_t bytes = 0u;
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0u, &width, &height, &bytes) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(bytes);
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(), &width,
                                            &height, &bytes) == SAO_STATUS_OK);
    if (out_width != nullptr)
        *out_width = width;
    if (out_height != nullptr)
        *out_height = height;
    return pixels;
}

std::vector<Pixel> raster_snapshot(sao_ui_offscreen_raster_handle_t raster) {
    size_t bytes = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t stride = 0u;
    REQUIRE(sao_ui_offscreen_raster_snapshot(raster, nullptr, 0u, &bytes, &width, &height,
                                             &stride) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<Pixel> pixels(bytes / sizeof(Pixel));
    REQUIRE(sao_ui_offscreen_raster_snapshot(raster, reinterpret_cast<uint8_t*>(pixels.data()),
                                             bytes, &bytes, &width, &height,
                                             &stride) == SAO_STATUS_OK);
    return pixels;
}

SaoUiParticleEmitterConfig particle_config(uint32_t seed) {
    SaoUiParticleEmitterConfig config{};
    config.struct_size = sizeof(config);
    config.max_particles = 128u;
    config.seed = seed;
    config.origin_x = 32.0F;
    config.origin_y = 32.0F;
    config.min_speed = 10.0F;
    config.max_speed = 30.0F;
    config.direction_radians = 0.0F;
    config.spread_radians = 6.28318530717958647692F;
    config.min_life_seconds = 1.0F;
    config.max_life_seconds = 2.0F;
    config.min_size_px = 1.0F;
    config.max_size_px = 3.0F;
    config.acceleration_y = 2.0F;
    config.damping_per_second = 0.1F;
    config.color_start_argb = 0xff68e4ffu;
    config.color_end_argb = 0xffd49c17u;
    config.prefer_gpu = true;
    return config;
}

} // namespace

TEST_CASE("particle emitters are deterministic for a fixed seed", "[ui][particle][determinism]") {
    const SaoUiParticleEmitterConfig config = particle_config(0x12345678u);
    sao_ui_particle_emitter_handle_t first = nullptr;
    sao_ui_particle_emitter_handle_t second = nullptr;
    REQUIRE(sao_ui_particle_emitter_create(nullptr, &config, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_particle_emitter_create(nullptr, &config, &second) == SAO_STATUS_OK);

    uint32_t first_spawned = 0u;
    uint32_t second_spawned = 0u;
    REQUIRE(sao_ui_particle_emitter_spawn_burst(first, 32u, &first_spawned) == SAO_STATUS_OK);
    REQUIRE(sao_ui_particle_emitter_spawn_burst(second, 32u, &second_spawned) == SAO_STATUS_OK);
    REQUIRE(first_spawned == 32u);
    REQUIRE(second_spawned == first_spawned);
    REQUIRE(sao_ui_particle_emitter_update(first, 0.125F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_particle_emitter_update(second, 0.125F) == SAO_STATUS_OK);

    size_t first_count = 0u;
    size_t second_count = 0u;
    REQUIRE(sao_ui_particle_emitter_snapshot(first, nullptr, 0u, &first_count) == SAO_STATUS_OK);
    REQUIRE(sao_ui_particle_emitter_snapshot(second, nullptr, 0u, &second_count) == SAO_STATUS_OK);
    REQUIRE(first_count == second_count);
    std::vector<SaoUiParticle> first_particles(first_count);
    std::vector<SaoUiParticle> second_particles(second_count);
    REQUIRE(sao_ui_particle_emitter_snapshot(first, first_particles.data(), first_particles.size(),
                                             &first_count) == SAO_STATUS_OK);
    REQUIRE(sao_ui_particle_emitter_snapshot(second, second_particles.data(),
                                             second_particles.size(),
                                             &second_count) == SAO_STATUS_OK);
    CHECK(std::memcmp(first_particles.data(), second_particles.data(),
                      first_particles.size() * sizeof(SaoUiParticle)) == 0);

    void* gpu_buffer = reinterpret_cast<void*>(1);
    CHECK(sao_ui_particle_emitter_gpu_buffer(first, &gpu_buffer) ==
          SAO_STATUS_ERR_CAPABILITY_MISSING);
    CHECK(gpu_buffer == nullptr);

    SaoUiOffscreenRasterDesc raster_desc{64u, 64u, 0x00000000u};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t paint_ctx = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &paint_ctx) == SAO_STATUS_OK);
    REQUIRE(sao_ui_particle_emitter_render(first, paint_ctx) == SAO_STATUS_OK);
    const auto rendered = raster_snapshot(raster);
    CHECK(std::ranges::any_of(rendered, [](const Pixel& pixel) { return pixel.a != 0u; }));

    sao_ui_paint_ctx_destroy(paint_ctx);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_particle_emitter_destroy(second);
    sao_ui_particle_emitter_destroy(first);
}

TEST_CASE("hp bar paints polygon clipped red yellow green ramp", "[ui][widget][hp][paint]") {
    SaoUiProgressBarSpec spec{};
    spec.value = 1.0F;
    spec.max_value = 1.0F;
    spec.style = SAO_UI_PROGRESS_HP_RAMP;
    spec.bg_argb = 0xff000000u;
    spec.fill_low_argb = 0xffff0000u;
    spec.fill_mid_argb = 0xffffff00u;
    spec.fill_high_argb = 0xff00ff00u;
    spec.leading_skew_px = 8;
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &widget) == SAO_STATUS_OK);

    SaoUiOffscreenRasterDesc raster_desc{100u, 12u, 0x00000000u};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t paint_ctx = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &paint_ctx) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint_at(widget, paint_ctx, 0, 0, 100, 12, 1.0F) == SAO_STATUS_OK);
    const auto pixels = raster_snapshot(raster);
    const Pixel low = pixels[6u * 100u + 5u];
    const Pixel middle = pixels[6u * 100u + 25u];
    const Pixel high = pixels[6u * 100u + 75u];
    CHECK(low.r > low.g);
    CHECK(middle.r > 220u);
    CHECK(middle.g > 220u);
    CHECK(high.g > high.r);
    CHECK(pixels[0u * 100u + 98u].r == 0u);
    CHECK(pixels[11u * 100u + 98u].g > 200u);

    sao_ui_paint_ctx_destroy(paint_ctx);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(widget);
}

TEST_CASE("d2d effects probe noops without a device and software fallback composes",
          "[ui][effects][fallback]") {
    bool native_available = true;
    REQUIRE(sao_ui_d2d_effects_native_available(nullptr, &native_available) == SAO_STATUS_OK);
    CHECK_FALSE(native_available);

    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    SaoLayerConfig base_config{};
    base_config.struct_size = sizeof(base_config);
    base_config.name_utf8 = "effects.base";
    base_config.width = 16;
    base_config.height = 8;
    base_config.bgra_swizzle = true;
    sao_ui_layer_handle_t base = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &base_config, &base) == SAO_STATUS_OK);
    std::vector<uint8_t> base_pixels(16u * 8u * 4u, 0u);
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            uint8_t* pixel = base_pixels.data() + (static_cast<size_t>(y) * 16u + x) * 4u;
            pixel[0] = x < 8u ? 0u : 255u;
            pixel[2] = x < 8u ? 255u : 0u;
            pixel[3] = 255u;
        }
    }
    REQUIRE(sao_ui_layer_update_bgra(base, base_pixels.data(), 16u, 8u, 64u) == SAO_STATUS_OK);
    const auto before = compositor_snapshot(compositor);

    SaoLayerConfig modal_config{};
    modal_config.struct_size = sizeof(modal_config);
    modal_config.name_utf8 = "effects.modal";
    modal_config.width = 1;
    modal_config.height = 1;
    modal_config.z_order = 10;
    modal_config.click_through = true;
    modal_config.bgra_swizzle = true;
    sao_ui_layer_handle_t modal = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &modal_config, &modal) == SAO_STATUS_OK);
    const uint8_t transparent[4]{};
    REQUIRE(sao_ui_layer_update_bgra(modal, transparent, 1u, 1u, 4u) == SAO_STATUS_OK);
    SaoUiLayerEffects effects{};
    REQUIRE(sao_ui_layer_effects_init(SAO_UI_LAYER_EFFECT_PRESET_MODAL, &effects) == SAO_STATUS_OK);
    effects.flags &= ~SAO_UI_LAYER_EFFECT_SHADOW;
    REQUIRE(sao_ui_layer_set_effects(modal, &effects) == SAO_STATUS_OK);
    SaoUiLayerEffects roundtrip{};
    REQUIRE(sao_ui_layer_get_effects(modal, &roundtrip) == SAO_STATUS_OK);
    CHECK((roundtrip.flags & SAO_UI_LAYER_EFFECT_MODAL_BACKDROP) != 0u);

    uint32_t width = 0u;
    uint32_t height = 0u;
    const auto after = compositor_snapshot(compositor, &width, &height);
    REQUIRE(width == 16u);
    REQUIRE(height == 8u);
    CHECK(after != before);
    CHECK(after[2u] < before[2u]);

    sao_ui_layer_destroy(modal);
    sao_ui_layer_destroy(base);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("linkstart software overlay follows the nervegear timeline",
          "[ui][linkstart][timeline]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    sao_ui_nervegear_handle_t nervegear = nullptr;
    REQUIRE(sao_ui_nervegear_create(compositor, nullptr, 0, 0, SAO_UI_NG_PALETTE_DARK,
                                    &nervegear) == SAO_STATUS_OK);

    SaoUiLinkStartTimeline timeline{};
    timeline.startup_prelude = 0.02F;
    timeline.p1_end = 0.10F;
    timeline.p2_start = 0.10F;
    timeline.p2_end = 0.22F;
    timeline.p3_start = 0.18F;
    timeline.p3_end = 0.32F;
    timeline.p4_start = 0.28F;
    timeline.p4_hold_end = 0.40F;
    timeline.p4_fade_end = 0.48F;
    timeline.total_duration = 0.50F;
    SaoUiLinkStartConfig config{};
    config.struct_size = sizeof(config);
    config.width_px = 160u;
    config.height_px = 90u;
    config.particle_seed = 0x55aa7711u;
    config.timeline = &timeline;
    sao_ui_linkstart_handle_t intro = nullptr;
    REQUIRE(sao_ui_linkstart_create(compositor, nervegear, &config, &intro) == SAO_STATUS_OK);
    REQUIRE(sao_ui_linkstart_show(intro) == SAO_STATUS_OK);

    bool active = false;
    REQUIRE(sao_ui_linkstart_is_active(intro, &active) == SAO_STATUS_OK);
    CHECK(active);
    SaoUiNerveGearState nervegear_state = SAO_UI_NG_STATE_IDLE;
    REQUIRE(sao_ui_nervegear_get_state(nervegear, &nervegear_state) == SAO_STATUS_OK);
    CHECK(nervegear_state == SAO_UI_NG_STATE_LINKING);
    SaoUiLinkStartPhase phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
    float phase_progress = 0.0F;
    REQUIRE(sao_ui_linkstart_get_phase(intro, &phase, &phase_progress) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL);
    uint32_t width = 0u;
    uint32_t height = 0u;
    const auto frame = compositor_snapshot(compositor, &width, &height);
    CHECK(width == 160u);
    CHECK(height == 90u);
    CHECK(std::ranges::any_of(frame, [](uint8_t value) { return value != 0u; }));

    REQUIRE(sao_ui_linkstart_tick(intro, 190) == SAO_STATUS_OK);
    REQUIRE(sao_ui_linkstart_get_phase(intro, &phase, &phase_progress) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_LINKSTART_PHASE_RADIAL_BURST);
    REQUIRE(sao_ui_linkstart_tick(intro, 310) == SAO_STATUS_OK);
    REQUIRE(sao_ui_linkstart_is_active(intro, &active) == SAO_STATUS_OK);
    CHECK_FALSE(active);
    REQUIRE(sao_ui_nervegear_get_state(nervegear, &nervegear_state) == SAO_STATUS_OK);
    CHECK(nervegear_state == SAO_UI_NG_STATE_LINKED);
    REQUIRE(sao_ui_linkstart_get_phase(intro, &phase, &phase_progress) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_LINKSTART_PHASE_COMPLETE);

    REQUIRE(sao_ui_linkstart_show(intro) == SAO_STATUS_OK);
    REQUIRE(sao_ui_linkstart_dismiss(intro) == SAO_STATUS_OK);
    REQUIRE(sao_ui_nervegear_get_state(nervegear, &nervegear_state) == SAO_STATUS_OK);
    CHECK(nervegear_state == SAO_UI_NG_STATE_IDLE);

    sao_ui_linkstart_destroy(intro);
    sao_ui_nervegear_destroy(nervegear);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("hud chrome primitives render through the shared raster path", "[ui][widget][chrome]") {
    SaoUiOffscreenRasterDesc raster_desc{32u, 32u, 0x00000000u};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t paint_ctx = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &paint_ctx) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_draw_corner_brackets(paint_ctx, 2.0F, 2.0F, 28.0F, 28.0F, 6.0F, 1.0F,
                                                  0xff68e4ffu) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_draw_scanlines(paint_ctx, 2.0F, 2.0F, 28.0F, 28.0F, 4.0F, 1.0F,
                                            0x4000c8ffu) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_draw_clock_pulse(paint_ctx, 16.0F, 16.0F, 10.0F, 0.5F, 1.0F,
                                              0xffd49c17u) == SAO_STATUS_OK);
    const auto pixels = raster_snapshot(raster);
    CHECK(std::ranges::any_of(pixels, [](const Pixel& pixel) { return pixel.a != 0u; }));
    sao_ui_paint_ctx_destroy(paint_ctx);
    sao_ui_offscreen_raster_destroy(raster);
}
