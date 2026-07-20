// SAO Auto — time-series chart widgets and ring-buffer storage.
//
// This slice implements the TimeSeriesChart portion of widget_chart.h:
//   * sao_ui_time_series_chart_create / _set_lanes / _append / _set_zoom
//
// Storage model: each lane owns a ring buffer sized by spec.max_visible_points.
// Once the ring fills up further append() calls overwrite the oldest
// slot in-place; that matches the sao_gui_graph_timeseries.py rolling
// window semantics (memory: 4-metric graph with a sliding window).
//
// UTF-8 no BOM.

#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_AXIS_LINEAR == 0, "axis scale enum drifted");
static_assert(SAO_UI_AXIS_TIME == 2, "axis scale enum drifted");
static_assert(SAO_UI_AXIS_TIME_CLOCK == 0, "axis time fmt enum drifted");
static_assert(SAO_UI_AXIS_TIME_RELATIVE == 2, "axis time fmt enum drifted");

namespace {

constexpr int32_t kTimeSeriesTag = 150; // aligned with SAO_UI_WIDGET_TIME_SERIES_CHART
constexpr int32_t kBarChartTag = 151;
constexpr int32_t kLineChartTag = 152;
constexpr int32_t kSparklineTag = 153;
constexpr int32_t kDefaultCapacity = 1024;
constexpr size_t kMaxTimeSeriesCapacity = 1U << 20;
constexpr size_t kMaxTimeSeriesLanes = 4096;
constexpr size_t kMaxChartSeries = 4096;
constexpr size_t kMaxChartItems = 1U << 20;
constexpr int32_t kMaxAxisTicks = 1000;

struct TimeSeriesLane {
    std::string lane_id;
    std::string label;
    uint32_t fill_argb{0};
    uint32_t area_fill_argb{0};
    float line_width_px{1.5f};
    bool show_points{false};
    bool interpolate{true};
    double peak_hint{0.0};

    // Ring buffer of samples.
    std::vector<SaoUiTimePoint> ring;
    size_t capacity{static_cast<size_t>(kDefaultCapacity)};
    size_t count{0}; // valid points
    size_t head{0};  // next slot to write
};

struct TimeSeriesState {
    SaoUiTimeSeriesSpec spec{};
    std::string x_axis_title;
    std::string y_axis_title;
    std::vector<TimeSeriesLane> lanes;
    // Overrides the spec's implicit capacity; 0 → use spec.max_visible_points
    // (or kDefaultCapacity as last-resort).
    int32_t capacity_override{0};
    int64_t zoom_window_ms{0}; // 0 → autoscale
    mutable std::mutex mtx;
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
    SaoUiBarChartSpec spec{};
    std::string value_axis_title;
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
    SaoUiLineChartSpec spec{};
    std::string x_axis_title;
    std::string y_axis_title;
    std::vector<OwnedLineSeries> series;
    mutable std::mutex mtx;
};

struct SparklineState {
    SaoUiSparklineSpec spec{};
    std::vector<double> values;
    mutable std::mutex mtx;
};

struct ChartHandleShell {
    int32_t tag = -1;
    uint64_t generation = 0;
};

struct ChartHandleRecord {
    int32_t tag = -1;
    uint64_t generation = 0;
    std::shared_ptr<void> state;
    std::mutex lifecycle_mtx;
    std::condition_variable lifecycle_cv;
    size_t in_flight = 0;
    bool accepting = true;
    bool retired = false;
    bool finalized = false;
    bool finalization_started = false;
};

struct ChartHandleRegistry {
    std::mutex mtx;
    uint64_t next_generation = 1;
    std::unordered_map<sao_ui_widget_handle_t, std::shared_ptr<ChartHandleRecord>> active;
    std::vector<std::unique_ptr<ChartHandleShell>> shells;
};

ChartHandleRegistry& chart_handle_registry() {
    static ChartHandleRegistry* registry = new ChartHandleRegistry();
    return *registry;
}

void finalize_chart_record(const std::shared_ptr<ChartHandleRecord>& record) noexcept {
    record->state.reset();
    {
        std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
        record->finalized = true;
    }
    record->lifecycle_cv.notify_all();
}

template <typename State> class ChartLease final {
  public:
    ChartLease() = default;
    ChartLease(std::shared_ptr<ChartHandleRecord> record, std::shared_ptr<State> state)
        : record_(std::move(record)), state_(std::move(state)) {}

    ChartLease(ChartLease&&) noexcept = default;
    ChartLease& operator=(ChartLease&&) noexcept = delete;
    ChartLease(const ChartLease&) = delete;
    ChartLease& operator=(const ChartLease&) = delete;

    ~ChartLease() {
        release();
    }

    explicit operator bool() const noexcept {
        return state_ != nullptr;
    }

    State* operator->() const noexcept {
        return state_.get();
    }

  private:
    void release() noexcept {
        if (record_ == nullptr)
            return;
        bool finalize = false;
        {
            std::lock_guard<std::mutex> lifecycle_lock(record_->lifecycle_mtx);
            if (record_->in_flight > 0)
                --record_->in_flight;
            if (record_->retired && record_->in_flight == 0 && !record_->finalized &&
                !record_->finalization_started) {
                record_->finalization_started = true;
                finalize = true;
            }
        }
        record_->lifecycle_cv.notify_all();
        if (finalize)
            finalize_chart_record(record_);
        state_.reset();
        record_.reset();
    }

    std::shared_ptr<ChartHandleRecord> record_;
    std::shared_ptr<State> state_;
};

template <typename State>
ChartLease<State> acquire_chart_lease(sao_ui_widget_handle_t handle, int32_t expected_tag) {
    if (handle == nullptr)
        return {};
    auto& registry = chart_handle_registry();
    std::lock_guard<std::mutex> registry_lock(registry.mtx);
    const auto found = registry.active.find(handle);
    if (found == registry.active.end() || found->second->tag != expected_tag)
        return {};
    const auto& record = found->second;
    std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
    if (!record->accepting || record->retired || record->state == nullptr)
        return {};
    ++record->in_flight;
    return ChartLease<State>(record, std::static_pointer_cast<State>(record->state));
}

template <typename State>
sao_ui_widget_handle_t publish_chart_handle(int32_t tag, const std::shared_ptr<State>& state) {
    auto shell = std::make_unique<ChartHandleShell>();
    auto record = std::make_shared<ChartHandleRecord>();
    auto& registry = chart_handle_registry();
    shell->tag = tag;
    {
        std::lock_guard<std::mutex> registry_lock(registry.mtx);
        shell->generation = registry.next_generation++;
    }
    record->tag = tag;
    record->generation = shell->generation;
    record->state = state;
    auto handle = reinterpret_cast<sao_ui_widget_handle_t>(shell.get());
    if (!sao::ui::detail::register_widget_lifecycle(
            handle, sao::ui::detail::WidgetHandleFamily::chart, tag,
            record->generation)) {
        return nullptr;
    }
    try {
        std::lock_guard<std::mutex> registry_lock(registry.mtx);
        const auto [active_it, active_inserted] = registry.active.emplace(handle, record);
        if (!active_inserted) {
            (void)active_it;
            (void)sao::ui::detail::retire_widget_lifecycle(handle);
            return nullptr;
        }
        registry.shells.push_back(std::move(shell));
        return handle;
    } catch (...) {
        std::lock_guard<std::mutex> registry_lock(registry.mtx);
        registry.active.erase(handle);
        (void)sao::ui::detail::retire_widget_lifecycle(handle);
        return nullptr;
    }
}

bool valid_axis(const SaoUiAxisSpec& axis) {
    if (axis.scale < SAO_UI_AXIS_LINEAR || axis.scale > SAO_UI_AXIS_TIME ||
        axis.time_fmt < SAO_UI_AXIS_TIME_CLOCK || axis.time_fmt > SAO_UI_AXIS_TIME_RELATIVE ||
        axis.desired_tick_count < 0 || axis.desired_tick_count > kMaxAxisTicks) {
        return false;
    }
    const bool min_valid = std::isnan(axis.min_value) || std::isfinite(axis.min_value);
    const bool max_valid = std::isnan(axis.max_value) || std::isfinite(axis.max_value);
    if (!min_valid || !max_valid)
        return false;
    if (std::isnan(axis.min_value) || std::isnan(axis.max_value))
        return true;
    return (axis.min_value == 0.0 && axis.max_value == 0.0) || axis.min_value < axis.max_value;
}

void own_axis_title(SaoUiAxisSpec& axis, std::string& owned_title) {
    owned_title = axis.title_utf8 == nullptr ? "" : axis.title_utf8;
    axis.title_utf8 = owned_title.empty() ? nullptr : owned_title.c_str();
}

bool finite_bar(const SaoUiBarChartBar& bar) {
    return std::isfinite(bar.value) && std::isfinite(bar.overlay_value);
}

bool valid_bars(const SaoUiBarChartBar* bars, size_t count) {
    if ((bars == nullptr && count != 0) || count > kMaxChartItems)
        return false;
    for (size_t index = 0; index < count; ++index) {
        if (!finite_bar(bars[index]))
            return false;
    }
    return true;
}

bool valid_line_series(const SaoUiLineChartSeries* series, size_t count) {
    if ((series == nullptr && count != 0) || count > kMaxChartSeries)
        return false;
    size_t total_points = 0;
    for (size_t series_index = 0; series_index < count; ++series_index) {
        const auto& item = series[series_index];
        if ((item.points == nullptr && item.point_count != 0) ||
            item.point_count > kMaxChartItems - total_points ||
            !std::isfinite(item.line_width_px) || item.line_width_px < 0.0F ||
            (!std::isnan(item.threshold_y) && !std::isfinite(item.threshold_y))) {
            return false;
        }
        total_points += item.point_count;
        for (size_t point_index = 0; point_index < item.point_count; ++point_index) {
            if (!std::isfinite(item.points[point_index].x) ||
                !std::isfinite(item.points[point_index].y)) {
                return false;
            }
        }
    }
    return true;
}

bool valid_sparkline_values(const double* values, size_t count) {
    if ((values == nullptr && count != 0) || count > kMaxChartItems)
        return false;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]))
            return false;
    }
    return true;
}

void set_bars_no_lock(BarChartState& state, const SaoUiBarChartBar* bars, size_t count) {
    std::vector<OwnedBar> replacement;
    replacement.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        replacement.push_back({
            bars[index].label_utf8 == nullptr ? "" : bars[index].label_utf8,
            bars[index].value,
            bars[index].fill_argb,
            bars[index].border_argb,
            bars[index].overlay_value,
            bars[index].overlay_fill_argb,
        });
    }
    if (state.spec.sort_desc) {
        std::stable_sort(
            replacement.begin(), replacement.end(),
            [](const OwnedBar& left, const OwnedBar& right) { return left.value > right.value; });
    }
    state.bars = std::move(replacement);
}

void set_series_no_lock(LineChartState& state, const SaoUiLineChartSeries* series, size_t count) {
    std::vector<OwnedLineSeries> replacement;
    replacement.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        OwnedLineSeries owned{};
        owned.series_id =
            series[index].series_id_utf8 == nullptr ? "" : series[index].series_id_utf8;
        owned.label = series[index].label_utf8 == nullptr ? "" : series[index].label_utf8;
        if (series[index].points != nullptr && series[index].point_count != 0) {
            owned.points.assign(series[index].points,
                                series[index].points + series[index].point_count);
        }
        owned.line_argb = series[index].line_argb;
        owned.marker_argb = series[index].marker_argb;
        owned.line_width_px = series[index].line_width_px;
        owned.dashed = series[index].dashed;
        owned.show_markers = series[index].show_markers;
        owned.threshold_y = series[index].threshold_y;
        owned.threshold_argb = series[index].threshold_argb;
        replacement.push_back(std::move(owned));
    }
    state.series = std::move(replacement);
}

void set_sparkline_values_no_lock(SparklineState& state, const double* values, size_t count) {
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
    if (cap <= 0)
        cap = s.spec.max_visible_points;
    if (cap <= 0)
        cap = kDefaultCapacity;
    return static_cast<size_t>(cap);
}

bool valid_time_points(const SaoUiTimePoint* points, size_t count) {
    if ((points == nullptr && count != 0) || count > kMaxChartItems)
        return false;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(points[index].value))
            return false;
    }
    return true;
}

bool valid_time_lanes(const SaoUiTimeSeriesLane* lanes, size_t count) {
    if ((lanes == nullptr && count != 0) || count > kMaxTimeSeriesLanes) {
        return false;
    }
    std::vector<std::string> lane_ids;
    lane_ids.reserve(count);
    size_t total_points = 0;
    for (size_t index = 0; index < count; ++index) {
        if (lanes[index].lane_id_utf8 == nullptr || lanes[index].lane_id_utf8[0] == '\0' ||
            !std::isfinite(lanes[index].line_width_px) || lanes[index].line_width_px < 0.0F ||
            !std::isfinite(lanes[index].peak_hint) ||
            lanes[index].point_count > kMaxChartItems - total_points ||
            !valid_time_points(lanes[index].points, lanes[index].point_count)) {
            return false;
        }
        total_points += lanes[index].point_count;
        const std::string lane_id(lanes[index].lane_id_utf8);
        if (std::find(lane_ids.begin(), lane_ids.end(), lane_id) != lane_ids.end()) {
            return false;
        }
        lane_ids.push_back(lane_id);
    }
    return true;
}

void configure_lane(TimeSeriesLane& lane, const SaoUiTimeSeriesLane& src, size_t capacity) {
    lane.lane_id = src.lane_id_utf8 ? src.lane_id_utf8 : "";
    lane.label = src.label_utf8 ? src.label_utf8 : "";
    lane.fill_argb = src.fill_argb;
    lane.area_fill_argb = src.area_fill_argb;
    lane.line_width_px = src.line_width_px;
    lane.show_points = src.show_points;
    lane.interpolate = src.interpolate;
    lane.peak_hint = src.peak_hint;
    lane.capacity = capacity;
    lane.ring.assign(capacity, SaoUiTimePoint{0, 0.0});
    lane.count = 0;
    lane.head = 0;
    if (src.points && src.point_count > 0) {
        const size_t start = src.point_count > capacity ? (src.point_count - capacity) : 0;
        for (size_t i = start; i < src.point_count; ++i) {
            lane.ring[lane.head] = src.points[i];
            lane.head = (lane.head + 1) % lane.capacity;
            if (lane.count < lane.capacity)
                ++lane.count;
        }
    }
}

TimeSeriesLane* find_lane_locked(TimeSeriesState& s, const char* lane_id_utf8) {
    if (lane_id_utf8 == nullptr)
        return nullptr;
    for (auto& l : s.lanes) {
        if (l.lane_id == lane_id_utf8)
            return &l;
    }
    return nullptr;
}

bool resize_lanes_no_lock(TimeSeriesState& state, size_t capacity) {
    if (capacity == 0 || capacity > kMaxTimeSeriesCapacity)
        return false;
    std::vector<std::vector<SaoUiTimePoint>> replacement_rings;
    std::vector<size_t> replacement_counts;
    replacement_rings.reserve(state.lanes.size());
    replacement_counts.reserve(state.lanes.size());
    for (const auto& lane : state.lanes) {
        std::vector<SaoUiTimePoint> replacement(capacity, SaoUiTimePoint{0, 0.0});
        const size_t retained = std::min(lane.count, capacity);
        const size_t oldest = (lane.head + lane.capacity - lane.count) % lane.capacity;
        const size_t skip = lane.count - retained;
        for (size_t index = 0; index < retained; ++index) {
            replacement[index] = lane.ring[(oldest + skip + index) % lane.capacity];
        }
        replacement_rings.push_back(std::move(replacement));
        replacement_counts.push_back(retained);
    }
    for (size_t index = 0; index < state.lanes.size(); ++index) {
        auto& lane = state.lanes[index];
        lane.ring = std::move(replacement_rings[index]);
        lane.capacity = capacity;
        lane.count = replacement_counts[index];
        lane.head = lane.count % capacity;
    }
    return true;
}

} // namespace

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_chart_get_generation(sao_ui_widget_handle_t handle, uint64_t* out_generation) {
    if (out_generation != nullptr)
        *out_generation = 0;
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        auto& registry = chart_handle_registry();
        std::lock_guard<std::mutex> registry_lock(registry.mtx);
        const auto found = registry.active.find(handle);
        if (found == registry.active.end())
            return SAO_STATUS_ERR_HANDLE_INVALID;
        const auto& record = found->second;
        std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
        if (!record->accepting || record->retired || record->state == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (out_generation != nullptr)
            *out_generation = record->generation;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Time-series ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_create(
    void* /*d3d_device_ptr*/, const SaoUiTimeSeriesSpec* spec, sao_ui_widget_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    if (spec == nullptr || spec->max_visible_points < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        if (!valid_axis(spec->x_axis) || !valid_axis(spec->y_axis) ||
            static_cast<size_t>(spec->max_visible_points) > kMaxTimeSeriesCapacity ||
            !valid_time_lanes(spec->lanes, spec->lane_count)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto state = std::make_shared<TimeSeriesState>();
        state->spec = *spec;
        state->spec.lanes = nullptr;
        state->spec.lane_count = 0;
        own_axis_title(state->spec.x_axis, state->x_axis_title);
        own_axis_title(state->spec.y_axis, state->y_axis_title);
        state->capacity_override = 0;
        const size_t capacity = resolve_capacity(*state);
        if (capacity > kMaxTimeSeriesCapacity) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        state->lanes.reserve(spec->lane_count);
        for (size_t index = 0; index < spec->lane_count; ++index) {
            TimeSeriesLane lane;
            configure_lane(lane, spec->lanes[index], capacity);
            state->lanes.push_back(std::move(lane));
        }
        *out_handle = publish_chart_handle(kTimeSeriesTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_set_lanes(
    sao_ui_widget_handle_t handle, const SaoUiTimeSeriesLane* lanes, size_t lane_count) {
    try {
        if (!valid_time_lanes(lanes, lane_count)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        const size_t cap = resolve_capacity(*lease.operator->());
        std::vector<TimeSeriesLane> candidate;
        candidate.reserve(lane_count);
        for (size_t i = 0; i < lane_count; ++i) {
            TimeSeriesLane lane;
            configure_lane(lane, lanes[i], cap);
            candidate.push_back(std::move(lane));
        }
        lease->lanes = std::move(candidate);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_append(sao_ui_widget_handle_t handle,
                                                                    const char* lane_id_utf8,
                                                                    const SaoUiTimePoint* points,
                                                                    size_t point_count) {
    if (lane_id_utf8 == nullptr || lane_id_utf8[0] == '\0' ||
        !valid_time_points(points, point_count)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        TimeSeriesLane* lane = find_lane_locked(*lease.operator->(), lane_id_utf8);
        if (lane == nullptr)
            return SAO_STATUS_ERR_NOT_FOUND;
        for (size_t i = 0; i < point_count; ++i) {
            lane->ring[lane->head] = points[i];
            lane->head = (lane->head + 1) % lane->capacity;
            if (lane->count < lane->capacity)
                ++lane->count;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_time_series_chart_set_zoom(sao_ui_widget_handle_t handle,
                                                                      int64_t window_ms) {
    if (window_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        lease->zoom_window_ms = window_ms;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_create(void*, const SaoUiBarChartSpec* spec,
                                                            sao_ui_widget_handle_t* out) {
    if (out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (spec == nullptr || !valid_axis(spec->value_axis) ||
        !valid_bars(spec->bars, spec->bar_count) || spec->max_visible_bars < 0 ||
        spec->bar_gap_px < 0 || spec->bar_thickness_px < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<BarChartState>();
        state->spec = *spec;
        state->spec.bars = nullptr;
        state->spec.bar_count = 0;
        own_axis_title(state->spec.value_axis, state->value_axis_title);
        set_bars_no_lock(*state, spec->bars, spec->bar_count);
        *out = publish_chart_handle(kBarChartTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_set_bars(sao_ui_widget_handle_t handle,
                                                              const SaoUiBarChartBar* bars,
                                                              size_t count) {
    if (!valid_bars(bars, count))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        set_bars_no_lock(*lease.operator->(), bars, count);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar_count(sao_ui_widget_handle_t handle,
                                                                   size_t* out_count,
                                                                   size_t* out_hidden_count) {
    if (out_count == nullptr || out_hidden_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    *out_hidden_count = 0;
    try {
        auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        const size_t limit = lease->spec.max_visible_bars == 0
                                 ? lease->bars.size()
                                 : static_cast<size_t>(lease->spec.max_visible_bars);
        *out_count = std::min(limit, lease->bars.size());
        *out_hidden_count = lease->bars.size() - *out_count;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar(sao_ui_widget_handle_t handle,
                                                             size_t index,
                                                             SaoUiBarChartBar* out_bar) {
    if (out_bar == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_bar = {};
    try {
        auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        thread_local std::string label_snapshot;
        std::lock_guard<std::mutex> lock(lease->mtx);
        const size_t limit =
            lease->spec.max_visible_bars == 0
                ? lease->bars.size()
                : std::min(lease->bars.size(), static_cast<size_t>(lease->spec.max_visible_bars));
        if (index >= limit)
            return SAO_STATUS_ERR_NOT_FOUND;
        const OwnedBar& bar = lease->bars[index];
        label_snapshot = bar.label;
        *out_bar = {label_snapshot.c_str(), bar.value,         bar.fill_argb,
                    bar.border_argb,        bar.overlay_value, bar.overlay_fill_argb};
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_bar_chart_get_bar_copy(
    sao_ui_widget_handle_t handle, size_t index, SaoUiBarChartBar* out_bar, char* out_label_utf8,
    size_t label_capacity, size_t* out_label_required) {
    if (out_bar == nullptr || out_label_required == nullptr ||
        (out_label_utf8 == nullptr && label_capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_bar = {};
    *out_label_required = 0;
    try {
        auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        const size_t limit =
            lease->spec.max_visible_bars == 0
                ? lease->bars.size()
                : std::min(lease->bars.size(), static_cast<size_t>(lease->spec.max_visible_bars));
        if (index >= limit)
            return SAO_STATUS_ERR_NOT_FOUND;
        const OwnedBar& bar = lease->bars[index];
        const size_t required = bar.label.size() + 1;
        *out_label_required = required;
        if (out_label_utf8 == nullptr || label_capacity < required)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out_label_utf8, bar.label.c_str(), required);
        *out_bar = {out_label_utf8,  bar.value,         bar.fill_argb,
                    bar.border_argb, bar.overlay_value, bar.overlay_fill_argb};
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_create(void*, const SaoUiLineChartSpec* spec,
                                                             sao_ui_widget_handle_t* out) {
    if (out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (spec == nullptr || !valid_axis(spec->x_axis) || !valid_axis(spec->y_axis) ||
        !valid_line_series(spec->series, spec->series_count) || spec->left_pad_px < 0 ||
        spec->right_pad_px < 0 || spec->top_pad_px < 0 || spec->bottom_pad_px < 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<LineChartState>();
        state->spec = *spec;
        state->spec.series = nullptr;
        state->spec.series_count = 0;
        own_axis_title(state->spec.x_axis, state->x_axis_title);
        own_axis_title(state->spec.y_axis, state->y_axis_title);
        set_series_no_lock(*state, spec->series, spec->series_count);
        *out = publish_chart_handle(kLineChartTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_set_series(sao_ui_widget_handle_t handle,
                                                                 const SaoUiLineChartSeries* series,
                                                                 size_t count) {
    if (!valid_line_series(series, count))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_chart_lease<LineChartState>(handle, kLineChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        set_series_no_lock(*lease.operator->(), series, count);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_line_chart_get_series_count(sao_ui_widget_handle_t handle, size_t* out_count) {
    if (out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    try {
        auto lease = acquire_chart_lease<LineChartState>(handle, kLineChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        *out_count = lease->series.size();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_line_chart_get_point(sao_ui_widget_handle_t handle,
                                                                size_t series_index,
                                                                size_t point_index,
                                                                SaoUiLinePoint* out_point) {
    if (out_point == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_point = {};
    try {
        auto lease = acquire_chart_lease<LineChartState>(handle, kLineChartTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        if (series_index >= lease->series.size() ||
            point_index >= lease->series[series_index].points.size()) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        *out_point = lease->series[series_index].points[point_index];
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_create(void*, const SaoUiSparklineSpec* spec,
                                                            sao_ui_widget_handle_t* out) {
    if (out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (spec == nullptr || spec->max_points > kMaxChartItems ||
        !std::isfinite(spec->line_width_px) || spec->line_width_px < 0.0F ||
        !valid_sparkline_values(spec->values, spec->value_count)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<SparklineState>();
        state->spec = *spec;
        state->spec.values = nullptr;
        state->spec.value_count = 0;
        set_sparkline_values_no_lock(*state, spec->values, spec->value_count);
        *out = publish_chart_handle(kSparklineTag, state);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_set_values(sao_ui_widget_handle_t handle,
                                                                const double* values,
                                                                size_t count) {
    if (!valid_sparkline_values(values, count))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_chart_lease<SparklineState>(handle, kSparklineTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        set_sparkline_values_no_lock(*lease.operator->(), values, count);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_append(sao_ui_widget_handle_t handle,
                                                            double value) {
    if (!std::isfinite(value))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_chart_lease<SparklineState>(handle, kSparklineTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        const size_t capacity = lease->spec.max_points == 0 ? 64 : lease->spec.max_points;
        if (lease->values.size() == capacity)
            lease->values.erase(lease->values.begin());
        lease->values.push_back(value);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_sparkline_get_range(sao_ui_widget_handle_t handle,
                                                               double* out_min, double* out_max,
                                                               size_t* out_count) {
    if (out_min == nullptr || out_max == nullptr || out_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_min = 0.0;
    *out_max = 0.0;
    *out_count = 0;
    try {
        auto lease = acquire_chart_lease<SparklineState>(handle, kSparklineTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        *out_count = lease->values.size();
        if (lease->values.empty())
            return SAO_STATUS_OK;
        const auto range = std::minmax_element(lease->values.begin(), lease->values.end());
        *out_min = *range.first;
        *out_max = *range.second;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Time-series helper API — sample fetch, clear, axis auto-compute.
// ---------------------------------------------------------------------------

// Preferred ring capacity setter (0 → use spec.max_visible_points).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_set_capacity(sao_ui_widget_handle_t handle, int32_t capacity) {
    if (capacity < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        const int32_t previous_override = lease->capacity_override;
        lease->capacity_override = capacity;
        const size_t resolved = resolve_capacity(*lease.operator->());
        lease->capacity_override = previous_override;
        if (resolved > kMaxTimeSeriesCapacity) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (!lease->lanes.empty() && lease->lanes.front().capacity != resolved &&
            !resize_lanes_no_lock(*lease.operator->(), resolved)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        lease->capacity_override = capacity;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_get_capacity(sao_ui_widget_handle_t handle, size_t* out_capacity) {
    if (out_capacity == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_capacity = 0;
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lock(lease->mtx);
        *out_capacity = resolve_capacity(*lease.operator->());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Number of samples currently stored in a lane's ring.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_get_sample_count(
    sao_ui_widget_handle_t handle, const char* lane_id_utf8, size_t* out_count) {
    if (lane_id_utf8 == nullptr || lane_id_utf8[0] == '\0' || out_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        TimeSeriesLane* lane = find_lane_locked(*lease.operator->(), lane_id_utf8);
        if (lane == nullptr)
            return SAO_STATUS_ERR_NOT_FOUND;
        *out_count = lane->count;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Push a single sample (convenience wrapper over append).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_push_sample(
    sao_ui_widget_handle_t handle, const char* lane_id_utf8, int64_t timestamp_ms, double value) {
    try {
        const SaoUiTimePoint point{timestamp_ms, value};
        return sao_ui_time_series_chart_append(handle, lane_id_utf8, &point, 1);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Copy up to `capacity` samples in chronological order into `samples_out`.
// If capacity is smaller than count, we return the tail (most recent
// `capacity` points); if larger, we return count points and set
// `count_out = count`.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_get_samples(
    sao_ui_widget_handle_t handle, const char* lane_id_utf8, SaoUiTimePoint* samples_out,
    size_t capacity, size_t* count_out) {
    if (lane_id_utf8 == nullptr || lane_id_utf8[0] == '\0' ||
        (samples_out == nullptr && capacity > 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (count_out)
        *count_out = 0;
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        TimeSeriesLane* lane = find_lane_locked(*lease.operator->(), lane_id_utf8);
        if (lane == nullptr)
            return SAO_STATUS_ERR_NOT_FOUND;
        const size_t to_copy = std::min(capacity, lane->count);
        const size_t start = (lane->head + lane->capacity - lane->count) % lane->capacity;
        const size_t skip = lane->count - to_copy;
        for (size_t index = 0; index < to_copy; ++index) {
            const size_t source_index = (start + skip + index) % lane->capacity;
            samples_out[index] = lane->ring[source_index];
        }
        if (count_out)
            *count_out = to_copy;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Reset all lanes to empty rings (retains lane configuration).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_timeseries_clear(sao_ui_widget_handle_t handle) {
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        for (auto& lane : lease->lanes) {
            std::fill(lane.ring.begin(), lane.ring.end(), SaoUiTimePoint{0, 0.0});
            lane.count = 0;
            lane.head = 0;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_timeseries_compute_axis(
    sao_ui_widget_handle_t handle, SaoUiTimeSeriesAxis* out_axis) {
    if (out_axis == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_axis = {};
    try {
        auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
        if (!lease)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        std::lock_guard<std::mutex> lk(lease->mtx);
        int64_t all_x_min = std::numeric_limits<int64_t>::max();
        int64_t x_max = std::numeric_limits<int64_t>::min();
        bool any = false;
        for (const auto& lane : lease->lanes) {
            const size_t start = (lane.head + lane.capacity - lane.count) % lane.capacity;
            for (size_t index = 0; index < lane.count; ++index) {
                const auto& point = lane.ring[(start + index) % lane.capacity];
                all_x_min = std::min(all_x_min, point.time_ms);
                x_max = std::max(x_max, point.time_ms);
                any = true;
            }
        }
        if (!any)
            return SAO_STATUS_OK;

        int64_t x_min = all_x_min;
        if (lease->zoom_window_ms > 0) {
            const int64_t threshold =
                x_max < std::numeric_limits<int64_t>::min() + lease->zoom_window_ms
                    ? std::numeric_limits<int64_t>::min()
                    : x_max - lease->zoom_window_ms;
            x_min = std::max(all_x_min, threshold);
        }
        double y_min = std::numeric_limits<double>::infinity();
        double y_max = -std::numeric_limits<double>::infinity();
        for (const auto& lane : lease->lanes) {
            const size_t start = (lane.head + lane.capacity - lane.count) % lane.capacity;
            for (size_t index = 0; index < lane.count; ++index) {
                const auto& point = lane.ring[(start + index) % lane.capacity];
                if (point.time_ms < x_min)
                    continue;
                y_min = std::min(y_min, point.value);
                y_max = std::max(y_max, point.value);
            }
        }
        out_axis->x_min_ms = x_min;
        out_axis->x_max_ms = x_max;
        out_axis->y_min = y_min;
        out_axis->y_max = y_max;
        out_axis->tick_count =
            lease->spec.x_axis.desired_tick_count > 0 ? lease->spec.x_axis.desired_tick_count : 5;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Shared destroy helper.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_chart_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        std::shared_ptr<ChartHandleRecord> record;
        auto& registry = chart_handle_registry();
        {
            std::lock_guard<std::mutex> registry_lock(registry.mtx);
            const auto found = registry.active.find(handle);
            if (found == registry.active.end())
                return;
            record = found->second;
            {
                std::lock_guard<std::mutex> lifecycle_lock(record->lifecycle_mtx);
                record->accepting = false;
                record->retired = true;
            }
            registry.active.erase(found);
        }
            (void)sao::ui::detail::retire_widget_lifecycle(handle);
        uint32_t removed = 0;
        (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);

        bool finalize = false;
        std::unique_lock<std::mutex> lifecycle_lock(record->lifecycle_mtx);
        while (!record->finalized) {
            if (record->in_flight == 0 && !record->finalization_started) {
                record->finalization_started = true;
                finalize = true;
                break;
            }
            record->lifecycle_cv.wait(lifecycle_lock);
        }
        lifecycle_lock.unlock();
        if (finalize)
            finalize_chart_record(record);
    } catch (...) {
    }
}
