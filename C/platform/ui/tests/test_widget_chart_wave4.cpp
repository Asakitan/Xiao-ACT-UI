// Wave 4 tests for the TimeSeriesChart widget (G3.8 first slice).
//
// Coverage (4 CASE):
//   * timeseries_push_and_get_returns_chronological
//   * timeseries_ring_buffer_wraps_after_capacity
//   * timeseries_clear_resets_all_lanes
//   * timeseries_compute_axis_returns_min_max
//
// All tests are pure state — no D3D device.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_chart.h"

extern "C" {

struct SaoUiTimeSeriesAxis {
    int64_t x_min_ms;
    int64_t x_max_ms;
    double  y_min;
    double  y_max;
    int32_t tick_count;
    uint8_t _pad[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_set_capacity(
    sao_ui_widget_handle_t handle,
    int32_t capacity);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_get_sample_count(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    size_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_push_sample(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    int64_t timestamp_ms,
    double value);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_get_samples(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    SaoUiTimePoint* samples_out,
    size_t capacity,
    size_t* count_out);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_clear(
    sao_ui_widget_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_compute_axis(
    sao_ui_widget_handle_t handle,
    SaoUiTimeSeriesAxis* out_axis);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_chart_family_destroy(
    sao_ui_widget_handle_t handle);

}  // extern "C"

namespace {

sao_ui_widget_handle_t make_chart_with_single_lane(int32_t capacity) {
    SaoUiTimeSeriesLane lane{};
    lane.lane_id_utf8 = "damage";
    lane.label_utf8   = "Damage";
    lane.points       = nullptr;
    lane.point_count  = 0;
    lane.fill_argb    = 0xFFFFCC00;
    lane.line_width_px = 1.5f;
    lane.show_points   = false;
    lane.interpolate   = true;

    SaoUiTimeSeriesSpec spec{};
    spec.lanes      = &lane;
    spec.lane_count = 1;
    spec.x_axis.scale = SAO_UI_AXIS_TIME;
    spec.x_axis.time_fmt = SAO_UI_AXIS_TIME_DURATION;
    spec.x_axis.desired_tick_count = 5;
    spec.y_axis.scale = SAO_UI_AXIS_LINEAR;
    spec.max_visible_points = capacity;

    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_time_series_chart_create(nullptr, &spec, &h)
            == SAO_STATUS_OK);
    return h;
}

}  // namespace

TEST_CASE("timeseries_push_and_get_returns_chronological",
          "[ui][widget][chart][wave4]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(64);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(
            h, "damage", 1000 + i * 100, static_cast<double>(i))
            == SAO_STATUS_OK);
    }
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count)
            == SAO_STATUS_OK);
    REQUIRE(count == 5);
    SaoUiTimePoint buf[10]{};
    size_t written = 0;
    REQUIRE(sao_ui_widget_timeseries_get_samples(h, "damage", buf, 10, &written)
            == SAO_STATUS_OK);
    REQUIRE(written == 5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(buf[i].time_ms == 1000 + i * 100);
        REQUIRE(std::fabs(buf[i].value - static_cast<double>(i)) < 1e-9);
    }
    sao_ui_widget_chart_family_destroy(h);
}

TEST_CASE("timeseries_ring_buffer_wraps_after_capacity",
          "[ui][widget][chart][wave4]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(4);
    // Push 6 → ring holds only the last 4.
    for (int i = 0; i < 6; ++i) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(
            h, "damage", 1000 + i, static_cast<double>(i))
            == SAO_STATUS_OK);
    }
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count)
            == SAO_STATUS_OK);
    REQUIRE(count == 4);
    SaoUiTimePoint buf[8]{};
    size_t written = 0;
    REQUIRE(sao_ui_widget_timeseries_get_samples(h, "damage", buf, 8, &written)
            == SAO_STATUS_OK);
    REQUIRE(written == 4);
    // Should be samples 2, 3, 4, 5 in chronological order.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(buf[i].time_ms == 1000 + i + 2);
        REQUIRE(std::fabs(buf[i].value - static_cast<double>(i + 2)) < 1e-9);
    }
    sao_ui_widget_chart_family_destroy(h);
}

TEST_CASE("timeseries_clear_resets_all_lanes",
          "[ui][widget][chart][wave4]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(16);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(
            h, "damage", 1000 + i, static_cast<double>(i))
            == SAO_STATUS_OK);
    }
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count)
            == SAO_STATUS_OK);
    REQUIRE(count == 8);
    REQUIRE(sao_ui_widget_timeseries_clear(h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count)
            == SAO_STATUS_OK);
    REQUIRE(count == 0);
    // After clear, further pushes still work.
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 5000, 42.0)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count)
            == SAO_STATUS_OK);
    REQUIRE(count == 1);
    sao_ui_widget_chart_family_destroy(h);
}

TEST_CASE("timeseries_compute_axis_returns_min_max",
          "[ui][widget][chart][wave4]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(16);
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1000, 5.0)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1050, -3.0)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1200, 42.0)
            == SAO_STATUS_OK);
    SaoUiTimeSeriesAxis axis{};
    REQUIRE(sao_ui_widget_timeseries_compute_axis(h, &axis) == SAO_STATUS_OK);
    REQUIRE(axis.x_min_ms == 1000);
    REQUIRE(axis.x_max_ms == 1200);
    REQUIRE(std::fabs(axis.y_min + 3.0) < 1e-6);
    REQUIRE(std::fabs(axis.y_max - 42.0) < 1e-6);
    REQUIRE(axis.tick_count == 5);
    // Empty chart → all zeros.
    REQUIRE(sao_ui_widget_timeseries_clear(h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_compute_axis(h, &axis) == SAO_STATUS_OK);
    REQUIRE(axis.x_min_ms == 0);
    REQUIRE(axis.x_max_ms == 0);
    REQUIRE(axis.tick_count == 0);
    sao_ui_widget_chart_family_destroy(h);
}
