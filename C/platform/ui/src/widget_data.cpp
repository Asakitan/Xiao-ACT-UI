// SAO Auto — data-display widgets first slice (Wave 4 / Agent d, G3.8).
//
// This slice implements the ProgressBar portion of widget_data.h:
//   * sao_ui_progress_bar_create / _set_value / _set_max
//
// Plus wave4 helper API for style-driven fill-ratio, HP-ramp colour
// resolution, and the HP_TRAIL animation tick.  Gauge / badge / tooltip
// / more_indicator are stubbed to NOT_IMPLEMENTED for later slices.
//
// UTF-8 no BOM.

#include "sao/ui/widget_data.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_PROGRESS_FLAT      == 0, "progress style enum drifted");
static_assert(SAO_UI_PROGRESS_HP_RAMP   == 1, "progress style enum drifted");
static_assert(SAO_UI_PROGRESS_HP_TRAIL  == 2, "progress style enum drifted");
static_assert(SAO_UI_PROGRESS_SEGMENTS  == 3, "progress style enum drifted");

namespace {

constexpr int32_t kProgressTag = 140;   // aligned with SAO_UI_WIDGET_PROGRESS_BAR
constexpr int32_t kGaugeTag = 141;
constexpr int32_t kBadgeTag = 142;
constexpr int32_t kTooltipTag = 143;
constexpr int32_t kMoreTag = 144;
constexpr int32_t kMetricTag = 147;
constexpr int32_t kEmptyStateTag = 148;

struct ProgressState {
    int32_t                             tag{kProgressTag};
    SaoUiProgressBarSpec                spec{};
    // Owned segment copy — spec.segments is caller-borrowed only across
    // the create/update call.
    std::vector<SaoUiProgressSegment>   segments;
    // Runtime interpolation state.
    float                               displayed_value{0.0f};
    // HP_TRAIL specific: the trailing decay marker that lags behind
    // displayed_value by trail_lag_ms.  Stays >= displayed_value.
    float                               trail_value{0.0f};
    // Cached animation target (set by set_value; interpolate_toward
    // reads it every tick).
    float                               target_value{0.0f};
    int32_t                             animate_elapsed_ms{0};
    mutable std::mutex                  mtx;
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

int32_t peek_tag(sao_ui_widget_handle_t h) {
    if (h == nullptr) return -1;
    return *reinterpret_cast<const int32_t*>(h);
}

ProgressState* as_progress(sao_ui_widget_handle_t h) {
    if (peek_tag(h) != kProgressTag) return nullptr;
    return reinterpret_cast<ProgressState*>(h);
}

template <typename T>
T* as_tagged(sao_ui_widget_handle_t handle, int32_t tag) {
    return peek_tag(handle) == tag ? reinterpret_cast<T*>(handle) : nullptr;
}

sao_status_t copy_string_to_caller(
    const std::string& value, char* out_utf8, size_t capacity,
    size_t* out_bytes_written) {
    if (out_bytes_written == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
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

void apply_progress_spec_no_lock(ProgressState& s,
                                  const SaoUiProgressBarSpec* spec) {
    s.spec = *spec;
    if (spec->segments != nullptr && spec->segment_count > 0) {
        s.segments.assign(spec->segments,
                          spec->segments + spec->segment_count);
        s.spec.segments = s.segments.data();
        s.spec.segment_count = s.segments.size();
    } else {
        s.segments.clear();
        s.spec.segments = nullptr;
        s.spec.segment_count = 0;
    }
    s.displayed_value = spec->value;
    s.target_value    = spec->value;
    s.trail_value     = spec->value;
    s.animate_elapsed_ms = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Progress bar ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_progress_bar_create(
    void* /*d3d_device_ptr*/,
    const SaoUiProgressBarSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* s = new ProgressState();
    apply_progress_spec_no_lock(*s, spec);
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(s);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_progress_bar_set_value(
    sao_ui_widget_handle_t handle,
    float value) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    // Kick off an animation if animate_duration_ms > 0; otherwise
    // snap immediately.
    if (s->spec.animate_duration_ms > 0) {
        s->target_value = value;
        s->animate_elapsed_ms = 0;
    } else {
        s->displayed_value = value;
        s->target_value = value;
        // Trail: for HP_TRAIL style, the trail stays ≥ displayed while
        // draining (health going down).  For FLAT/HP_RAMP the trail
        // just mirrors displayed.
        if (s->spec.style == SAO_UI_PROGRESS_HP_TRAIL) {
            s->trail_value = std::max(s->trail_value, value);
        } else {
            s->trail_value = value;
        }
    }
    s->spec.value = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_progress_bar_set_max(
    sao_ui_widget_handle_t handle,
    float max_value) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->spec.max_value = max_value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gauge_create(
    void*, const SaoUiGaugeSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) GaugeState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    if (state->spec.max_value < 0.0f) state->spec.max_value = 0.0f;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gauge_set_value(
    sao_ui_widget_handle_t handle, float value) {
    GaugeState* state = as_tagged<GaugeState>(handle, kGaugeTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(value)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.value = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_gauge_get_ratio(
    sao_ui_widget_handle_t handle, float* out_ratio) {
    GaugeState* state = as_tagged<GaugeState>(handle, kGaugeTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_ratio == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_ratio = state->spec.max_value <= 0.0f
        ? 0.0f
        : clamp_ratio(state->spec.value / state->spec.max_value);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_status_badge_create(
    void*, const SaoUiStatusBadgeSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) BadgeState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->text = spec->text_utf8 == nullptr ? "" : spec->text_utf8;
    state->spec.text_utf8 = nullptr;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_status_badge_set_text(
    sao_ui_widget_handle_t handle, const char* text) {
    BadgeState* state = as_tagged<BadgeState>(handle, kBadgeTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (text == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->text = text;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_status_badge_get_text(
    sao_ui_widget_handle_t handle, char* out_utf8, size_t capacity,
    size_t* out_bytes_written) {
    BadgeState* state = as_tagged<BadgeState>(handle, kBadgeTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    return copy_string_to_caller(state->text, out_utf8, capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tooltip_attach(
    sao_ui_widget_handle_t target, const SaoUiTooltipSpec* spec,
    sao_ui_widget_handle_t* out) {
    if (target == nullptr || spec == nullptr || out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* state = new (std::nothrow) TooltipState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->target = target;
    state->spec = *spec;
    state->text = spec->text_utf8 == nullptr ? "" : spec->text_utf8;
    state->spec.text_utf8 = nullptr;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_tooltip_set_text(
    sao_ui_widget_handle_t handle, const char* text) {
    TooltipState* state = as_tagged<TooltipState>(handle, kTooltipTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (text == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->text = text;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_more_indicator_create(
    void*, const SaoUiMoreIndicatorSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) MoreState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->spec.hidden_count = std::max(0, spec->hidden_count);
    state->noun = spec->noun_utf8 == nullptr ? "条" : spec->noun_utf8;
    state->spec.noun_utf8 = nullptr;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_more_indicator_set_count(
    sao_ui_widget_handle_t handle, int32_t hidden_count) {
    MoreState* state = as_tagged<MoreState>(handle, kMoreTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (hidden_count < 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->spec.hidden_count = hidden_count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_more_indicator_get_count(
    sao_ui_widget_handle_t handle, int32_t* out_hidden_count) {
    MoreState* state = as_tagged<MoreState>(handle, kMoreTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_hidden_count == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    *out_hidden_count = state->spec.hidden_count;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_metric_create(
    void*, const SaoUiMetricSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) MetricState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->label = spec->label_utf8 == nullptr ? "" : spec->label_utf8;
    state->value = spec->value_utf8 == nullptr ? "" : spec->value_utf8;
    state->unit = spec->unit_utf8 == nullptr ? "" : spec->unit_utf8;
    state->spec.label_utf8 = nullptr;
    state->spec.value_utf8 = nullptr;
    state->spec.unit_utf8 = nullptr;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_metric_set_value(
    sao_ui_widget_handle_t handle, const char* value) {
    MetricState* state = as_tagged<MetricState>(handle, kMetricTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (value == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->value = value;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_metric_get_value(
    sao_ui_widget_handle_t handle, char* out_utf8, size_t capacity,
    size_t* out_bytes_written) {
    MetricState* state = as_tagged<MetricState>(handle, kMetricTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    return copy_string_to_caller(state->value, out_utf8, capacity, out_bytes_written);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_empty_state_create(
    void*, const SaoUiEmptyStateSpec* spec, sao_ui_widget_handle_t* out) {
    if (spec == nullptr || out == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto* state = new (std::nothrow) EmptyState();
    if (state == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    state->spec = *spec;
    state->title = spec->title_utf8 == nullptr ? "" : spec->title_utf8;
    state->detail = spec->detail_utf8 == nullptr ? "" : spec->detail_utf8;
    state->action = spec->action_utf8 == nullptr ? "" : spec->action_utf8;
    state->spec.title_utf8 = nullptr;
    state->spec.detail_utf8 = nullptr;
    state->spec.action_utf8 = nullptr;
    *out = reinterpret_cast<sao_ui_widget_handle_t>(state);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_empty_state_set_detail(
    sao_ui_widget_handle_t handle, const char* detail) {
    EmptyState* state = as_tagged<EmptyState>(handle, kEmptyStateTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (detail == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state->mtx);
    state->detail = detail;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_empty_state_get_detail(
    sao_ui_widget_handle_t handle, char* out_utf8, size_t capacity,
    size_t* out_bytes_written) {
    EmptyState* state = as_tagged<EmptyState>(handle, kEmptyStateTag);
    if (state == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(state->mtx);
    return copy_string_to_caller(state->detail, out_utf8, capacity, out_bytes_written);
}

// ---------------------------------------------------------------------------
// Wave 4 helper API — fill ratio, style-aware colour resolution, and
// the HP_TRAIL animation tick.  Not part of widget_data.h yet.
// ---------------------------------------------------------------------------

// clamp(value / max, 0..1).  Returns 0 when max == 0 to keep
// downstream painters safe from division by zero.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_fill_ratio(
    sao_ui_widget_handle_t handle,
    float* out_ratio) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const float max_v = s->spec.max_value;
    float ratio = 0.0f;
    if (max_v > 0.0f) {
        ratio = clamp_ratio(s->displayed_value / max_v);
    }
    if (out_ratio) *out_ratio = ratio;
    return SAO_STATUS_OK;
}

// Same but for the trailing decay marker (only meaningful for HP_TRAIL).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_trail_ratio(
    sao_ui_widget_handle_t handle,
    float* out_ratio) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const float max_v = s->spec.max_value;
    float ratio = 0.0f;
    if (max_v > 0.0f) {
        ratio = clamp_ratio(s->trail_value / max_v);
    }
    if (out_ratio) *out_ratio = ratio;
    return SAO_STATUS_OK;
}

// Resolve the current fill colour under the widget's style rules.
// FLAT / HP_TRAIL fall back to spec.fill_argb; HP_RAMP picks from the
// hp_bar.py ramp table (< 25% → low, < 50% → mid, else high).  For
// SEGMENTS the caller iterates over segments themselves; we return
// spec.fill_argb as a base.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_resolve_fill_argb(
    sao_ui_widget_handle_t handle,
    uint32_t* out_argb) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const float max_v = s->spec.max_value;
    float ratio = 0.0f;
    if (max_v > 0.0f) ratio = clamp_ratio(s->displayed_value / max_v);
    uint32_t argb = s->spec.fill_argb;
    if (s->spec.style == SAO_UI_PROGRESS_HP_RAMP) {
        if (ratio < 0.25f && s->spec.fill_low_argb != 0) {
            argb = s->spec.fill_low_argb;
        } else if (ratio < 0.50f && s->spec.fill_mid_argb != 0) {
            argb = s->spec.fill_mid_argb;
        } else if (s->spec.fill_high_argb != 0) {
            argb = s->spec.fill_high_argb;
        }
    }
    if (out_argb) *out_argb = argb;
    return SAO_STATUS_OK;
}

// Advance the HP_TRAIL animation.  For FLAT/HP_RAMP the trail just
// tracks displayed; for HP_TRAIL the trail decays linearly toward
// displayed over trail_lag_ms.  Also progresses any value animation
// begun by set_value when animate_duration_ms > 0.
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_tick(
    sao_ui_widget_handle_t handle,
    int32_t dt_ms) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dt_ms <= 0) return SAO_STATUS_OK;
    std::lock_guard<std::mutex> lk(s->mtx);
    // 1) value animation.
    if (s->spec.animate_duration_ms > 0 &&
        std::fabs(s->target_value - s->displayed_value) > 1e-6f) {
        s->animate_elapsed_ms += dt_ms;
        const float t = std::min(1.0f,
            static_cast<float>(s->animate_elapsed_ms) /
            static_cast<float>(s->spec.animate_duration_ms));
        s->displayed_value =
            s->displayed_value + (s->target_value - s->displayed_value) * t;
        if (t >= 1.0f) {
            s->displayed_value = s->target_value;
            s->animate_elapsed_ms = 0;
        }
    }
    // 2) HP_TRAIL trail decay.
    if (s->spec.style == SAO_UI_PROGRESS_HP_TRAIL) {
        if (s->trail_value > s->displayed_value) {
            const int32_t lag_ms = s->spec.trail_lag_ms > 0
                                    ? s->spec.trail_lag_ms : 280;
            const float span = s->trail_value - s->displayed_value;
            const float step =
                span * (static_cast<float>(dt_ms) /
                        static_cast<float>(lag_ms));
            s->trail_value = std::max(s->displayed_value,
                                       s->trail_value - step);
        } else if (s->trail_value < s->displayed_value) {
            // Heal: trail snaps up to displayed immediately.
            s->trail_value = s->displayed_value;
        }
    } else {
        s->trail_value = s->displayed_value;
    }
    return SAO_STATUS_OK;
}

// Query segment count and fetch a segment by index (for SEGMENTS style).
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_progress_get_segment(
    sao_ui_widget_handle_t handle,
    size_t index,
    SaoUiProgressSegment* out_segment) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (index >= s->segments.size()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (out_segment) *out_segment = s->segments[index];
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_widget_progress_get_segment_count(
    sao_ui_widget_handle_t handle) {
    ProgressState* s = as_progress(handle);
    if (s == nullptr) return 0;
    std::lock_guard<std::mutex> lk(s->mtx);
    return s->segments.size();
}

// Shared destroy helper.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    uint32_t removed = 0;
    (void)sao_ui_widget_release_event_handlers(handle, &removed);
    if (peek_tag(handle) == kProgressTag) {
        delete reinterpret_cast<ProgressState*>(handle);
    } else if (peek_tag(handle) == kGaugeTag) {
        delete reinterpret_cast<GaugeState*>(handle);
    } else if (peek_tag(handle) == kBadgeTag) {
        delete reinterpret_cast<BadgeState*>(handle);
    } else if (peek_tag(handle) == kTooltipTag) {
        delete reinterpret_cast<TooltipState*>(handle);
    } else if (peek_tag(handle) == kMoreTag) {
        delete reinterpret_cast<MoreState*>(handle);
    } else if (peek_tag(handle) == kMetricTag) {
        delete reinterpret_cast<MetricState*>(handle);
    } else if (peek_tag(handle) == kEmptyStateTag) {
        delete reinterpret_cast<EmptyState*>(handle);
    }
}
