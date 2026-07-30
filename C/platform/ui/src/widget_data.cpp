// SAO Auto — data-display widgets and progress-bar behavior.
//
// This slice implements the ProgressBar portion of widget_data.h:
//   * sao_ui_progress_bar_create / _set_value / _set_max
//
// Plus helper APIs for style-driven fill-ratio, HP-ramp colour
// resolution, and the HP_TRAIL animation tick.  Gauge / badge / tooltip
// / more_indicator are stubbed to NOT_IMPLEMENTED for later slices.
//
// UTF-8 no BOM.

#include "sao/ui/widget_data.h"
#include "sao/ui/widget_kit.h"

#include "hp_bar_widget_internal.h"
#include "panel_theme_internal.h"
#include "widget_paint_internal.h"
#include "widget_typed_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// ── Forward declarations for extended data-family widgets ──────────
// These live in widget_badge.cpp and widget_filter_row.cpp but dispatch
// through the shared data-family apply/restore/paint entry points.
namespace sao::ui::detail {
sao_status_t widget_animated_badge_apply_props(sao_ui_widget_handle_t handle,
                                               const WidgetPropsJson& props,
                                               WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_animated_badge_restore_props(sao_ui_widget_handle_t handle,
                                                 const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_animated_badge_paint(sao_ui_widget_handle_t handle,
                                         sao_ui_paint_ctx_handle_t context,
                                         int32_t x, int32_t y,
                                         int32_t width, int32_t height) noexcept;
sao_status_t widget_filter_row_apply_props(sao_ui_widget_handle_t handle,
                                           const WidgetPropsJson& props,
                                           WidgetPropsSnapshot* out_snapshot) noexcept;
sao_status_t widget_filter_row_restore_props(sao_ui_widget_handle_t handle,
                                             const WidgetPropsSnapshot& snapshot) noexcept;
sao_status_t widget_filter_row_paint(sao_ui_widget_handle_t handle,
                                      sao_ui_paint_ctx_handle_t context,
                                      int32_t x, int32_t y,
                                      int32_t width, int32_t height) noexcept;
}  // namespace sao::ui::detail

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_PROGRESS_FLAT == 0, "progress style enum drifted");
static_assert(SAO_UI_PROGRESS_HP_RAMP == 1, "progress style enum drifted");
static_assert(SAO_UI_PROGRESS_HP_TRAIL == 2, "progress style enum drifted");
static_assert(SAO_UI_PROGRESS_SEGMENTS == 3, "progress style enum drifted");

namespace {

constexpr int32_t kProgressTag = 140; // aligned with SAO_UI_WIDGET_PROGRESS_BAR
constexpr int32_t kGaugeTag = 141;
constexpr int32_t kBadgeTag = 142;
constexpr int32_t kTooltipTag = 143;
constexpr int32_t kMoreTag = 144;
constexpr int32_t kMetricTag = 147;
constexpr int32_t kEmptyStateTag = 148;
constexpr int32_t kAnimatedBadgeTag = 170;  // SAO_UI_WIDGET_ANIMATED_BADGE
constexpr int32_t kFilterRowTag = 171;      // SAO_UI_WIDGET_FILTER_ROW

struct ProgressState {
    int32_t tag{kProgressTag};
    SaoUiProgressBarSpec spec{};
    // Owned segment copy — spec.segments is caller-borrowed only across
    // the create/update call.
    std::vector<SaoUiProgressSegment> segments;
    // Runtime interpolation state.
    float displayed_value{0.0f};
    // HP_TRAIL specific: the trailing decay marker that lags behind
    // displayed_value by trail_lag_ms.  Stays >= displayed_value.
    float trail_value{0.0f};
    // Cached animation target (set by set_value; interpolate_toward
    // reads it every tick).
    float target_value{0.0f};
    float last_value{0.0f};
    int32_t animate_elapsed_ms{0};
    float segment_gap_px{1.0F};
    int32_t segment_pulse_ms{180};
    uint32_t trail_argb{0};
    std::vector<int32_t> segment_pulse_remaining_ms;
    mutable std::mutex mtx;
};

struct GaugeState {
    int32_t tag{kGaugeTag};
    SaoUiGaugeSpec spec{};
    mutable std::mutex mtx;
};

struct BadgeState {
    int32_t tag{kBadgeTag};
    SaoUiStatusBadgeSpec spec{};
    std::string text;
    mutable std::mutex mtx;
};

struct TooltipState {
    int32_t tag{kTooltipTag};
    sao_ui_widget_handle_t target{nullptr};
    SaoUiTooltipSpec spec{};
    std::string text;
    mutable std::mutex mtx;
};

struct MoreState {
    int32_t tag{kMoreTag};
    SaoUiMoreIndicatorSpec spec{};
    std::string noun;
    mutable std::mutex mtx;
};

struct MetricState {
    int32_t tag{kMetricTag};
    SaoUiMetricSpec spec{};
    std::string label;
    std::string value;
    std::string unit;
    mutable std::mutex mtx;
};

struct EmptyState {
    int32_t tag{kEmptyStateTag};
    SaoUiEmptyStateSpec spec{};
    std::string title;
    std::string detail;
    std::string action;
    mutable std::mutex mtx;
};

struct DataPropsSnapshot {
    std::string text;
    float value{};
    float max_value{};
    float displayed_value{};
    float trail_value{};
    float target_value{};
    float last_value{};
    float segment_gap_px{};
    uint32_t trail_argb{};
    int32_t elapsed_ms{};
    int32_t style{};
    int32_t trail_lag_ms{};
    int32_t segment_pulse_ms{};
    int32_t count{};
    std::vector<int32_t> segment_pulse_remaining_ms;
};

template <typename State>
std::shared_ptr<State> as_tagged(sao_ui_widget_handle_t handle, int32_t kind) {
    return std::static_pointer_cast<State>(sao::ui::detail::acquire_widget_handle(
        handle, sao::ui::detail::WidgetHandleFamily::data, kind));
}

std::shared_ptr<ProgressState> as_progress(sao_ui_widget_handle_t handle) {
    return as_tagged<ProgressState>(handle, kProgressTag);
}

template <typename State>
sao_status_t publish_data_state(int32_t kind, std::shared_ptr<State> state,
                                sao_ui_widget_handle_t* out_handle) {
    void* const handle = sao::ui::detail::register_widget_handle(
        sao::ui::detail::WidgetHandleFamily::data, kind, std::move(state));
    if (handle == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(handle);
    return SAO_STATUS_OK;
}

sao_status_t copy_string_to_caller(const std::string& value, char* out_utf8, size_t capacity,
                                   size_t* out_bytes_written) {
    if (out_bytes_written == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_bytes_written = value.size();
    if (out_utf8 == nullptr || capacity <= value.size()) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out_utf8, value.c_str(), value.size() + 1);
    return SAO_STATUS_OK;
}

float clamp_ratio(float v, float lo = 0.0f, float hi = 1.0f) {
    return std::max(lo, std::min(hi, v));
}

uint32_t scale_alpha(uint32_t color, float scale) {
    const uint32_t alpha = static_cast<uint32_t>(
        std::lround(static_cast<float>((color >> 24U) & 0xffU) * std::clamp(scale, 0.0F, 1.0F)));
    return (color & 0x00ffffffU) | (alpha << 24U);
}

uint32_t progress_ramp_color(const SaoUiProgressBarSpec& spec, float ratio) {
    const uint32_t low = spec.fill_low_argb == 0
                             ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_HP_RED_R)
                             : spec.fill_low_argb;
    const uint32_t middle = spec.fill_mid_argb == 0
                                ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_HP_YELLOW_R)
                                : spec.fill_mid_argb;
    const uint32_t high = spec.fill_high_argb == 0
                              ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_HP_GREEN_R)
                              : spec.fill_high_argb;
    return sao::ui::detail::hp_bar_ramp_color(low, middle, high, ratio);
}

void pulse_changed_segments_no_lock(ProgressState& state, float previous, float next) {
    if (state.spec.style != SAO_UI_PROGRESS_SEGMENTS || state.segments.empty() ||
        state.segment_pulse_ms <= 0 || state.spec.max_value <= 0.0F) {
        return;
    }
    if (state.segment_pulse_remaining_ms.size() != state.segments.size())
        state.segment_pulse_remaining_ms.assign(state.segments.size(), 0);
    const float previous_ratio = clamp_ratio(previous / state.spec.max_value);
    const float next_ratio = clamp_ratio(next / state.spec.max_value);
    const float changed_start = std::min(previous_ratio, next_ratio);
    const float changed_end = std::max(previous_ratio, next_ratio);
    for (size_t index = 0; index < state.segments.size(); ++index) {
        const float start = clamp_ratio(state.segments[index].fraction_start);
        const float end = clamp_ratio(state.segments[index].fraction_end);
        const bool overlaps = changed_end > changed_start
                                  ? end > changed_start && start < changed_end
                                  : next_ratio >= start && next_ratio <= end;
        if (overlaps)
            state.segment_pulse_remaining_ms[index] = state.segment_pulse_ms;
    }
}

void set_progress_value_no_lock(ProgressState& state, float value) {
    const float previous = state.spec.value;
    state.last_value = previous;
    pulse_changed_segments_no_lock(state, previous, value);
    if (state.spec.animate_duration_ms > 0) {
        state.target_value = value;
        state.animate_elapsed_ms = 0;
    } else {
        state.displayed_value = value;
        state.target_value = value;
        if (state.spec.style == SAO_UI_PROGRESS_HP_TRAIL)
            state.trail_value = std::max(state.trail_value, value);
        else
            state.trail_value = value;
    }
    state.spec.value = value;
}

sao_status_t paint_rounded_box(sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
                               int32_t width, int32_t height, uint32_t fill, uint32_t border,
                               int32_t radius_px) noexcept {
    const float radius = static_cast<float>(std::max(0, radius_px));
    sao_status_t status = sao::ui::detail::paint_rounded_rect(
        context, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
        static_cast<float>(height), radius, border);
    if (status != SAO_STATUS_OK || width <= 2 || height <= 2)
        return status;
    return sao::ui::detail::paint_rounded_rect(
        context, static_cast<float>(x + 1), static_cast<float>(y + 1),
        static_cast<float>(width - 2), static_cast<float>(height - 2),
        std::max(0.0F, radius - 1.0F), fill);
}

void apply_progress_spec_no_lock(ProgressState& s, const SaoUiProgressBarSpec* spec) {
    s.spec = *spec;
    if (spec->segments != nullptr && spec->segment_count > 0) {
        s.segments.assign(spec->segments, spec->segments + spec->segment_count);
        s.spec.segments = s.segments.data();
        s.spec.segment_count = s.segments.size();
    } else {
        s.segments.clear();
        s.spec.segments = nullptr;
        s.spec.segment_count = 0;
    }
    s.displayed_value = spec->value;
    s.target_value = spec->value;
    s.trail_value = spec->value;
    s.last_value = spec->value;
    s.animate_elapsed_ms = 0;
    s.segment_pulse_remaining_ms.assign(s.segments.size(), 0);
}

} // namespace

// ---------------------------------------------------------------------------
// Progress bar ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_progress_bar_create(void* /*d3d_device_ptr*/,
                                                               const SaoUiProgressBarSpec* spec,
                                                               sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    try {
        auto state = std::make_shared<ProgressState>();
        apply_progress_spec_no_lock(*state, spec);
        return publish_data_state(kProgressTag, std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_progress_bar_set_value(sao_ui_widget_handle_t handle,
                                                                  float value) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    set_progress_value_no_lock(*s, value);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_progress_bar_set_max(sao_ui_widget_handle_t handle,
                                                                float max_value) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->spec.max_value = max_value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gauge_create(void*, const SaoUiGaugeSpec* spec,
                                                        sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<GaugeState> state;
    try {
        state = std::make_shared<GaugeState>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state->spec = *spec;
    if (state->spec.max_value < 0.0f)
        state->spec.max_value = 0.0f;
    return publish_data_state(kGaugeTag, std::move(state), out);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gauge_set_value(sao_ui_widget_handle_t handle,
                                                           float value) {
    auto state = as_tagged<GaugeState>(handle, kGaugeTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(value))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.value = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gauge_get_ratio(sao_ui_widget_handle_t handle,
                                                           float* out_ratio) {
    auto state = as_tagged<GaugeState>(handle, kGaugeTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_ratio == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_ratio = state->spec.max_value <= 0.0f
                     ? 0.0f
                     : clamp_ratio(state->spec.value / state->spec.max_value);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_status_badge_create(void*,
                                                               const SaoUiStatusBadgeSpec* spec,
                                                               sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<BadgeState> state;
    try {
        state = std::make_shared<BadgeState>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state->spec = *spec;
    state->text = spec->text_utf8 == nullptr ? "" : spec->text_utf8;
    state->spec.text_utf8 = nullptr;
    return publish_data_state(kBadgeTag, std::move(state), out);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_status_badge_set_text(sao_ui_widget_handle_t handle,
                                                                 const char* text) {
    auto state = as_tagged<BadgeState>(handle, kBadgeTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (text == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->text = text;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_status_badge_get_text(sao_ui_widget_handle_t handle,
                                                                 char* out_utf8, size_t capacity,
                                                                 size_t* out_bytes_written) {
    auto state = as_tagged<BadgeState>(handle, kBadgeTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    return copy_string_to_caller(state->text, out_utf8, capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tooltip_attach(sao_ui_widget_handle_t target,
                                                          const SaoUiTooltipSpec* spec,
                                                          sao_ui_widget_handle_t* out) {
    if (target == nullptr || spec == nullptr || out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    std::shared_ptr<TooltipState> state;
    try {
        state = std::make_shared<TooltipState>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state->target = target;
    state->spec = *spec;
    state->text = spec->text_utf8 == nullptr ? "" : spec->text_utf8;
    state->spec.text_utf8 = nullptr;
    return publish_data_state(kTooltipTag, std::move(state), out);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tooltip_set_text(sao_ui_widget_handle_t handle,
                                                            const char* text) {
    auto state = as_tagged<TooltipState>(handle, kTooltipTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (text == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->text = text;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_more_indicator_create(void*,
                                                                 const SaoUiMoreIndicatorSpec* spec,
                                                                 sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<MoreState> state;
    try {
        state = std::make_shared<MoreState>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state->spec = *spec;
    state->spec.hidden_count = std::max(0, spec->hidden_count);
    state->noun = spec->noun_utf8 == nullptr ? "条" : spec->noun_utf8;
    state->spec.noun_utf8 = nullptr;
    return publish_data_state(kMoreTag, std::move(state), out);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_more_indicator_set_count(sao_ui_widget_handle_t handle,
                                                                    int32_t hidden_count) {
    auto state = as_tagged<MoreState>(handle, kMoreTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (hidden_count < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.hidden_count = hidden_count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_more_indicator_get_count(sao_ui_widget_handle_t handle,
                                                                    int32_t* out_hidden_count) {
    auto state = as_tagged<MoreState>(handle, kMoreTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_hidden_count == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_hidden_count = state->spec.hidden_count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_metric_create(void*, const SaoUiMetricSpec* spec,
                                                         sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<MetricState> state;
    try {
        state = std::make_shared<MetricState>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state->spec = *spec;
    state->label = spec->label_utf8 == nullptr ? "" : spec->label_utf8;
    state->value = spec->value_utf8 == nullptr ? "" : spec->value_utf8;
    state->unit = spec->unit_utf8 == nullptr ? "" : spec->unit_utf8;
    state->spec.label_utf8 = nullptr;
    state->spec.value_utf8 = nullptr;
    state->spec.unit_utf8 = nullptr;
    return publish_data_state(kMetricTag, std::move(state), out);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_metric_set_value(sao_ui_widget_handle_t handle,
                                                            const char* value) {
    auto state = as_tagged<MetricState>(handle, kMetricTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (value == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->value = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_metric_get_value(sao_ui_widget_handle_t handle,
                                                            char* out_utf8, size_t capacity,
                                                            size_t* out_bytes_written) {
    auto state = as_tagged<MetricState>(handle, kMetricTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    return copy_string_to_caller(state->value, out_utf8, capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_empty_state_create(void*,
                                                              const SaoUiEmptyStateSpec* spec,
                                                              sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<EmptyState> state;
    try {
        state = std::make_shared<EmptyState>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    state->spec = *spec;
    state->title = spec->title_utf8 == nullptr ? "" : spec->title_utf8;
    state->detail = spec->detail_utf8 == nullptr ? "" : spec->detail_utf8;
    state->action = spec->action_utf8 == nullptr ? "" : spec->action_utf8;
    state->spec.title_utf8 = nullptr;
    state->spec.detail_utf8 = nullptr;
    state->spec.action_utf8 = nullptr;
    return publish_data_state(kEmptyStateTag, std::move(state), out);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_empty_state_set_detail(sao_ui_widget_handle_t handle,
                                                                  const char* detail) {
    auto state = as_tagged<EmptyState>(handle, kEmptyStateTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (detail == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->detail = detail;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_empty_state_get_detail(sao_ui_widget_handle_t handle,
                                                                  char* out_utf8, size_t capacity,
                                                                  size_t* out_bytes_written) {
    auto state = as_tagged<EmptyState>(handle, kEmptyStateTag);
    if (state == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    return copy_string_to_caller(state->detail, out_utf8, capacity, out_bytes_written);
}

// ---------------------------------------------------------------------------
// Progress-bar helper API — fill ratio, style-aware colour resolution, and
// the HP_TRAIL animation tick.  Not part of widget_data.h yet.
// ---------------------------------------------------------------------------

// clamp(value / max, 0..1).  Returns 0 when max == 0 to keep
// downstream painters safe from division by zero.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_fill_ratio(sao_ui_widget_handle_t handle, float* out_ratio) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const float max_v = s->spec.max_value;
    float ratio = 0.0f;
    if (max_v > 0.0f) {
        ratio = clamp_ratio(s->displayed_value / max_v);
    }
    if (out_ratio)
        *out_ratio = ratio;
    return SAO_STATUS_OK;
}

// Same but for the trailing decay marker (only meaningful for HP_TRAIL).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_trail_ratio(sao_ui_widget_handle_t handle, float* out_ratio) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const float max_v = s->spec.max_value;
    float ratio = 0.0f;
    if (max_v > 0.0f) {
        ratio = clamp_ratio(s->trail_value / max_v);
    }
    if (out_ratio)
        *out_ratio = ratio;
    return SAO_STATUS_OK;
}

// Resolve the current fill colour under the widget's style rules.
// FLAT / HP_TRAIL fall back to spec.fill_argb; HP_RAMP picks from the
// hp_bar.py ramp table (< 25% → low, < 50% → mid, else high).  For
// SEGMENTS the caller iterates over segments themselves; we return
// spec.fill_argb as a base.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_resolve_fill_argb(sao_ui_widget_handle_t handle, uint32_t* out_argb) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const float max_v = s->spec.max_value;
    float ratio = 0.0f;
    if (max_v > 0.0f)
        ratio = clamp_ratio(s->displayed_value / max_v);
    uint32_t argb = s->spec.fill_argb;
    if (s->spec.style == SAO_UI_PROGRESS_HP_RAMP)
        argb = progress_ramp_color(s->spec, ratio);
    if (out_argb)
        *out_argb = argb;
    return SAO_STATUS_OK;
}

// Advance the HP_TRAIL animation.  For FLAT/HP_RAMP the trail just
// tracks displayed; for HP_TRAIL the trail decays linearly toward
// displayed over trail_lag_ms.  Also progresses any value animation
// begun by set_value when animate_duration_ms > 0.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_tick(sao_ui_widget_handle_t handle, int32_t dt_ms) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dt_ms <= 0)
        return SAO_STATUS_OK;
    std::lock_guard<std::mutex> lk(s->mtx);
    // 1) value animation.
    if (s->spec.animate_duration_ms > 0 &&
        std::fabs(s->target_value - s->displayed_value) > 1e-6f) {
        s->animate_elapsed_ms += dt_ms;
        const float t = std::min(1.0f, static_cast<float>(s->animate_elapsed_ms) /
                                           static_cast<float>(s->spec.animate_duration_ms));
        s->displayed_value = s->displayed_value + (s->target_value - s->displayed_value) * t;
        if (t >= 1.0f) {
            s->displayed_value = s->target_value;
            s->animate_elapsed_ms = 0;
        }
    }
    // 2) HP_TRAIL trail decay.
    if (s->spec.style == SAO_UI_PROGRESS_HP_TRAIL) {
        if (s->trail_value > s->displayed_value) {
            const int32_t lag_ms = s->spec.trail_lag_ms > 0 ? s->spec.trail_lag_ms : 280;
            const float span = s->trail_value - s->displayed_value;
            const float step = span * (static_cast<float>(dt_ms) / static_cast<float>(lag_ms));
            s->trail_value = std::max(s->displayed_value, s->trail_value - step);
        } else if (s->trail_value < s->displayed_value) {
            // Heal: trail snaps up to displayed immediately.
            s->trail_value = s->displayed_value;
        }
    } else {
        s->trail_value = s->displayed_value;
    }
    for (int32_t& remaining : s->segment_pulse_remaining_ms)
        remaining = std::max(0, remaining - dt_ms);
    return SAO_STATUS_OK;
}

// Query segment count and fetch a segment by index (for SEGMENTS style).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_progress_get_segment(
    sao_ui_widget_handle_t handle, size_t index, SaoUiProgressSegment* out_segment) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (index >= s->segments.size()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_segment)
        *out_segment = s->segments[index];
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_widget_progress_get_segment_count(sao_ui_widget_handle_t handle) {
    auto s = as_progress(handle);
    if (s == nullptr)
        return 0;
    std::lock_guard<std::mutex> lk(s->mtx);
    return s->segments.size();
}

sao_status_t sao::ui::detail::widget_data_apply_props(sao_ui_widget_handle_t handle, int32_t kind,
                                                      const WidgetPropsJson& props,
                                                      WidgetPropsSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    try {
        auto snapshot = std::make_shared<DataPropsSnapshot>();
        switch (kind) {
        case kProgressTag: {
            if (!widget_props_has_only(props, {"value", "max_value", "style", "trail_argb",
                                               "trail_lag_ms", "gap_px", "segment_pulse_ms"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            float value = 0.0F;
            float maximum = 0.0F;
            float gap_px = 0.0F;
            uint32_t trail_argb = 0;
            int32_t style = SAO_UI_PROGRESS_FLAT;
            int32_t trail_lag_ms = 0;
            int32_t segment_pulse_ms = 0;
            bool has_value = false;
            bool has_maximum = false;
            bool has_style = false;
            bool has_trail_argb = false;
            bool has_trail_lag = false;
            bool has_gap = false;
            bool has_segment_pulse = false;
            const auto value_property = props.find("value");
            if (value_property != props.end()) {
                if (!widget_props_float(*value_property, &value))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                has_value = true;
            }
            const auto maximum_property = props.find("max_value");
            if (maximum_property != props.end()) {
                if (!widget_props_float(*maximum_property, &maximum) || maximum < 0.0F)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                has_maximum = true;
            }
            const auto style_property = props.find("style");
            if (style_property != props.end()) {
                if (style_property->is_number_integer()) {
                    if (!widget_props_i32(*style_property, &style))
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                } else if (style_property->is_string()) {
                    const std::string name = style_property->get<std::string>();
                    if (name == "flat")
                        style = SAO_UI_PROGRESS_FLAT;
                    else if (name == "hp_ramp")
                        style = SAO_UI_PROGRESS_HP_RAMP;
                    else if (name == "hp_trail")
                        style = SAO_UI_PROGRESS_HP_TRAIL;
                    else if (name == "segments")
                        style = SAO_UI_PROGRESS_SEGMENTS;
                    else
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                } else {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                if (style < SAO_UI_PROGRESS_FLAT || style > SAO_UI_PROGRESS_SEGMENTS)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                has_style = true;
            }
            const auto trail_color_property = props.find("trail_argb");
            if (trail_color_property != props.end()) {
                if (!widget_props_argb(*trail_color_property, &trail_argb))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                has_trail_argb = true;
            }
            const auto trail_lag_property = props.find("trail_lag_ms");
            if (trail_lag_property != props.end()) {
                if (!widget_props_i32(*trail_lag_property, &trail_lag_ms) || trail_lag_ms < 0 ||
                    trail_lag_ms > 60000) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                has_trail_lag = true;
            }
            const auto gap_property = props.find("gap_px");
            if (gap_property != props.end()) {
                if (!widget_props_float(*gap_property, &gap_px) || gap_px < 0.0F ||
                    gap_px > 64.0F) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                has_gap = true;
            }
            const auto pulse_property = props.find("segment_pulse_ms");
            if (pulse_property != props.end()) {
                if (!widget_props_i32(*pulse_property, &segment_pulse_ms) || segment_pulse_ms < 0 ||
                    segment_pulse_ms > 60000) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                has_segment_pulse = true;
            }
            auto state = as_progress(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->value = state->spec.value;
                snapshot->max_value = state->spec.max_value;
                snapshot->displayed_value = state->displayed_value;
                snapshot->trail_value = state->trail_value;
                snapshot->target_value = state->target_value;
                snapshot->last_value = state->last_value;
                snapshot->elapsed_ms = state->animate_elapsed_ms;
                snapshot->style = state->spec.style;
                snapshot->trail_lag_ms = state->spec.trail_lag_ms;
                snapshot->segment_gap_px = state->segment_gap_px;
                snapshot->segment_pulse_ms = state->segment_pulse_ms;
                snapshot->trail_argb = state->trail_argb;
                snapshot->segment_pulse_remaining_ms = state->segment_pulse_remaining_ms;
                if (has_maximum)
                    state->spec.max_value = maximum;
                if (has_style)
                    state->spec.style = style;
                if (has_trail_argb)
                    state->trail_argb = trail_argb;
                if (has_trail_lag)
                    state->spec.trail_lag_ms = trail_lag_ms;
                if (has_gap)
                    state->segment_gap_px = gap_px;
                if (has_segment_pulse)
                    state->segment_pulse_ms = segment_pulse_ms;
                if (has_value)
                    set_progress_value_no_lock(*state, value);
            }
            break;
        }
        case kGaugeTag: {
            if (!widget_props_has_only(props, {"value"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto state = as_tagged<GaugeState>(handle, kGaugeTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->value = state->spec.value;
            }
            const auto value_property = props.find("value");
            if (value_property != props.end()) {
                float value = 0.0F;
                if (!widget_props_float(*value_property, &value))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const sao_status_t status = sao_ui_gauge_set_value(handle, value);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kBadgeTag:
        case kTooltipTag:
        case kMetricTag:
        case kEmptyStateTag: {
            const char* key = kind == kMetricTag       ? "value"
                              : kind == kEmptyStateTag ? "detail"
                                                       : "text";
            if (!widget_props_has_only(props, {key}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            const auto replacement = props.find(key);
            std::string text;
            if (replacement != props.end()) {
                if (!replacement->is_string())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                text = replacement->get<std::string>();
            }
            if (kind == kBadgeTag) {
                auto state = as_tagged<BadgeState>(handle, kBadgeTag);
                if (state == nullptr)
                    return SAO_STATUS_ERR_HANDLE_INVALID;
                {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    snapshot->text = state->text;
                }
                if (replacement != props.end()) {
                    const sao_status_t status = sao_ui_status_badge_set_text(handle, text.c_str());
                    if (status != SAO_STATUS_OK)
                        return status;
                }
            } else if (kind == kTooltipTag) {
                auto state = as_tagged<TooltipState>(handle, kTooltipTag);
                if (state == nullptr)
                    return SAO_STATUS_ERR_HANDLE_INVALID;
                {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    snapshot->text = state->text;
                }
                if (replacement != props.end()) {
                    const sao_status_t status = sao_ui_tooltip_set_text(handle, text.c_str());
                    if (status != SAO_STATUS_OK)
                        return status;
                }
            } else if (kind == kMetricTag) {
                auto state = as_tagged<MetricState>(handle, kMetricTag);
                if (state == nullptr)
                    return SAO_STATUS_ERR_HANDLE_INVALID;
                {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    snapshot->text = state->value;
                }
                if (replacement != props.end()) {
                    const sao_status_t status = sao_ui_metric_set_value(handle, text.c_str());
                    if (status != SAO_STATUS_OK)
                        return status;
                }
            } else {
                auto state = as_tagged<EmptyState>(handle, kEmptyStateTag);
                if (state == nullptr)
                    return SAO_STATUS_ERR_HANDLE_INVALID;
                {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    snapshot->text = state->detail;
                }
                if (replacement != props.end()) {
                    const sao_status_t status = sao_ui_empty_state_set_detail(handle, text.c_str());
                    if (status != SAO_STATUS_OK)
                        return status;
                }
            }
            break;
        }
        case kMoreTag: {
            if (!widget_props_has_only(props, {"hidden_count"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto state = as_tagged<MoreState>(handle, kMoreTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->count = state->spec.hidden_count;
            }
            const auto count_property = props.find("hidden_count");
            if (count_property != props.end()) {
                int32_t count = 0;
                if (!widget_props_i32(*count_property, &count) || count < 0)
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const sao_status_t status = sao_ui_more_indicator_set_count(handle, count);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kAnimatedBadgeTag: {
            if (!widget_props_has_only(props, {"count", "dot_radius", "pulse_ms",
                                               "font_size", "pad_x", "pad_y", "radius",
                                               "fill", "fg", "border", "pulse", "theme"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            return sao::ui::detail::widget_animated_badge_apply_props(handle, props, out_snapshot);
        }
        case kFilterRowTag: {
            if (!widget_props_has_only(props, {"query", "chips", "placeholder",
                                               "search_width", "chip_height", "chip_gap",
                                               "font_size", "pad_x", "pad_y", "radius",
                                               "bg", "fg", "border", "chip_bg", "chip_selected",
                                               "chip_fg", "chip_selected_fg", "theme"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            return sao::ui::detail::widget_filter_row_apply_props(handle, props, out_snapshot);
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

sao_status_t
sao::ui::detail::widget_data_restore_props(sao_ui_widget_handle_t handle, int32_t kind,
                                           const WidgetPropsSnapshot& snapshot) noexcept {
    const auto previous = std::static_pointer_cast<DataPropsSnapshot>(snapshot);
    if (previous == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        switch (kind) {
        case kProgressTag: {
            auto state = as_progress(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(state->mtx);
            state->spec.value = previous->value;
            state->spec.max_value = previous->max_value;
            state->displayed_value = previous->displayed_value;
            state->trail_value = previous->trail_value;
            state->target_value = previous->target_value;
            state->last_value = previous->last_value;
            state->animate_elapsed_ms = previous->elapsed_ms;
            state->spec.style = previous->style;
            state->spec.trail_lag_ms = previous->trail_lag_ms;
            state->segment_gap_px = previous->segment_gap_px;
            state->segment_pulse_ms = previous->segment_pulse_ms;
            state->trail_argb = previous->trail_argb;
            state->segment_pulse_remaining_ms = previous->segment_pulse_remaining_ms;
            return SAO_STATUS_OK;
        }
        case kGaugeTag:
            return sao_ui_gauge_set_value(handle, previous->value);
        case kBadgeTag:
            return sao_ui_status_badge_set_text(handle, previous->text.c_str());
        case kTooltipTag:
            return sao_ui_tooltip_set_text(handle, previous->text.c_str());
        case kMoreTag:
            return sao_ui_more_indicator_set_count(handle, previous->count);
        case kMetricTag:
            return sao_ui_metric_set_value(handle, previous->text.c_str());
        case kEmptyStateTag:
            return sao_ui_empty_state_set_detail(handle, previous->text.c_str());
        case kAnimatedBadgeTag:
            return sao::ui::detail::widget_animated_badge_restore_props(handle, snapshot);
        case kFilterRowTag:
            return sao::ui::detail::widget_filter_row_restore_props(handle, snapshot);
        default:
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t sao::ui::detail::widget_data_paint(sao_ui_widget_handle_t handle, int32_t kind,
                                                sao_ui_paint_ctx_handle_t context, int32_t x,
                                                int32_t y, int32_t width, int32_t height) noexcept {
    try {
        switch (kind) {
        case kProgressTag: {
            SaoUiProgressBarSpec spec{};
            std::vector<SaoUiProgressSegment> segments;
            std::vector<int32_t> segment_pulses;
            float displayed = 0.0F;
            float trail_value = 0.0F;
            float gap_px = 0.0F;
            int32_t segment_pulse_ms = 0;
            uint32_t trail_argb = 0;
            auto state = as_progress(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
                displayed = state->displayed_value;
                trail_value = state->trail_value;
                segments = state->segments;
                segment_pulses = state->segment_pulse_remaining_ms;
                gap_px = state->segment_gap_px;
                segment_pulse_ms = state->segment_pulse_ms;
                trail_argb = state->trail_argb;
            }
            const float ratio =
                spec.max_value <= 0.0F ? 0.0F : clamp_ratio(displayed / spec.max_value);
            const uint32_t background =
                spec.bg_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_CARD)
                                  : spec.bg_argb;
            sao_status_t status = sao::ui::detail::paint_rounded_rect(
                context, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                static_cast<float>(height), static_cast<float>(std::max(0, spec.radius_px)),
                background);
            if (status != SAO_STATUS_OK)
                return status;
            if (spec.style == SAO_UI_PROGRESS_SEGMENTS && !segments.empty()) {
                for (size_t index = 0; index < segments.size(); ++index) {
                    const auto& segment = segments[index];
                    const float start = std::clamp(segment.fraction_start, 0.0F, ratio);
                    const float end = std::clamp(segment.fraction_end, 0.0F, ratio);
                    if (end <= start)
                        continue;
                    const float left_gap = start > 0.0F ? gap_px * 0.5F : 0.0F;
                    const float right_gap = end < ratio ? gap_px * 0.5F : 0.0F;
                    const float left = static_cast<float>(x) + width * start + left_gap;
                    const float right = static_cast<float>(x) + width * end - right_gap;
                    if (right <= left)
                        continue;
                    uint32_t color =
                        segment.fill_argb == 0
                            ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                            : segment.fill_argb;
                    if (segment_pulse_ms > 0 && index < segment_pulses.size() &&
                        segment_pulses[index] > 0) {
                        const float phase = 1.0F - static_cast<float>(segment_pulses[index]) /
                                                       static_cast<float>(segment_pulse_ms);
                        const float alpha_scale =
                            1.0F - 0.35F * std::sin(phase * 3.14159265358979323846F);
                        color = scale_alpha(color, alpha_scale);
                    }
                    status =
                        sao_ui_paint_ctx_fill_rect(context, left, static_cast<float>(y),
                                                   right - left, static_cast<float>(height), color);
                    if (status != SAO_STATUS_OK)
                        return status;
                }
                return SAO_STATUS_OK;
            }
            uint32_t fill = spec.fill_argb == 0
                                ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                                : spec.fill_argb;
            if (spec.style == SAO_UI_PROGRESS_HP_RAMP) {
                const uint32_t low =
                    spec.fill_low_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_HP_RED_R)
                        : spec.fill_low_argb;
                const uint32_t middle =
                    spec.fill_mid_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_HP_YELLOW_R)
                        : spec.fill_mid_argb;
                const uint32_t high =
                    spec.fill_high_argb == 0
                        ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_HP_GREEN_R)
                        : spec.fill_high_argb;
                return sao::ui::detail::paint_hp_bar(
                    context, x, y, width, height, ratio, ratio, low, middle, high, 0u,
                    std::max(0, spec.radius_px), std::max(0, spec.leading_skew_px));
            }
            if (spec.style == SAO_UI_PROGRESS_HP_TRAIL && spec.max_value > 0.0F) {
                const float trail_ratio = clamp_ratio(trail_value / spec.max_value);
                const uint32_t trail =
                    trail_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_GOLD)
                                    : trail_argb;
                return sao::ui::detail::paint_hp_bar(
                    context, x, y, width, height, ratio, trail_ratio, fill, fill, fill, trail,
                    std::max(0, spec.radius_px), std::max(0, spec.leading_skew_px));
            }
            if (ratio <= 0.0F)
                return SAO_STATUS_OK;
            return sao::ui::detail::paint_rounded_rect(
                context, static_cast<float>(x), static_cast<float>(y),
                std::max(1.0F, width * ratio), static_cast<float>(height),
                static_cast<float>(std::max(0, spec.radius_px)), fill);
        }
        case kGaugeTag: {
            SaoUiGaugeSpec spec{};
            auto state = as_tagged<GaugeState>(handle, kGaugeTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
            }
            const float ratio =
                spec.max_value <= 0.0F ? 0.0F : clamp_ratio(spec.value / spec.max_value);
            const int32_t diameter = std::max(1, std::min(width, height));
            const int32_t left = x + (width - diameter) / 2;
            const int32_t top = y + (height - diameter) / 2;
            sao_status_t status = sao_ui_paint_ctx_fill_rect(
                context, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                static_cast<float>(height),
                spec.center_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BG)
                                      : spec.center_argb);
            if (status != SAO_STATUS_OK)
                return status;
            status = sao_ui_paint_ctx_fill_ellipse(
                context, static_cast<float>(left), static_cast<float>(top),
                static_cast<float>(diameter), static_cast<float>(diameter),
                spec.track_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER)
                                     : spec.track_argb);
            if (status != SAO_STATUS_OK)
                return status;
            const int32_t inner = std::max(1, diameter - std::max(2, spec.thickness_px) * 2);
            status = sao_ui_paint_ctx_fill_ellipse(
                context, static_cast<float>(left + (diameter - inner) / 2),
                static_cast<float>(top + (diameter - inner) / 2), static_cast<float>(inner),
                static_cast<float>(inner),
                spec.center_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BG)
                                      : spec.center_argb);
            if (status != SAO_STATUS_OK || ratio <= 0.0F)
                return status;
            const int32_t indicator = std::max(2, static_cast<int32_t>(std::lround(inner * ratio)));
            return sao_ui_paint_ctx_fill_ellipse(
                context, static_cast<float>(left + (diameter - indicator) / 2),
                static_cast<float>(top + (diameter - indicator) / 2), static_cast<float>(indicator),
                static_cast<float>(indicator),
                spec.fill_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_ACCENT)
                                    : spec.fill_argb);
        }
        case kBadgeTag: {
            SaoUiStatusBadgeSpec spec{};
            std::string text;
            auto state = as_tagged<BadgeState>(handle, kBadgeTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
                text = state->text;
            }
            const int32_t radius =
                spec.radius_px > 0 ? spec.radius_px
                                   : std::min(height / 2, sao::ui::detail::panel_theme_metric(
                                                              SAO_UI_METRIC_BORDER_RADIUS_MEDIUM));
            sao_status_t status = paint_rounded_box(
                context, x, y, width, height,
                spec.fill_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_CARD)
                                    : spec.fill_argb,
                spec.border_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER)
                                      : spec.border_argb,
                radius);
            if (status != SAO_STATUS_OK)
                return status;
            return sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + std::max(2, spec.pad_x_px)),
                static_cast<float>(y + std::max(2, spec.pad_y_px)), text.c_str(),
                static_cast<float>(spec.font_size_px > 0 ? spec.font_size_px : 12),
                spec.fg_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT)
                                  : spec.fg_argb);
        }
        case kTooltipTag: {
            SaoUiTooltipSpec spec{};
            std::string text;
            auto state = as_tagged<TooltipState>(handle, kTooltipTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
                text = state->text;
            }
            sao_status_t status = paint_rounded_box(
                context, x, y, width, height,
                spec.bg_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_CARD)
                                  : spec.bg_argb,
                spec.border_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_BORDER)
                                      : spec.border_argb,
                sao::ui::detail::panel_theme_metric(SAO_UI_METRIC_BORDER_RADIUS_MEDIUM));
            if (status != SAO_STATUS_OK)
                return status;
            return sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + std::max(2, spec.pad_x_px)),
                static_cast<float>(y + std::max(2, spec.pad_y_px)), text.c_str(),
                static_cast<float>(spec.font_size_px > 0 ? spec.font_size_px : 12),
                spec.fg_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT)
                                  : spec.fg_argb);
        }
        case kMoreTag: {
            SaoUiMoreIndicatorSpec spec{};
            std::string noun;
            auto state = as_tagged<MoreState>(handle, kMoreTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
                noun = state->noun;
            }
            const std::string text = "... " + std::to_string(spec.hidden_count) + " " + noun;
            return sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + std::max(2, spec.pad_x_px)),
                static_cast<float>(y + std::max(2, spec.pad_y_px)), text.c_str(),
                static_cast<float>(spec.font_size_px > 0 ? spec.font_size_px : 12),
                spec.fg_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT_2)
                                  : spec.fg_argb);
        }
        case kMetricTag: {
            SaoUiMetricSpec spec{};
            std::string label;
            std::string value;
            std::string unit;
            auto state = as_tagged<MetricState>(handle, kMetricTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
                label = state->label;
                value = state->value;
                unit = state->unit;
            }
            sao_status_t status = sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + 2), static_cast<float>(y + 2), label.c_str(), 10.0F,
                spec.label_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT_2)
                                     : spec.label_argb);
            if (status != SAO_STATUS_OK)
                return status;
            const std::string displayed = value + (unit.empty() ? "" : " " + unit);
            return sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + 2), static_cast<float>(y + height / 2),
                displayed.c_str(),
                static_cast<float>(spec.value_font_size_px > 0 ? spec.value_font_size_px : 14),
                spec.value_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT)
                                     : spec.value_argb);
        }
        case kEmptyStateTag: {
            SaoUiEmptyStateSpec spec{};
            std::string title;
            std::string detail;
            auto state = as_tagged<EmptyState>(handle, kEmptyStateTag);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                spec = state->spec;
                title = state->title;
                detail = state->detail;
            }
            sao_status_t status = sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + 2), static_cast<float>(y + 2), title.c_str(), 14.0F,
                spec.title_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT)
                                     : spec.title_argb);
            if (status != SAO_STATUS_OK)
                return status;
            return sao_ui_paint_ctx_draw_utf8(
                context, static_cast<float>(x + 2), static_cast<float>(y + height / 2),
                detail.c_str(), 11.0F,
                spec.detail_argb == 0 ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT_2)
                                      : spec.detail_argb);
        }
        case kAnimatedBadgeTag:
            return sao::ui::detail::widget_animated_badge_paint(handle, context, x, y, width, height);
        case kFilterRowTag:
            return sao::ui::detail::widget_filter_row_paint(handle, context, x, y, width, height);
        default:
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// Shared destroy helper.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr)
        return;
    auto state =
        sao::ui::detail::retire_widget_handle(handle, sao::ui::detail::WidgetHandleFamily::data);
    if (state == nullptr)
        return;
    uint32_t removed = 0;
    (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);
}
