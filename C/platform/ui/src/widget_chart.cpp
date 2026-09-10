// SAO Auto — time-series, bar, line, and sparkline chart widgets.

#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_kit.h"

#include "panel_theme_internal.h"
#include "widget_typed_internal.h"

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

struct ChartPropsSnapshot {
    std::vector<TimeSeriesLane> lanes;
    std::vector<OwnedBar> bars;
    std::vector<OwnedLineSeries> series;
    std::vector<double> values;
    int64_t zoom_window_ms{};
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

sao_status_t sao::ui::detail::widget_chart_apply_props(
    sao_ui_widget_handle_t handle, int32_t kind, const WidgetPropsJson& props,
    WidgetPropsSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    try {
        auto snapshot = std::make_shared<ChartPropsSnapshot>();
        switch (kind) {
        case kTimeSeriesTag: {
            if (!widget_props_has_only(props, {"lanes", "zoom_window_ms"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const auto lanes_property = props.find("lanes");
            const auto zoom_property = props.find("zoom_window_ms");
            int64_t zoom = 0;
            if (zoom_property != props.end() &&
                (!widget_props_i64(*zoom_property, &zoom) || zoom < 0)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }

            std::vector<std::string> lane_ids;
            std::vector<std::string> lane_labels;
            std::vector<std::vector<SaoUiTimePoint>> lane_points;
            std::vector<SaoUiTimeSeriesLane> lanes;
            if (lanes_property != props.end()) {
                if (!lanes_property->is_array() ||
                    lanes_property->size() > kMaxTimeSeriesLanes) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                const size_t count = lanes_property->size();
                lane_ids.resize(count);
                lane_labels.resize(count);
                lane_points.resize(count);
                lanes.resize(count);
                for (size_t index = 0; index < count; ++index) {
                    const auto& item = (*lanes_property)[index];
                    if (!widget_props_has_only(
                            item, {"lane_id", "label", "points", "fill_argb",
                                   "area_fill_argb", "line_width_px", "show_points",
                                   "interpolate", "peak_hint"})) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    const auto lane_id = item.find("lane_id");
                    if (lane_id == item.end() || !lane_id->is_string())
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    lane_ids[index] = lane_id->get<std::string>();
                    if (lane_ids[index].empty() ||
                        std::find(lane_ids.begin(), lane_ids.begin() + index, lane_ids[index]) !=
                            lane_ids.begin() + index) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    const auto label = item.find("label");
                    if (label != item.end()) {
                        if (!label->is_string())
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        lane_labels[index] = label->get<std::string>();
                    }
                    const auto points = item.find("points");
                    if (points != item.end()) {
                        if (!points->is_array() || points->size() > kMaxChartItems)
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        lane_points[index].reserve(points->size());
                        for (const auto& point : *points) {
                            if (!widget_props_has_only(point, {"time_ms", "value"}))
                                return SAO_STATUS_ERR_INVALID_ARGUMENT;
                            const auto time = point.find("time_ms");
                            const auto value = point.find("value");
                            int64_t time_ms = 0;
                            double sample = 0.0;
                            if (time == point.end() || value == point.end() ||
                                !widget_props_i64(*time, &time_ms) ||
                                !widget_props_double(*value, &sample)) {
                                return SAO_STATUS_ERR_INVALID_ARGUMENT;
                            }
                            lane_points[index].push_back({time_ms, sample});
                        }
                    }
                    auto& lane = lanes[index];
                    lane.lane_id_utf8 = lane_ids[index].c_str();
                    lane.label_utf8 = lane_labels[index].empty() ? nullptr
                                                                 : lane_labels[index].c_str();
                    lane.points = lane_points[index].empty() ? nullptr : lane_points[index].data();
                    lane.point_count = lane_points[index].size();
                    lane.line_width_px = 1.5F;
                    lane.interpolate = true;
                    const auto fill = item.find("fill_argb");
                    const auto area = item.find("area_fill_argb");
                    const auto line_width = item.find("line_width_px");
                    const auto show_points = item.find("show_points");
                    const auto interpolate = item.find("interpolate");
                    const auto peak_hint = item.find("peak_hint");
                    if ((fill != item.end() && !widget_props_argb(*fill, &lane.fill_argb)) ||
                        (area != item.end() &&
                         !widget_props_argb(*area, &lane.area_fill_argb)) ||
                        (line_width != item.end() &&
                         (!widget_props_float(*line_width, &lane.line_width_px) ||
                          lane.line_width_px < 0.0F)) ||
                        (show_points != item.end() &&
                         !widget_props_bool(*show_points, &lane.show_points)) ||
                        (interpolate != item.end() &&
                         !widget_props_bool(*interpolate, &lane.interpolate)) ||
                        (peak_hint != item.end() &&
                         !widget_props_double(*peak_hint, &lane.peak_hint))) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                }
            }

            auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                snapshot->lanes = lease->lanes;
                snapshot->zoom_window_ms = lease->zoom_window_ms;
            }
            sao_status_t status = SAO_STATUS_OK;
            if (lanes_property != props.end()) {
                status = sao_ui_time_series_chart_set_lanes(
                    handle, lanes.empty() ? nullptr : lanes.data(), lanes.size());
            }
            if (status == SAO_STATUS_OK && zoom_property != props.end())
                status = sao_ui_time_series_chart_set_zoom(handle, zoom);
            if (status != SAO_STATUS_OK) {
                auto restore = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
                if (restore) {
                    std::lock_guard<std::mutex> lock(restore->mtx);
                    restore->lanes = snapshot->lanes;
                    restore->zoom_window_ms = snapshot->zoom_window_ms;
                }
                return status;
            }
            break;
        }
        case kBarChartTag: {
            if (!widget_props_has_only(props, {"bars"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const auto bars_property = props.find("bars");
            std::vector<std::string> labels;
            std::vector<SaoUiBarChartBar> bars;
            if (bars_property != props.end()) {
                if (!bars_property->is_array() || bars_property->size() > kMaxChartItems)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                labels.resize(bars_property->size());
                bars.resize(bars_property->size());
                for (size_t index = 0; index < bars.size(); ++index) {
                    const auto& item = (*bars_property)[index];
                    if (!widget_props_has_only(
                            item, {"label", "value", "fill_argb", "border_argb",
                                   "overlay_value", "overlay_fill_argb"})) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    const auto label = item.find("label");
                    const auto value = item.find("value");
                    if (value == item.end() || !widget_props_double(*value, &bars[index].value))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    if (label != item.end()) {
                        if (!label->is_string())
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        labels[index] = label->get<std::string>();
                    }
                    bars[index].label_utf8 = labels[index].c_str();
                    const auto fill = item.find("fill_argb");
                    const auto border = item.find("border_argb");
                    const auto overlay = item.find("overlay_value");
                    const auto overlay_fill = item.find("overlay_fill_argb");
                    if ((fill != item.end() &&
                         !widget_props_argb(*fill, &bars[index].fill_argb)) ||
                        (border != item.end() &&
                         !widget_props_argb(*border, &bars[index].border_argb)) ||
                        (overlay != item.end() &&
                         !widget_props_double(*overlay, &bars[index].overlay_value)) ||
                        (overlay_fill != item.end() &&
                         !widget_props_argb(*overlay_fill,
                                            &bars[index].overlay_fill_argb))) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                }
            }
            auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                snapshot->bars = lease->bars;
            }
            if (bars_property != props.end()) {
                const sao_status_t status = sao_ui_bar_chart_set_bars(
                    handle, bars.empty() ? nullptr : bars.data(), bars.size());
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kLineChartTag: {
            if (!widget_props_has_only(props, {"series"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const auto series_property = props.find("series");
            std::vector<std::string> ids;
            std::vector<std::string> labels;
            std::vector<std::vector<SaoUiLinePoint>> points;
            std::vector<SaoUiLineChartSeries> series;
            if (series_property != props.end()) {
                if (!series_property->is_array() || series_property->size() > kMaxChartSeries)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const size_t count = series_property->size();
                ids.resize(count);
                labels.resize(count);
                points.resize(count);
                series.resize(count);
                size_t total_points = 0;
                for (size_t index = 0; index < count; ++index) {
                    const auto& item = (*series_property)[index];
                    if (!widget_props_has_only(
                            item, {"series_id", "label", "points", "line_argb",
                                   "marker_argb", "line_width_px", "dashed", "show_markers",
                                   "threshold_y", "threshold_argb"})) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                    const auto id = item.find("series_id");
                    if (id == item.end() || !id->is_string())
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    ids[index] = id->get<std::string>();
                    const auto label = item.find("label");
                    if (label != item.end()) {
                        if (!label->is_string())
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        labels[index] = label->get<std::string>();
                    }
                    const auto point_array = item.find("points");
                    if (point_array != item.end()) {
                        if (!point_array->is_array() ||
                            point_array->size() > kMaxChartItems - total_points) {
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        }
                        total_points += point_array->size();
                        points[index].reserve(point_array->size());
                        for (const auto& point : *point_array) {
                            if (!widget_props_has_only(point, {"x", "y"}))
                                return SAO_STATUS_ERR_INVALID_ARGUMENT;
                            const auto px = point.find("x");
                            const auto py = point.find("y");
                            SaoUiLinePoint parsed{};
                            if (px == point.end() || py == point.end() ||
                                !widget_props_double(*px, &parsed.x) ||
                                !widget_props_double(*py, &parsed.y)) {
                                return SAO_STATUS_ERR_INVALID_ARGUMENT;
                            }
                            points[index].push_back(parsed);
                        }
                    }
                    auto& output = series[index];
                    output.series_id_utf8 = ids[index].c_str();
                    output.label_utf8 = labels[index].empty() ? nullptr : labels[index].c_str();
                    output.points = points[index].empty() ? nullptr : points[index].data();
                    output.point_count = points[index].size();
                    output.line_width_px = 1.0F;
                    output.threshold_y = std::numeric_limits<double>::quiet_NaN();
                    const auto line_color = item.find("line_argb");
                    const auto marker_color = item.find("marker_argb");
                    const auto line_width = item.find("line_width_px");
                    const auto dashed = item.find("dashed");
                    const auto show_markers = item.find("show_markers");
                    const auto threshold = item.find("threshold_y");
                    const auto threshold_color = item.find("threshold_argb");
                    if ((line_color != item.end() &&
                         !widget_props_argb(*line_color, &output.line_argb)) ||
                        (marker_color != item.end() &&
                         !widget_props_argb(*marker_color, &output.marker_argb)) ||
                        (line_width != item.end() &&
                         (!widget_props_float(*line_width, &output.line_width_px) ||
                          output.line_width_px < 0.0F)) ||
                        (dashed != item.end() && !widget_props_bool(*dashed, &output.dashed)) ||
                        (show_markers != item.end() &&
                         !widget_props_bool(*show_markers, &output.show_markers)) ||
                        (threshold != item.end() &&
                         !widget_props_double(*threshold, &output.threshold_y)) ||
                        (threshold_color != item.end() &&
                         !widget_props_argb(*threshold_color, &output.threshold_argb))) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                }
            }
            auto lease = acquire_chart_lease<LineChartState>(handle, kLineChartTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                snapshot->series = lease->series;
            }
            if (series_property != props.end()) {
                const sao_status_t status = sao_ui_line_chart_set_series(
                    handle, series.empty() ? nullptr : series.data(), series.size());
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kSparklineTag: {
            if (!widget_props_has_only(props, {"values"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const auto values_property = props.find("values");
            std::vector<double> values;
            if (values_property != props.end()) {
                if (!values_property->is_array() || values_property->size() > kMaxChartItems)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                values.reserve(values_property->size());
                for (const auto& value : *values_property) {
                    double parsed = 0.0;
                    if (!widget_props_double(value, &parsed))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    values.push_back(parsed);
                }
            }
            auto lease = acquire_chart_lease<SparklineState>(handle, kSparklineTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                snapshot->values = lease->values;
            }
            if (values_property != props.end()) {
                const sao_status_t status = sao_ui_sparkline_set_values(
                    handle, values.empty() ? nullptr : values.data(), values.size());
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        default:
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }
        *out_snapshot = std::move(snapshot);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sao::ui::detail::widget_chart_restore_props(
    sao_ui_widget_handle_t handle, int32_t kind,
    const WidgetPropsSnapshot& snapshot) noexcept {
    const auto previous = std::static_pointer_cast<ChartPropsSnapshot>(snapshot);
    if (previous == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        switch (kind) {
        case kTimeSeriesTag: {
            auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(lease->mtx);
            lease->lanes = previous->lanes;
            lease->zoom_window_ms = previous->zoom_window_ms;
            return SAO_STATUS_OK;
        }
        case kBarChartTag: {
            auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(lease->mtx);
            lease->bars = previous->bars;
            return SAO_STATUS_OK;
        }
        case kLineChartTag: {
            auto lease = acquire_chart_lease<LineChartState>(handle, kLineChartTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(lease->mtx);
            lease->series = previous->series;
            return SAO_STATUS_OK;
        }
        case kSparklineTag: {
            auto lease = acquire_chart_lease<SparklineState>(handle, kSparklineTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(lease->mtx);
            lease->values = previous->values;
            return SAO_STATUS_OK;
        }
        default:
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sao::ui::detail::widget_chart_paint(
    sao_ui_widget_handle_t handle, int32_t kind,
    sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
    int32_t width, int32_t height) noexcept {
    try {
        const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
        float plot_origin_x = static_cast<float>(x + 2);
        float plot_origin_y = static_cast<float>(y + 2);
        float plot_size_w = static_cast<float>(std::max(0, width - 4));
        float plot_size_h = static_cast<float>(std::max(0, height - 4));
        const auto configure_plot = [&](int32_t left, int32_t right,
                                        int32_t top, int32_t bottom) {
            const int32_t bounded_width = std::max(0, width);
            const int32_t bounded_height = std::max(0, height);
            const int32_t bounded_left = std::max(0, left);
            const int32_t bounded_right = std::max(0, right);
            const int32_t bounded_top = std::max(0, top);
            const int32_t bounded_bottom = std::max(0, bottom);
            plot_origin_x = static_cast<float>(x + bounded_left);
            plot_origin_y = static_cast<float>(y + bounded_top);
            plot_size_w = static_cast<float>(std::max(0, bounded_width - bounded_left - bounded_right));
            plot_size_h = static_cast<float>(std::max(0, bounded_height - bounded_top - bounded_bottom));
        };
        const auto paint_polyline = [&](const std::vector<std::pair<double, double>>& points,
                                        uint32_t color, float line_width,
                                        double min_x, double max_x,
                                        double min_y, double max_y) -> sao_status_t {
            if (points.empty() || plot_size_w <= 0.0F || plot_size_h <= 0.0F)
                return SAO_STATUS_OK;
            // Decimate dense series to a bounded vertex count; the
            // stride keeps min/max extrema visible.
            const size_t kMaxPaintPoints = 600;
            const size_t stride =
                points.size() > kMaxPaintPoints
                    ? (points.size() + kMaxPaintPoints - 1U) / kMaxPaintPoints
                    : 1U;
            const double x_span = max_x > min_x ? max_x - min_x : 1.0;
            const double y_span = max_y > min_y ? max_y - min_y : 1.0;
            const auto screen = [&](const auto& point) {
                const float px = static_cast<float>(
                    plot_origin_x + plot_size_w * static_cast<float>((point.first - min_x) / x_span));
                const float py = static_cast<float>(
                    plot_origin_y + plot_size_h * (1.0F - static_cast<float>((point.second - min_y) / y_span)));
                return std::pair{px, py};
            };
            if (points.size() == 1) {
                const auto [px, py] = screen(points.front());
                return sao_ui_paint_ctx_fill_ellipse(context, px - 2.0F, py - 2.0F, 4.0F,
                                                     4.0F, color);
            }
            size_t previous = 0;
            for (size_t index = stride; index < points.size(); index += stride) {
                const auto [x1, y1] = screen(points[previous]);
                const auto [x2, y2] = screen(points[index]);
                const sao_status_t status = sao_ui_paint_ctx_stroke_line(
                    context, x1, y1, x2, y2, std::max(1.0F, line_width), color);
                if (status != SAO_STATUS_OK)
                    return status;
                previous = index;
            }
            if (previous + 1U < points.size()) {
                const auto [x1, y1] = screen(points[previous]);
                const auto [x2, y2] = screen(points.back());
                const sao_status_t status = sao_ui_paint_ctx_stroke_line(
                    context, x1, y1, x2, y2, std::max(1.0F, line_width), color);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            return SAO_STATUS_OK;
        };

        if (kind == kTimeSeriesTag) {
            SaoUiTimeSeriesSpec spec{};
            std::vector<TimeSeriesLane> lanes;
            auto lease = acquire_chart_lease<TimeSeriesState>(handle, kTimeSeriesTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                spec = lease->spec;
                lanes = lease->lanes;
            }
            sao_status_t status = sao_ui_paint_ctx_fill_rect(
                context, static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(width), static_cast<float>(height),
                high_contrast || spec.bg_argb == 0
                    ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_CARD)
                    : spec.bg_argb);
            if (status != SAO_STATUS_OK)
                return status;
            double min_x = std::numeric_limits<double>::infinity();
            double max_x = -std::numeric_limits<double>::infinity();
            double min_y = std::numeric_limits<double>::infinity();
            double max_y = -std::numeric_limits<double>::infinity();
            std::vector<std::vector<std::pair<double, double>>> samples(lanes.size());
            for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
                const auto& lane = lanes[lane_index];
                const size_t start = (lane.head + lane.capacity - lane.count) % lane.capacity;
                samples[lane_index].reserve(lane.count);
                for (size_t index = 0; index < lane.count; ++index) {
                    const auto& point = lane.ring[(start + index) % lane.capacity];
                    const double px = static_cast<double>(point.time_ms);
                    samples[lane_index].emplace_back(px, point.value);
                    min_x = std::min(min_x, px);
                    max_x = std::max(max_x, px);
                    min_y = std::min(min_y, point.value);
                    max_y = std::max(max_y, point.value);
                }
            }
            if (!std::isfinite(min_x)) {
                constexpr const char* kEmptyHint = "暂无数据";
                return sao_ui_paint_ctx_draw_utf8(
                    context, static_cast<float>(x + (width - 44) / 2),
                    static_cast<float>(y + (height - 11) / 2), kEmptyHint, 11.0F,
                    sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT_2));
            }
            configure_plot(spec.left_pad_px, spec.right_pad_px, spec.top_pad_px, spec.bottom_pad_px);
            for (int32_t tick = 1; tick < 5; ++tick) {
                const float gx = plot_origin_x + plot_size_w * tick / 5.0F;
                const float gy = plot_origin_y + plot_size_h * tick / 5.0F;
                status = sao_ui_paint_ctx_stroke_line(context, gx, plot_origin_y, gx,
                                                       plot_origin_y + plot_size_h, 1.0F,
                                                       sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER));
                if (status != SAO_STATUS_OK) return status;
                status = sao_ui_paint_ctx_stroke_line(context, plot_origin_x, gy,
                                                       plot_origin_x + plot_size_w, gy, 1.0F,
                                                       sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER));
                if (status != SAO_STATUS_OK) return status;
            }
            // Summary stats top-left: latest value / peak.
            if (!lanes.empty() && !samples[0].empty() &&
                (spec.show_latest_value || spec.show_peak)) {
                const uint32_t stat_color = sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT);
                int32_t stat_row = 0;
                if (spec.show_latest_value) {
                    const double latest = samples[0].back().second;
                    const std::string label = "最新 " + std::to_string(static_cast<int64_t>(latest));
                    status = sao_ui_paint_ctx_draw_utf8(context, static_cast<float>(x + 6),
                                                        static_cast<float>(y + 5 + stat_row * 13),
                                                        label.c_str(), 10.0F, stat_color);
                    if (status != SAO_STATUS_OK) return status;
                    ++stat_row;
                }
                if (spec.show_peak) {
                    const std::string label = "峰值 " + std::to_string(static_cast<int64_t>(max_y));
                    status = sao_ui_paint_ctx_draw_utf8(context, static_cast<float>(x + 6),
                                                        static_cast<float>(y + 5 + stat_row * 13),
                                                        label.c_str(), 10.0F, stat_color);
                    if (status != SAO_STATUS_OK) return status;
                }
            }
            // Legend top-right: colour swatch + lane label.
            if (spec.show_legend && !lanes.empty()) {
                const float legend_x = static_cast<float>(x + width - 92);
                for (size_t legend_row = 0; legend_row < lanes.size(); ++legend_row) {
                    const float row_y = static_cast<float>(y + 4) +
                                        static_cast<float>(legend_row) * 13.0F;
                    const uint32_t legend_color =
                        lanes[legend_row].fill_argb == 0
                            ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                            : lanes[legend_row].fill_argb;
                    status = sao_ui_paint_ctx_fill_rect(context, legend_x, row_y, 8.0F, 8.0F,
                                                        legend_color);
                    if (status != SAO_STATUS_OK) return status;
                    const std::string legend_label =
                        !lanes[legend_row].label.empty() ? lanes[legend_row].label
                                                        : lanes[legend_row].lane_id;
                    status = sao_ui_paint_ctx_draw_utf8(
                        context, legend_x + 12.0F, row_y - 1.0F, legend_label.c_str(), 9.0F,
                        sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT));
                    if (status != SAO_STATUS_OK) return status;
                }
            }
            // Lane overlays: area shading and dot markers first, then the
            // line on top.
            const double x_span = max_x > min_x ? max_x - min_x : 1.0;
            const double y_span = max_y > min_y ? max_y - min_y : 1.0;
            const float baseline_y = plot_origin_y + plot_size_h;
            for (size_t overlay_lane = 0; overlay_lane < lanes.size(); ++overlay_lane) {
                const auto& pts = samples[overlay_lane];
                if (pts.size() < 2U)
                    continue;
                const uint32_t lane_color =
                    lanes[overlay_lane].fill_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                        : lanes[overlay_lane].fill_argb;
                const size_t stride =
                    pts.size() > 600U ? (pts.size() + 599U) / 600U : 1U;
                if (!high_contrast && lanes[overlay_lane].area_fill_argb != 0) {
                    for (size_t i = stride; ; i += stride) {
                        const size_t end = std::min(i, pts.size() - 1U);
                        const float sx0 = static_cast<float>(
                            plot_origin_x +
                            plot_size_w * static_cast<float>((pts[i - stride].first - min_x) / x_span));
                        const float sy0 = static_cast<float>(
                            plot_origin_y +
                            plot_size_h *
                                (1.0F -
                                 static_cast<float>((pts[i - stride].second - min_y) / y_span)));
                        const float sx1 = static_cast<float>(
                            plot_origin_x +
                            plot_size_w * static_cast<float>((pts[end].first - min_x) / x_span));
                        const float left = std::min(sx0, sx1);
                        const float right = std::max(sx0, sx1);
                        status = sao_ui_paint_ctx_fill_rect(
                            context, left, std::min(sy0, baseline_y),
                            std::max(1.0F, right - left), std::max(1.0F, baseline_y - sy0),
                            lanes[overlay_lane].area_fill_argb);
                        if (status != SAO_STATUS_OK) return status;
                        if (end == pts.size() - 1U)
                            break;
                    }
                }
                if (lanes[overlay_lane].show_points) {
                    for (size_t i = 0; i < pts.size(); i += stride) {
                        const float px = static_cast<float>(
                            plot_origin_x +
                            plot_size_w * static_cast<float>((pts[i].first - min_x) / x_span));
                        const float py = static_cast<float>(
                            plot_origin_y +
                            plot_size_h *
                                (1.0F -
                                 static_cast<float>((pts[i].second - min_y) / y_span)));
                        status = sao_ui_paint_ctx_fill_ellipse(context, px - 1.5F, py - 1.5F,
                                                               3.0F, 3.0F, lane_color);
                        if (status != SAO_STATUS_OK) return status;
                    }
                }
            }
            for (size_t index = 0; index < lanes.size(); ++index) {
                status = paint_polyline(
                    samples[index],
                    lanes[index].fill_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                        : lanes[index].fill_argb,
                    lanes[index].line_width_px, min_x, max_x, min_y, max_y);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            return SAO_STATUS_OK;
        }
        if (kind == kBarChartTag) {
            SaoUiBarChartSpec spec{};
            std::vector<OwnedBar> bars;
            auto lease = acquire_chart_lease<BarChartState>(handle, kBarChartTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                spec = lease->spec;
                bars = lease->bars;
            }
            sao_status_t status = sao_ui_paint_ctx_fill_rect(
                context, static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(width), static_cast<float>(height),
                high_contrast || spec.bg_argb == 0
                    ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_CARD)
                    : spec.bg_argb);
            if (status != SAO_STATUS_OK)
                return status;
            if (bars.empty()) {
                constexpr const char* kEmptyHint = "暂无数据";
                return sao_ui_paint_ctx_draw_utf8(
                    context, static_cast<float>(x + (width - 44) / 2),
                    static_cast<float>(y + (height - 11) / 2), kEmptyHint, 11.0F,
                    sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT_2));
            }
            const size_t visible = spec.max_visible_bars <= 0
                                       ? bars.size()
                                       : std::min(bars.size(),
                                                  static_cast<size_t>(spec.max_visible_bars));
            double maximum = 0.0;
            for (size_t index = 0; index < visible; ++index)
                maximum = std::max(maximum, std::max(0.0, bars[index].value));
            if (maximum <= 0.0)
                maximum = 1.0;
            if (spec.horizontal) {
                const float row_height = static_cast<float>(height) / visible;
                for (size_t index = 0; index < visible; ++index) {
                    const float bar_width = static_cast<float>(
                        (width - 4) * (std::max(0.0, bars[index].value) / maximum));
                    status = sao_ui_paint_ctx_fill_rect(
                        context, static_cast<float>(x + 2), y + row_height * index + 1.0F,
                        std::max(1.0F, bar_width), std::max(1.0F, row_height - 2.0F),
                        bars[index].fill_argb == 0
                            ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                            : bars[index].fill_argb);
                    if (status != SAO_STATUS_OK)
                        return status;
                }
                return SAO_STATUS_OK;
            }
            const float column_width = static_cast<float>(width) / visible;
            for (size_t index = 0; index < visible; ++index) {
                const float bar_height = static_cast<float>(
                    (height - 4) * (std::max(0.0, bars[index].value) / maximum));
                status = sao_ui_paint_ctx_fill_rect(
                    context, x + column_width * index + 1.0F,
                    static_cast<float>(y + height - 2) - bar_height,
                    std::max(1.0F, column_width - 2.0F), std::max(1.0F, bar_height),
                    bars[index].fill_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                        : bars[index].fill_argb);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            return SAO_STATUS_OK;
        }
        if (kind == kLineChartTag) {
            SaoUiLineChartSpec spec{};
            std::vector<OwnedLineSeries> series;
            auto lease = acquire_chart_lease<LineChartState>(handle, kLineChartTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                spec = lease->spec;
                series = lease->series;
            }
            sao_status_t status = sao_ui_paint_ctx_fill_rect(
                context, static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(width), static_cast<float>(height),
                high_contrast || spec.bg_argb == 0
                    ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_CARD)
                    : spec.bg_argb);
            if (status != SAO_STATUS_OK)
                return status;
            double min_x = std::numeric_limits<double>::infinity();
            double max_x = -std::numeric_limits<double>::infinity();
            double min_y = std::numeric_limits<double>::infinity();
            double max_y = -std::numeric_limits<double>::infinity();
            for (const auto& item : series) {
                for (const auto& point : item.points) {
                    min_x = std::min(min_x, point.x);
                    max_x = std::max(max_x, point.x);
                    min_y = std::min(min_y, point.y);
                    max_y = std::max(max_y, point.y);
                }
            }
            if (!std::isfinite(min_x)) {
                constexpr const char* kEmptyHint = "暂无数据";
                return sao_ui_paint_ctx_draw_utf8(
                    context, static_cast<float>(x + (width - 44) / 2),
                    static_cast<float>(y + (height - 11) / 2), kEmptyHint, 11.0F,
                    sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT_2));
            }
            configure_plot(spec.left_pad_px, spec.right_pad_px, spec.top_pad_px, spec.bottom_pad_px);
            for (int32_t tick = 1; tick < 5; ++tick) {
                const float gx = plot_origin_x + plot_size_w * tick / 5.0F;
                const float gy = plot_origin_y + plot_size_h * tick / 5.0F;
                status = sao_ui_paint_ctx_stroke_line(context, gx, plot_origin_y, gx,
                                                       plot_origin_y + plot_size_h, 1.0F,
                                                       sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER));
                if (status != SAO_STATUS_OK) return status;
                status = sao_ui_paint_ctx_stroke_line(context, plot_origin_x, gy,
                                                       plot_origin_x + plot_size_w, gy, 1.0F,
                                                       sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER));
                if (status != SAO_STATUS_OK) return status;
            }
            // Legend top-right for multi-series line charts.
            if (spec.show_legend && !series.empty()) {
                const float legend_x = static_cast<float>(x + width - 92);
                for (size_t legend_row = 0; legend_row < series.size(); ++legend_row) {
                    const float row_y = static_cast<float>(y + 4) +
                                        static_cast<float>(legend_row) * 13.0F;
                    const uint32_t legend_color =
                        series[legend_row].line_argb == 0
                            ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                            : series[legend_row].line_argb;
                    status = sao_ui_paint_ctx_fill_rect(context, legend_x, row_y, 8.0F, 8.0F,
                                                        legend_color);
                    if (status != SAO_STATUS_OK) return status;
                    status = sao_ui_paint_ctx_draw_utf8(
                        context, legend_x + 12.0F, row_y - 1.0F,
                        series[legend_row].label.c_str(), 9.0F,
                        sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT));
                    if (status != SAO_STATUS_OK) return status;
                }
            }
            // Markers under the lines.
            if (std::any_of(series.begin(), series.end(),
                            [](const OwnedLineSeries& item) { return item.show_markers; })) {
                const double mx_span = max_x > min_x ? max_x - min_x : 1.0;
                const double my_span = max_y > min_y ? max_y - min_y : 1.0;
                for (const auto& item : series) {
                    if (!item.show_markers)
                        continue;
                    const uint32_t marker_color =
                        item.marker_argb == 0
                            ? (item.line_argb == 0
                                   ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                                   : item.line_argb)
                            : item.marker_argb;
                    const size_t stride =
                        item.points.size() > 600U ? (item.points.size() + 599U) / 600U : 1U;
                    for (size_t i = 0; i < item.points.size(); i += stride) {
                        const float px = static_cast<float>(
                            plot_origin_x +
                            plot_size_w * static_cast<float>((item.points[i].x - min_x) / mx_span));
                        const float py = static_cast<float>(
                            plot_origin_y +
                            plot_size_h *
                                (1.0F - static_cast<float>((item.points[i].y - min_y) / my_span)));
                        status = sao_ui_paint_ctx_fill_ellipse(context, px - 2.0F, py - 2.0F,
                                                               4.0F, 4.0F, marker_color);
                        if (status != SAO_STATUS_OK) return status;
                    }
                }
            }
            for (const auto& item : series) {
                std::vector<std::pair<double, double>> points;
                points.reserve(item.points.size());
                for (const auto& point : item.points)
                    points.emplace_back(point.x, point.y);
                status = paint_polyline(
                    points,
                    item.line_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                        : item.line_argb,
                    item.line_width_px, min_x, max_x, min_y, max_y);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            return SAO_STATUS_OK;
        }
        if (kind == kSparklineTag) {
            SaoUiSparklineSpec spec{};
            std::vector<double> values;
            auto lease = acquire_chart_lease<SparklineState>(handle, kSparklineTag);
            if (!lease)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(lease->mtx);
                spec = lease->spec;
                values = lease->values;
            }
            std::vector<std::pair<double, double>> points;
            points.reserve(values.size());
            for (size_t index = 0; index < values.size(); ++index)
                points.emplace_back(static_cast<double>(index), values[index]);
            if (points.empty() || plot_size_w <= 0.0F || plot_size_h <= 0.0F)
                return SAO_STATUS_OK;
            const auto range = std::minmax_element(values.begin(), values.end());
            return paint_polyline(
                points,
                spec.line_argb == 0
                    ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                    : spec.line_argb,
                spec.line_width_px, 0.0,
                static_cast<double>(std::max<size_t>(1, values.size() - 1)), *range.first,
                *range.second);
        }
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
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
