// ProgressBar style, ratio, and animation tests.
//
// Coverage (4 CASE, one per progress style):
//   * progress_flat_fill_ratio_clamps
//   * progress_hp_ramp_resolves_colour_by_threshold
//   * progress_hp_trail_lags_behind_displayed
//   * progress_segments_stored_and_indexable
//
// All tests are pure state — no D3D device.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/widget_kit.h"

extern "C" {

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_fill_ratio(sao_ui_widget_handle_t handle, float* out_ratio);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_trail_ratio(sao_ui_widget_handle_t handle, float* out_ratio);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_resolve_fill_argb(sao_ui_widget_handle_t handle, uint32_t* out_argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_progress_tick(sao_ui_widget_handle_t handle,
                                                                int32_t dt_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_progress_get_segment(
    sao_ui_widget_handle_t handle, size_t index, SaoUiProgressSegment* out_segment);

SAO_UI_API size_t SAO_UI_CALL
sao_ui_widget_progress_get_segment_count(sao_ui_widget_handle_t handle);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle);

} // extern "C"

namespace {

struct Pixel {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
};

std::vector<Pixel> paint_snapshot(sao_ui_widget_handle_t widget, uint32_t width, uint32_t height) {
    SaoUiOffscreenRasterDesc desc{width, height, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint_at(widget, context, 0, 0, static_cast<int32_t>(width),
                                   static_cast<int32_t>(height), 1.0F) == SAO_STATUS_OK);
    size_t bytes = 0;
    uint32_t actual_width = 0;
    uint32_t actual_height = 0;
    uint32_t stride = 0;
    REQUIRE(sao_ui_offscreen_raster_snapshot(raster, nullptr, 0, &bytes, &actual_width,
                                             &actual_height,
                                             &stride) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<Pixel> pixels(bytes / sizeof(Pixel));
    REQUIRE(sao_ui_offscreen_raster_snapshot(raster, reinterpret_cast<uint8_t*>(pixels.data()),
                                             bytes, &bytes, &actual_width, &actual_height,
                                             &stride) == SAO_STATUS_OK);
    REQUIRE(actual_width == width);
    REQUIRE(actual_height == height);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    return pixels;
}

SaoUiProgressBarSpec base_spec(int32_t style, float value = 0.5f, float max_value = 1.0f) {
    SaoUiProgressBarSpec spec{};
    spec.value = value;
    spec.max_value = max_value;
    spec.style = style;
    spec.fill_argb = 0xFF00FF00;
    spec.radius_px = 0;
    spec.animate_duration_ms = 0;
    spec.trail_lag_ms = 280;
    return spec;
}

} // namespace

TEST_CASE("progress_flat_fill_ratio_clamps", "[ui][widget][data][runtime]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiProgressBarSpec spec = base_spec(SAO_UI_PROGRESS_FLAT, 0.75f, 1.0f);
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    float ratio = -1.0f;
    REQUIRE(sao_ui_widget_progress_get_fill_ratio(h, &ratio) == SAO_STATUS_OK);
    REQUIRE(std::fabs(ratio - 0.75f) < 1e-5f);
    // Overshoot clamps to 1.
    REQUIRE(sao_ui_progress_bar_set_value(h, 3.0f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_fill_ratio(h, &ratio) == SAO_STATUS_OK);
    REQUIRE(ratio == 1.0f);
    // Undershoot clamps to 0.
    REQUIRE(sao_ui_progress_bar_set_value(h, -5.0f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_fill_ratio(h, &ratio) == SAO_STATUS_OK);
    REQUIRE(ratio == 0.0f);
    // Max = 0 → 0 (no div by zero).
    REQUIRE(sao_ui_progress_bar_set_max(h, 0.0f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_fill_ratio(h, &ratio) == SAO_STATUS_OK);
    REQUIRE(ratio == 0.0f);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("progress_hp_ramp_interpolates_red_yellow_green_stops", "[ui][widget][data][runtime]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiProgressBarSpec spec = base_spec(SAO_UI_PROGRESS_HP_RAMP, 0.9f, 1.0f);
    spec.fill_low_argb = 0xFFFF0000;  // red
    spec.fill_mid_argb = 0xFFFFFF00;  // yellow
    spec.fill_high_argb = 0xFF00FF00; // green
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    uint32_t argb = 0;
    REQUIRE(sao_ui_widget_progress_resolve_fill_argb(h, &argb) == SAO_STATUS_OK);
    REQUIRE(argb == 0xFF00FF00); // > 50% → green
    REQUIRE(sao_ui_progress_bar_set_value(h, 0.375f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_resolve_fill_argb(h, &argb) == SAO_STATUS_OK);
    REQUIRE(argb == 0xFF80FF00); // halfway yellow → green
    REQUIRE(sao_ui_progress_bar_set_value(h, 0.125f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_resolve_fill_argb(h, &argb) == SAO_STATUS_OK);
    REQUIRE(argb == 0xFFFF8000); // halfway red → yellow
    REQUIRE(sao_ui_progress_bar_set_value(h, 0.25f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_resolve_fill_argb(h, &argb) == SAO_STATUS_OK);
    REQUIRE(argb == 0xFFFFFF00);
    REQUIRE(sao_ui_progress_bar_set_value(h, 0.5f) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_resolve_fill_argb(h, &argb) == SAO_STATUS_OK);
    REQUIRE(argb == 0xFF00FF00);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("progress_hp_trail_lags_behind_displayed", "[ui][widget][data][runtime]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiProgressBarSpec spec = base_spec(SAO_UI_PROGRESS_HP_TRAIL, 1.0f, 1.0f);
    spec.trail_lag_ms = 280;
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    // Immediately drop to 0.5 — trail stays at 1.0 initially.
    REQUIRE(sao_ui_progress_bar_set_value(h, 0.5f) == SAO_STATUS_OK);
    float fill = 0.0f, trail = 0.0f;
    REQUIRE(sao_ui_widget_progress_get_fill_ratio(h, &fill) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_trail_ratio(h, &trail) == SAO_STATUS_OK);
    REQUIRE(std::fabs(fill - 0.5f) < 1e-5f);
    // trail == 1 immediately after the drop (no tick yet).
    REQUIRE(std::fabs(trail - 1.0f) < 1e-5f);
    // Tick 140 ms = half the lag → trail should reach mid-way.
    REQUIRE(sao_ui_widget_progress_tick(h, 140) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_trail_ratio(h, &trail) == SAO_STATUS_OK);
    // Linear decay: 1.0 - (0.5 × 140/280) = 0.75.
    REQUIRE(std::fabs(trail - 0.75f) < 1e-4f);
    // Full lag → trail catches up to fill.
    REQUIRE(sao_ui_widget_progress_tick(h, 500) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_trail_ratio(h, &trail) == SAO_STATUS_OK);
    REQUIRE(std::fabs(trail - 0.5f) < 1e-4f);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("progress_segments_stored_and_indexable", "[ui][widget][data][runtime]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiProgressSegment segs[3] = {
        {0.00f, 0.33f, 0xFF00FF00, 0},
        {0.33f, 0.66f, 0xFFFFFF00, 0},
        {0.66f, 1.00f, 0xFFFF0000, 0},
    };
    SaoUiProgressBarSpec spec = base_spec(SAO_UI_PROGRESS_SEGMENTS, 0.5f, 1.0f);
    spec.segments = segs;
    spec.segment_count = 3;
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_get_segment_count(h) == 3);
    // Retrieve the middle segment.
    SaoUiProgressSegment out{};
    REQUIRE(sao_ui_widget_progress_get_segment(h, 1, &out) == SAO_STATUS_OK);
    REQUIRE(out.fill_argb == 0xFFFFFF00);
    REQUIRE(std::fabs(out.fraction_start - 0.33f) < 1e-4f);
    REQUIRE(std::fabs(out.fraction_end - 0.66f) < 1e-4f);
    // Out-of-range index errors.
    REQUIRE(sao_ui_widget_progress_get_segment(h, 42, &out) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("progress_hp_trail_paints_a_separate_lagging_band", "[ui][widget][data][bar][paint]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiProgressBarSpec spec = base_spec(SAO_UI_PROGRESS_HP_TRAIL, 1.0F, 1.0F);
    spec.bg_argb = 0xff000000U;
    spec.fill_argb = 0xff00ff00U;
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_progress_bar_set_value(h, 0.5F) == SAO_STATUS_OK);

    auto pixels = paint_snapshot(h, 100, 8);
    const Pixel fill = pixels[4U * 100U + 25U];
    const Pixel trail = pixels[4U * 100U + 75U];
    CHECK(fill.g == 255U);
    CHECK(trail.a == 255U);
    CHECK((trail.r != 0U || trail.g != 0U || trail.b != 0U));
    CHECK((trail.r != fill.r || trail.g != fill.g || trail.b != fill.b));

    REQUIRE(sao_ui_widget_progress_tick(h, 500) == SAO_STATUS_OK);
    pixels = paint_snapshot(h, 100, 8);
    const Pixel caught_up = pixels[4U * 100U + 75U];
    CHECK(caught_up.r == 0U);
    CHECK(caught_up.g == 0U);
    CHECK(caught_up.b == 0U);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("progress_segments_pulse_changed_segment_alpha",
          "[ui][widget][data][bar][segments][paint]") {
    SaoUiProgressSegment segments[2] = {
        {0.0F, 0.5F, 0xffff0000U, 0},
        {0.5F, 1.0F, 0xff0000ffU, 0},
    };
    SaoUiProgressBarSpec spec = base_spec(SAO_UI_PROGRESS_SEGMENTS, 0.0F, 1.0F);
    spec.bg_argb = 0xff000000U;
    spec.segments = segments;
    spec.segment_count = 2;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_progress_bar_set_value(h, 1.0F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_progress_tick(h, 90) == SAO_STATUS_OK);

    auto pixels = paint_snapshot(h, 100, 8);
    const Pixel pulsing_red = pixels[4U * 100U + 25U];
    const Pixel pulsing_blue = pixels[4U * 100U + 75U];
    CHECK(pulsing_red.r > 100U);
    CHECK(pulsing_red.r < 255U);
    CHECK(pulsing_blue.b > 100U);
    CHECK(pulsing_blue.b < 255U);
    REQUIRE(sao_ui_widget_progress_tick(h, 90) == SAO_STATUS_OK);
    pixels = paint_snapshot(h, 100, 8);
    CHECK(pixels[4U * 100U + 25U].r == 255U);
    CHECK(pixels[4U * 100U + 75U].b == 255U);
    sao_ui_widget_data_family_destroy(h);
}
