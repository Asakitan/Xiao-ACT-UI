// SAO Auto — chart widgets first slice (Wave 4 / Agent d, G3.8).
//
// This slice implements the TimeSeriesChart portion of widget_chart.h:
//   * sao_ui_time_series_chart_create / _set_lanes / _append / _set_zoom
//
// Storage model: each lane owns a ring buffer sized by spec.max_visible_points.
// Once the ring fills up further append() calls overwrite the oldest
// slot in-place; that matches the sao_gui_graph_timeseries.py rolling
// window semantics (memory: 4-metric graph with a sliding window).
//
// Bar / line chart stubs return NOT_IMPLEMENTED — later slice.
//
// UTF-8 no BOM.

#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_AXIS_LINEAR       == 0, "axis scale enum drifted");
static_assert(SAO_UI_AXIS_TIME         == 2, "axis scale enum drifted");
static_assert(SAO_UI_AXIS_TIME_CLOCK   == 0, "axis time fmt enum drifted");
static_assert(SAO_UI_AXIS_TIME_RELATIVE== 2, "axis time fmt enum drifted");

namespace {

constexpr int32_t kTimeSeriesTag = 150;   // aligned with SAO_UI_WIDGET_TIME_SERIES_CHART
constexpr int32_t kBarChartTag = 151;
constexpr int32_t kLineChartTag = 152;
constexpr int32_t kSparklineTag = 153;
constexpr int32_t kDefaultCapacity = 1024;

struct TimeSeriesLane {
    std::string                 lane_id;
    std::string                 label;
    uint32_t                    fill_argb{0};
    uint32_t                    area_fill_argb{0};
    float                       line_width_px{1.5f};
    bool                        show_points{false};
    bool                        interpolate{true};
    double                      peak_hint{0.0};

    // Ring buffer of samples.
    std::vector<SaoUiTimePoint> ring;
    size_t                      capacity{static_cast<size_t>(kDefaultCapacity)};
    size_t                      count{0};       // valid points
    size_t                      head{0};        // next slot to write
};

struct TimeSeriesState {
    int32_t                     tag{kTimeSeriesTag};
    SaoUiTimeSeriesSpec         spec{};
    std::vector<TimeSeriesLane> lanes;
    // Overrides the spec's implicit capacity; 0 → use spec.max_visible_points
    // (or kDefaultCapacity as last-resort).
    int32_t                     capacity_override{0};
    int64_t                     zoom_window_ms{0};   // 0 → autoscale
    mutable std::mutex          mtx;
};

struct OwnedBar {
    std::string label;
    double value{0.0};
    uint32_t fill_argb{0};
    uint32_t border_argb{0};
    double overlay_value{0.0};
    uint32_t overlay_fill_argb{0};
};

struct BarChartState {
    int32_t tag{kBarChartTag};
    SaoUiBarChartSpec spec{};
    std::vector<OwnedBar> bars;
    mutable std::mutex mtx;
};

struct OwnedLineSeries {
    std::string series_id;
    std::string label;
    std::vector<SaoUiLinePoint> points;
    uint32_t line_argb{0};
    uint32_t marker_argb{0};
    float line_width_px{1.0f};
    bool dashed{false};
    bool show_markers{false};
    double threshold_y{std::numeric_limits<double>::quiet_NaN()};
    uint32_t threshold_argb{0};
};

struct LineChartState {
    int32_t tag{kLineChartTag};
    SaoUiLineChartSpec spec{};
    std::vector<OwnedLineSeries> series;
    mutable std::mutex mtx;
};

struct SparklineState {
    int32_t tag{kSparklineTag};
    SaoUiSparklineSpec spec{};
    std::vector<double> values;
    mutable std::mutex mtx;
};

int32_t peek_tag(sao_ui_widget_handle_t h) {
    if (h == nullptr) return -1;
    return *reinterpret_cast<const int32_t*>(h);
}

TimeSeriesState* as_timeseries(sao_ui_widget_handle_t h) {
    if (peek_tag(h) != kTimeSeriesTag) return nullptr;
    return reinterpret_cast<TimeSeriesState*>(h);
}

BarChartState* as_bar_chart(sao_ui_widget_handle_t h) {
    return peek_tag(h) == kBarChartTag ? reinterpret_cast<BarChartState*>(h) : nullptr;
}

LineChartState* as_line_chart(sao_ui_widget_handle_t h) {
    return peek_tag(h) == kLineChartTag ? reinterpret_cast<LineChartState*>(h) : nullptr;
}

SparklineState* as_sparkline(sao_ui_widget_handle_t h) {
    return peek_tag(h) == kSparklineTag ? reinterpret_cast<SparklineState*>(h) : nullptr;
}

void set_bars_no_lock(BarChartState& state, const SaoUiBarChartBar* bars, size_t count) {
    state.bars.clear();
    state.bars.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        state.bars.push_back({
            bars[index].label_utf8 == nullptr ? "" : bars[index].label_utf8,
            bars[index].value,
            bars[index].fill_argb,
            bars[index].border_argb,
            bars[index].overlay_value,
            bars[index].overlay_fill_argb,
        });
    }
    if (state.spec.sort_desc) {
        std::stable_sort(state.bars.begin(), state.bars.end(),
            [](const OwnedBar& left, const OwnedBar& right) {
                return left.value > right.value;
            });
    }
}

void set_series_no_lock(
    LineChartState& state, const SaoUiLineChartSeries* series, size_t count) {
    state.series.clear();
    state.series.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        OwnedLineSeries owned{};
        owned.series_id = series[index].series_id_utf8 == nullptr ? "" : series[index].series_id_utf8;
        owned.label = series[index].label_utf8 == nullptr ? "" : series[index].label_utf8;
        if (series[index].points != nullptr && series[index].point_count != 0) {
            owned.points.assign(series[index].points, series[index].points + series[index].point_count);
        }
        owned.line_argb = series[index].line_argb;
        owned.marker_argb = series[index].marker_argb;
        owned.line_width_px = series[index].line_width_px;
        owned.dashed = series[index].dashed;
        owned.show_markers = series[index].show_markers;
        owned.threshold_y = series[index].threshold_y;
        owned.threshold_argb = series[index].threshold_argb;
        state.series.push_back(std::move(owned));
    }
}

void set_sparkline_values_no_lock(
    SparklineState& state, const double* values, size_t count) {
    if (values == nullptr || count == 0) {
        state.values.clear();
        return;
    }
    const size_t capacity = state.spec.max_points == 0 ? 64 : state.spec.max_points;
    const size_t start = count > capacity ? count - capacity : 0;
    state.values.assign(values + start, values + count);
}

size_t resolve_capacity(const TimeSeriesState& s) {
    int32_t cap = s.capacity_override;
    if (cap <= 0) cap = s.spec.max_visible_points;
    if (cap <= 0) cap = kDefaultCapacity;
    return static_cast<size_t>(cap);
}

void configure_lane(TimeSeriesLane& lane,
                     const SaoUiTimeSeriesLane& src,
                     size_t capacity) {
    lane.lane_id       = src.lane_id_utf8 ? src.lane_id_utf8 : "";
    lane.label         = src.label_utf8   ? src.label_utf8   : "";
    lane.fill_argb     = src.fill_argb;
    lane.area_fill_argb = src.area_fill_argb;
    lane.line_width_px = src.line_width_px;
    lane.show_points   = src.show_points;
    lane.interpolate   = src.interpolate;
    lane.peak_hint     = src.peak_hint;
    lane.capacity      = capacity;
    lane.ring.assign(capacity, SaoUiTimePoint{0, 0.0});
    lane.count = 0;
    lane.head  = 0;
    if (src.points && src.point_count > 0) {
        const size_t start = src.point_count > capacity
                                ? (src.point_count - capacity) : 0;
        for (size_t i = start; i < src.point_count; ++i) {
            lane.ring[lane.head] = src.points[i];
            lane.head = (lane.head + 1) % lane.capacity;
            if (lane.count < lane.capacity) ++lane.count;
        }
    }
}

TimeSeriesLane* find_lane_locked(TimeSeriesState& s, const char* lane_id_utf8) {
    if (lane_id_utf8 == nullptr) return nullptr;
    for (auto& l : s.lanes) {
        if (l.lane_id == lane_id_utf8) return &l;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Time-series ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_create(
    void* /*d3d_device_ptr*/,
    const SaoUiTimeSeriesSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* s = new TimeSeriesState();
    s->spec = *spec;
    // spec.lanes is borrowed; we own the state via s->lanes.
    s->spec.lanes = nullptr;
    s->spec.lane_count = 0;
    s->capacity_override = 0;
    const size_t cap = resolve_capacity(*s);
    s->lanes.reserve(spec->lane_count);
    for (size_t i = 0; i < spec->lane_count; ++i) {
        TimeSeriesLane lane;
        configure_lane(lane, spec->lanes[i], cap);
        s->lanes.push_back(std::move(lane));
    }
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_set_lanes(
    sao_ui_widget_handle_t handle,
    const SaoUiTimeSeriesLane* lanes,
    size_t lane_count) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (lanes == nullptr && lane_count > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    const size_t cap = resolve_capacity(*s);
    s->lanes.clear();
    s->lanes.reserve(lane_count);
    for (size_t i = 0; i < lane_count; ++i) {
        TimeSeriesLane lane;
        configure_lane(lane, lanes[i], cap);
        s->lanes.push_back(std::move(lane));
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_append(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    const SaoUiTimePoint* points,
    size_t point_count) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (lane_id_utf8 == nullptr || (points == nullptr && point_count > 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    TimeSeriesLane* lane = find_lane_locked(*s, lane_id_utf8);
    if (lane == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    for (size_t i = 0; i < point_count; ++i) {
        lane->ring[lane->head] = points[i];
        lane->head = (lane->head + 1) % lane->capacity;
        if (lane->count < lane->capacity) ++lane->count;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_set_zoom(
    sao_ui_widget_handle_t handle,
    int64_t window_ms) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->zoom_window_ms = window_ms;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_create(
    void*, const SaoUiBarChartSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec->bars == nullptr && spec->bar_count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) BarChartState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->spec.bars = nullptr;
    state->spec.bar_count = 0;
    set_bars_no_lock(*state, spec->bars, spec->bar_count);
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_set_bars(
    sao_ui_widget_handle_t handle, const SaoUiBarChartBar* bars, size_t count) {
    BarChartState* state = as_bar_chart(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (bars == nullptr && count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    set_bars_no_lock(*state, bars, count);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar_count(
    sao_ui_widget_handle_t handle, size_t* out_count, size_t* out_hidden_count) {
    BarChartState* state = as_bar_chart(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_count == nullptr || out_hidden_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    const size_t limit = state->spec.max_visible_bars <= 0
        ? state->bars.size()
        : static_cast<size_t>(state->spec.max_visible_bars);
    *out_count = std::min(limit, state->bars.size());
    *out_hidden_count = state->bars.size() - *out_count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar(
    sao_ui_widget_handle_t handle, size_t index, SaoUiBarChartBar* out_bar) {
    BarChartState* state = as_bar_chart(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_bar == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    const size_t limit = state->spec.max_visible_bars <= 0
        ? state->bars.size()
        : std::min(state->bars.size(), static_cast<size_t>(state->spec.max_visible_bars));
    if (index >= limit) return SAO_STATUS_ERR_NOT_FOUND;
    const OwnedBar& bar = state->bars[index];
    *out_bar = {bar.label.c_str(), bar.value, bar.fill_argb, bar.border_argb,
                bar.overlay_value, bar.overlay_fill_argb};
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_create(
    void*, const SaoUiLineChartSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec->series == nullptr && spec->series_count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) LineChartState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->spec.series = nullptr;
    state->spec.series_count = 0;
    set_series_no_lock(*state, spec->series, spec->series_count);
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_set_series(
    sao_ui_widget_handle_t handle, const SaoUiLineChartSeries* series, size_t count) {
    LineChartState* state = as_line_chart(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (series == nullptr && count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    set_series_no_lock(*state, series, count);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_get_series_count(
    sao_ui_widget_handle_t handle, size_t* out_count) {
    LineChartState* state = as_line_chart(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_count = state->series.size();
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_get_point(
    sao_ui_widget_handle_t handle, size_t series_index,
    size_t point_index, SaoUiLinePoint* out_point) {
    LineChartState* state = as_line_chart(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_point == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    if (series_index >= state->series.size() ||
        point_index >= state->series[series_index].points.size()) {
        return SAO_STATUS_ERR_NOT_FOUND;
    }
    *out_point = state->series[series_index].points[point_index];
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_create(
    void*, const SaoUiSparklineSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (spec->values == nullptr && spec->value_count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) SparklineState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->spec.values = nullptr;
    state->spec.value_count = 0;
    set_sparkline_values_no_lock(*state, spec->values, spec->value_count);
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_set_values(
    sao_ui_widget_handle_t handle, const double* values, size_t count) {
    SparklineState* state = as_sparkline(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (values == nullptr && count != 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    set_sparkline_values_no_lock(*state, values, count);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_append(
    sao_ui_widget_handle_t handle, double value) {
    SparklineState* state = as_sparkline(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(value)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    const size_t capacity = state->spec.max_points == 0 ? 64 : state->spec.max_points;
    if (state->values.size() == capacity) state->values.erase(state->values.begin());
    state->values.push_back(value);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_get_range(
    sao_ui_widget_handle_t handle, double* out_min, double* out_max, size_t* out_count) {
    SparklineState* state = as_sparkline(handle);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_min == nullptr || out_max == nullptr || out_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_count = state->values.size();
    if (state->values.empty()) {
        *out_min = 0.0;
        *out_max = 0.0;
        return SAO_STATUS_OK;
    }
    const auto range = std::minmax_element(state->values.begin(), state->values.end());
    *out_min = *range.first;
    *out_max = *range.second;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Wave 4 helper API — sample fetch, clear, axis auto-compute.
// ---------------------------------------------------------------------------

// Preferred ring capacity setter (0 → use spec.max_visible_points).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_set_capacity(
    sao_ui_widget_handle_t handle,
    int32_t capacity) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (capacity < 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->capacity_override = capacity;
    return SAO_STATUS_OK;
}

// Number of samples currently stored in a lane's ring.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_get_sample_count(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    size_t* out_count) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    TimeSeriesLane* lane = find_lane_locked(*s, lane_id_utf8);
    if (lane == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    if (out_count) *out_count = lane->count;
    return SAO_STATUS_OK;
}

// Push a single sample (convenience wrapper over append).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_push_sample(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    int64_t timestamp_ms,
    double value) {
    const SaoUiTimePoint pt{timestamp_ms, value};
    return sao_ui_time_series_chart_append(handle, lane_id_utf8, &pt, 1);
}

// Copy up to `capacity` samples in chronological order into `samples_out`.
// If capacity is smaller than count, we return the tail (most recent
// `capacity` points); if larger, we return count points and set
// `count_out = count`.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_get_samples(
    sao_ui_widget_handle_t handle,
    const char* lane_id_utf8,
    SaoUiTimePoint* samples_out,
    size_t capacity,
    size_t* count_out) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (samples_out == nullptr && capacity > 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    TimeSeriesLane* lane = find_lane_locked(*s, lane_id_utf8);
    if (lane == nullptr) return SAO_STATUS_ERR_NOT_FOUND;
    const size_t to_copy = std::min(capacity, lane->count);
    // Chronological order: oldest sample sits at
    // (head - count + lane.capacity) % lane.capacity.
    const size_t start = (lane->head + lane->capacity - lane->count)
                          % lane->capacity;
    const size_t skip = lane->count - to_copy;  // drop oldest N when tight
    for (size_t i = 0; i < to_copy; ++i) {
        const size_t src_idx = (start + skip + i) % lane->capacity;
        samples_out[i] = lane->ring[src_idx];
    }
    if (count_out) *count_out = to_copy;
    return SAO_STATUS_OK;
}

// Reset all lanes to empty rings (retains lane configuration).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_clear(sao_ui_widget_handle_t handle) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    for (auto& lane : s->lanes) {
        std::fill(lane.ring.begin(), lane.ring.end(),
                  SaoUiTimePoint{0, 0.0});
        lane.count = 0;
        lane.head  = 0;
    }
    return SAO_STATUS_OK;
}

// Compute axis min/max across all lanes (respects zoom window).
struct SaoUiTimeSeriesAxis {
    int64_t x_min_ms;
    int64_t x_max_ms;
    double  y_min;
    double  y_max;
    int32_t tick_count;
    uint8_t _pad[4];
};

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_compute_axis(
    sao_ui_widget_handle_t handle,
    SaoUiTimeSeriesAxis* out_axis) {
    TimeSeriesState* s = as_timeseries(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_axis == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    int64_t x_min = std::numeric_limits<int64_t>::max();
    int64_t x_max = std::numeric_limits<int64_t>::min();
    double  y_min = std::numeric_limits<double>::infinity();
    double  y_max = -std::numeric_limits<double>::infinity();
    bool any = false;
    for (auto& lane : s->lanes) {
        if (lane.count == 0) continue;
        const size_t start = (lane.head + lane.capacity - lane.count)
                              % lane.capacity;
        for (size_t i = 0; i < lane.count; ++i) {
            const auto& p = lane.ring[(start + i) % lane.capacity];
            if (p.time_ms < x_min) x_min = p.time_ms;
            if (p.time_ms > x_max) x_max = p.time_ms;
            if (p.value   < y_min) y_min = p.value;
            if (p.value   > y_max) y_max = p.value;
            any = true;
        }
    }
    if (!any) {
        out_axis->x_min_ms   = 0;
        out_axis->x_max_ms   = 0;
        out_axis->y_min      = 0.0;
        out_axis->y_max      = 0.0;
        out_axis->tick_count = 0;
        return SAO_STATUS_OK;
    }
    // Apply zoom-window: shrink x_min if we have more history than the
    // caller wants shown.
    if (s->zoom_window_ms > 0 && (x_max - x_min) > s->zoom_window_ms) {
        x_min = x_max - s->zoom_window_ms;
    }
    // Tick count: use spec.desired_tick_count (x-axis) or a sensible default.
    int32_t ticks = s->spec.x_axis.desired_tick_count;
    if (ticks <= 0) ticks = 5;
    out_axis->x_min_ms   = x_min;
    out_axis->x_max_ms   = x_max;
    out_axis->y_min      = y_min;
    out_axis->y_max      = y_max;
    out_axis->tick_count = ticks;
    return SAO_STATUS_OK;
}

// Shared destroy helper.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_chart_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    uint32_t removed = 0;
    (void)sao_ui_widget_release_event_handlers(handle, &removed);
    if (peek_tag(handle) == kTimeSeriesTag) {
        delete reinterpret_cast<TimeSeriesState*>(handle);
    } else if (peek_tag(handle) == kBarChartTag) {
        delete reinterpret_cast<BarChartState*>(handle);
    } else if (peek_tag(handle) == kLineChartTag) {
        delete reinterpret_cast<LineChartState*>(handle);
    } else if (peek_tag(handle) == kSparklineTag) {
        delete reinterpret_cast<SparklineState*>(handle);
    }
}
