// TimeSeriesChart ring-buffer and axis tests.
//
// Coverage (4 CASE):
//   * timeseries_push_and_get_returns_chronological
//   * timeseries_ring_buffer_wraps_after_capacity
//   * timeseries_clear_resets_all_lanes
//   * timeseries_compute_axis_returns_min_max
//
// All tests are pure state — no D3D device.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_chart.h"

extern "C" {
SAO_UI_API void SAO_UI_CALL sao_ui_widget_chart_family_destroy(sao_ui_widget_handle_t handle);

} // extern "C"

namespace {

sao_ui_widget_handle_t make_chart_with_single_lane(int32_t capacity) {
    SaoUiTimeSeriesLane lane{};
    lane.lane_id_utf8 = "damage";
    lane.label_utf8 = "Damage";
    lane.points = nullptr;
    lane.point_count = 0;
    lane.fill_argb = 0xFFFFCC00;
    lane.line_width_px = 1.5f;
    lane.show_points = false;
    lane.interpolate = true;

    SaoUiTimeSeriesSpec spec{};
    spec.lanes = &lane;
    spec.lane_count = 1;
    spec.x_axis.scale = SAO_UI_AXIS_TIME;
    spec.x_axis.time_fmt = SAO_UI_AXIS_TIME_DURATION;
    spec.x_axis.desired_tick_count = 5;
    spec.y_axis.scale = SAO_UI_AXIS_LINEAR;
    spec.max_visible_points = capacity;

    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_time_series_chart_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    return h;
}

} // namespace

TEST_CASE("timeseries_push_and_get_returns_chronological", "[ui][widget][chart][runtime]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(64);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1000 + i * 100,
                                                     static_cast<double>(i)) == SAO_STATUS_OK);
    }
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 5);
    SaoUiTimePoint buf[10]{};
    size_t written = 0;
    REQUIRE(sao_ui_widget_timeseries_get_samples(h, "damage", buf, 10, &written) == SAO_STATUS_OK);
    REQUIRE(written == 5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(buf[i].time_ms == 1000 + i * 100);
        REQUIRE(std::fabs(buf[i].value - static_cast<double>(i)) < 1e-9);
    }
    sao_ui_widget_chart_family_destroy(h);
}

TEST_CASE("timeseries_ring_buffer_wraps_after_capacity", "[ui][widget][chart][runtime]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(4);
    // Push 6 → ring holds only the last 4.
    for (int i = 0; i < 6; ++i) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1000 + i,
                                                     static_cast<double>(i)) == SAO_STATUS_OK);
    }
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 4);
    SaoUiTimePoint buf[8]{};
    size_t written = 0;
    REQUIRE(sao_ui_widget_timeseries_get_samples(h, "damage", buf, 8, &written) == SAO_STATUS_OK);
    REQUIRE(written == 4);
    // Should be samples 2, 3, 4, 5 in chronological order.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(buf[i].time_ms == 1000 + i + 2);
        REQUIRE(std::fabs(buf[i].value - static_cast<double>(i + 2)) < 1e-9);
    }
    sao_ui_widget_chart_family_destroy(h);
}

TEST_CASE("timeseries_clear_resets_all_lanes", "[ui][widget][chart][runtime]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(16);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1000 + i,
                                                     static_cast<double>(i)) == SAO_STATUS_OK);
    }
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 8);
    REQUIRE(sao_ui_widget_timeseries_clear(h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);
    // After clear, further pushes still work.
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 5000, 42.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(h, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    sao_ui_widget_chart_family_destroy(h);
}

TEST_CASE("timeseries_compute_axis_returns_min_max", "[ui][widget][chart][runtime]") {
    sao_ui_widget_handle_t h = make_chart_with_single_lane(16);
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1000, 5.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1050, -3.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(h, "damage", 1200, 42.0) == SAO_STATUS_OK);
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

TEST_CASE("portable_timeseries_rejects_null_lanes_and_null_lane_points",
          "[ui][widget][chart][portable]") {
    SaoUiTimeSeriesSpec spec{};
    spec.lane_count = 1;
    spec.max_visible_points = 8;
    sao_ui_widget_handle_t handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_time_series_chart_create(nullptr, &spec, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    SaoUiTimeSeriesLane lane{};
    lane.lane_id_utf8 = "damage";
    lane.line_width_px = 1.0F;
    lane.point_count = 1;
    spec.lanes = &lane;
    handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_time_series_chart_create(nullptr, &spec, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    lane.point_count = 0;
    REQUIRE(sao_ui_time_series_chart_create(nullptr, &spec, &handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(handle, "damage", 1, 1.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_time_series_chart_set_lanes(handle, nullptr, 1) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    size_t count = 0;
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(handle, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);

    lane.point_count = 1;
    REQUIRE(sao_ui_time_series_chart_set_lanes(handle, &lane, 1) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_widget_timeseries_get_sample_count(handle, "damage", &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    sao_ui_widget_chart_family_destroy(handle);
}

TEST_CASE("portable_timeseries_capacity_resize_preserves_newest_samples",
          "[ui][widget][chart][portable]") {
    sao_ui_widget_handle_t handle = make_chart_with_single_lane(4);
    for (int index = 0; index < 6; ++index) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(handle, "damage", 1000 + index,
                                                     static_cast<double>(index)) == SAO_STATUS_OK);
    }

    REQUIRE(sao_ui_widget_timeseries_set_capacity(handle, 2) == SAO_STATUS_OK);
    SaoUiTimePoint samples[8]{};
    size_t written = 0;
    REQUIRE(sao_ui_widget_timeseries_get_samples(handle, "damage", samples, 8, &written) ==
            SAO_STATUS_OK);
    REQUIRE(written == 2);
    REQUIRE(samples[0].value == 4.0);
    REQUIRE(samples[1].value == 5.0);

    REQUIRE(sao_ui_widget_timeseries_set_capacity(handle, 5) == SAO_STATUS_OK);
    for (int index = 6; index < 10; ++index) {
        REQUIRE(sao_ui_widget_timeseries_push_sample(handle, "damage", 1000 + index,
                                                     static_cast<double>(index)) == SAO_STATUS_OK);
    }
    REQUIRE(sao_ui_widget_timeseries_get_samples(handle, "damage", samples, 8, &written) ==
            SAO_STATUS_OK);
    REQUIRE(written == 5);
    for (int index = 0; index < 5; ++index) {
        REQUIRE(samples[index].value == static_cast<double>(index + 5));
    }

    REQUIRE(sao_ui_widget_timeseries_set_capacity(handle, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_get_samples(handle, "damage", samples, 8, &written) ==
            SAO_STATUS_OK);
    REQUIRE(written == 4);
    for (int index = 0; index < 4; ++index) {
        REQUIRE(samples[index].value == static_cast<double>(index + 6));
    }
    REQUIRE(sao_ui_widget_timeseries_set_capacity(handle, (1 << 20) + 1) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_widget_timeseries_get_samples(handle, "damage", samples, 8, &written) ==
            SAO_STATUS_OK);
    REQUIRE(written == 4);
    sao_ui_widget_chart_family_destroy(handle);
}

TEST_CASE("portable_timeseries_zoom_recomputes_y_for_visible_window",
          "[ui][widget][chart][hardening]") {
    sao_ui_widget_handle_t handle = make_chart_with_single_lane(16);
    REQUIRE(sao_ui_widget_timeseries_push_sample(handle, "damage", 0, -100.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(handle, "damage", 100, 10.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_timeseries_push_sample(handle, "damage", 200, 20.0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_time_series_chart_set_zoom(handle, 100) == SAO_STATUS_OK);

    SaoUiTimeSeriesAxis axis{};
    REQUIRE(sao_ui_widget_timeseries_compute_axis(handle, &axis) == SAO_STATUS_OK);
    REQUIRE(axis.x_min_ms == 100);
    REQUIRE(axis.x_max_ms == 200);
    REQUIRE(axis.y_min == 10.0);
    REQUIRE(axis.y_max == 20.0);
    sao_ui_widget_chart_family_destroy(handle);
}

TEST_CASE("portable_bar_label_reads_are_owned_snapshots", "[ui][widget][chart][hardening]") {
    std::string original = "original-label";
    SaoUiBarChartBar first_bar{original.c_str(), 5.0, 1, 2, 1.0, 3};
    SaoUiBarChartSpec spec{};
    spec.bars = &first_bar;
    spec.bar_count = 1;
    sao_ui_widget_handle_t handle = nullptr;
    REQUIRE(sao_ui_bar_chart_create(nullptr, &spec, &handle) == SAO_STATUS_OK);

    SaoUiBarChartBar snapshot{};
    REQUIRE(sao_ui_bar_chart_get_bar(handle, 0, &snapshot) == SAO_STATUS_OK);
    const char* legacy_snapshot = snapshot.label_utf8;
    REQUIRE(std::string(legacy_snapshot) == "original-label");

    SaoUiBarChartBar replacement{"replacement", 8.0, 4, 5, 2.0, 6};
    REQUIRE(sao_ui_bar_chart_set_bars(handle, &replacement, 1) == SAO_STATUS_OK);
    REQUIRE(std::string(legacy_snapshot) == "original-label");

    size_t required = 0;
    SaoUiBarChartBar copied{};
    REQUIRE(sao_ui_bar_chart_get_bar_copy(handle, 0, &copied, nullptr, 0, &required) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(required == std::strlen("replacement") + 1);
    std::vector<char> label(required);
    REQUIRE(sao_ui_bar_chart_get_bar_copy(handle, 0, &copied, label.data(), label.size(),
                                          &required) == SAO_STATUS_OK);
    REQUIRE(copied.label_utf8 == label.data());
    REQUIRE(std::string(copied.label_utf8) == "replacement");
    sao_ui_widget_chart_family_destroy(handle);
}

TEST_CASE("portable_chart_validation_rejects_nonfinite_and_oversized_data",
          "[ui][widget][chart][hardening]") {
    SaoUiBarChartBar invalid_bar{"bad", std::numeric_limits<double>::infinity(), 0, 0, 0.0, 0};
    SaoUiBarChartSpec bar_spec{};
    bar_spec.bars = &invalid_bar;
    bar_spec.bar_count = 1;
    sao_ui_widget_handle_t handle = reinterpret_cast<sao_ui_widget_handle_t>(uintptr_t{1});
    REQUIRE(sao_ui_bar_chart_create(nullptr, &bar_spec, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    SaoUiLinePoint invalid_point{0.0, std::numeric_limits<double>::quiet_NaN()};
    SaoUiLineChartSeries series{};
    series.points = &invalid_point;
    series.point_count = 1;
    series.line_width_px = 1.0F;
    SaoUiLineChartSpec line_spec{};
    line_spec.series = &series;
    line_spec.series_count = 1;
    REQUIRE(sao_ui_line_chart_create(nullptr, &line_spec, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    double invalid_value = std::numeric_limits<double>::infinity();
    SaoUiSparklineSpec sparkline{};
    sparkline.values = &invalid_value;
    sparkline.value_count = 1;
    sparkline.line_width_px = 1.0F;
    REQUIRE(sao_ui_sparkline_create(nullptr, &sparkline, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    SaoUiBarChartBar valid_bar{"one", 1.0, 0, 0, 0.0, 0};
    bar_spec.bars = &valid_bar;
    bar_spec.bar_count = (1U << 20) + 1;
    REQUIRE(sao_ui_bar_chart_create(nullptr, &bar_spec, &handle) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
}

TEST_CASE("portable_timeseries_capacity_operations_serialize_with_destroy",
          "[ui][widget][chart][hardening]") {
    sao_ui_widget_handle_t handle = make_chart_with_single_lane(64);
    size_t capacity = 0;
    REQUIRE(sao_ui_widget_timeseries_get_capacity(handle, &capacity) == SAO_STATUS_OK);
    REQUIRE(capacity == 64);

    std::atomic<bool> started{false};
    std::atomic<int> completed{0};
    std::atomic<int> unexpected{0};
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        for (int index = 0; index < 10000; ++index) {
            const sao_status_t push = sao_ui_widget_timeseries_push_sample(
                handle, "damage", index, static_cast<double>(index));
            if (push == SAO_STATUS_ERR_HANDLE_INVALID)
                break;
            if (push != SAO_STATUS_OK) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            const sao_status_t resize =
                sao_ui_widget_timeseries_set_capacity(handle, 32 + (index % 8));
            if (resize == SAO_STATUS_ERR_HANDLE_INVALID)
                break;
            if (resize != SAO_STATUS_OK) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    sao_ui_widget_chart_family_destroy(handle);
    worker.join();
    REQUIRE(unexpected.load(std::memory_order_relaxed) == 0);
    REQUIRE(sao_ui_widget_timeseries_get_capacity(handle, &capacity) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_widget_chart_family_destroy(handle);
}
