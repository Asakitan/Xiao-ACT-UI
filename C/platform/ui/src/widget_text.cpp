// SAO Auto — text-family widgets (Wave 4 / Agent d, G3.8 first slice).
//
// This slice owns the state + data-structure half of the Label family
// and the three time-display labels (Clock / RelativeTime / Duration).
// It does NOT touch the D2D compose path — those land in a later slice
// (paint hooks call in through sao_ui_widget_paint dispatch).
//
// Header-declared entry points implemented here:
//   * sao_ui_label_create / _update / _set_text / _measure
//   * sao_ui_clock_label_create / _update
//   * sao_ui_relative_time_label_create / _update
//   * sao_ui_duration_label_create / _update
//
// The three time-display types use STRUCTURALLY-DIFFERENT wave4 helper
// formatters exported at the bottom (int64_t epoch_ms / int32_t
// delta_seconds / uint64_t duration_ms) so callers get compile-time
// prevention of the classic "wrong quantity to wrong widget" bug
// documented in memory [ACT时间显示三类分开].  See
// tests/test_widget_time_labels_wave4.cpp for the parity matrix.
//
// UTF-8 no BOM.

#include "sao/ui/widget_text.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

static_assert(SAO_UI_ALIGN_LEFT   == 0, "text align enum drifted");
static_assert(SAO_UI_ALIGN_CENTER == 1, "text align enum drifted");
static_assert(SAO_UI_ALIGN_RIGHT  == 2, "text align enum drifted");
static_assert(SAO_UI_ANCHOR_NW    == 0, "text anchor enum drifted");
static_assert(SAO_UI_ANCHOR_SE    == 8, "text anchor enum drifted");
static_assert(SAO_UI_FONT_SAO     == 0, "font slot enum drifted");
static_assert(SAO_UI_FONT_MONO    == 2, "font slot enum drifted");

// ---------------------------------------------------------------------------
// Internal widget kinds (extends d2d_widgets kind enum via wave4 tag).
// ---------------------------------------------------------------------------

namespace {

// Fixed-width glyph approximation matches the memory-note "8px char"
// first-slice contract — Wave 4a measures via the 8-px monospace grid
// used by fmt_dur alignment inside star_resonance panels, plus a
// per-font-slot multiplier for CJK / condensed SAO subtitle glyphs.
constexpr int32_t kAsciiGlyphAdvancePx = 8;
constexpr int32_t kCjkGlyphAdvancePx   = 16;

// Wave4 widget-kind tag stored at head of every text-widget struct so
// the shared dispatch (sao_ui_widget_get_kind / _paint) can identify
// the concrete type without RTTI.  Values are aligned with the
// widget_kit.h SAO_UI_WIDGET_* enum ranges but redeclared here so this
// TU doesn't have to include the umbrella.
enum TextWidgetTag : int32_t {
    kTagLabel               = 100,
    kTagClockLabel          = 103,
    kTagRelativeTimeLabel   = 104,
    kTagDurationLabel       = 105,
};

struct LabelState {
    int32_t                 tag{kTagLabel};
    SaoUiLabelSpec          spec{};
    std::string             text;                   // owns text bytes
    // Cached measurement inputs; invalidated whenever text/size/wrap/spacing change.
    int32_t                 cached_max_width{-1};
    int32_t                 cached_width{0};
    int32_t                 cached_height{0};
    bool                    measure_dirty{true};
    mutable std::mutex      mtx;
};

struct ClockLabelState {
    int32_t                 tag{kTagClockLabel};
    SaoUiClockLabelSpec     spec{};
    LabelState              label;                  // rendered text sub-state
    int64_t                 last_epoch_ms{0};
    mutable std::mutex      mtx;
};

struct RelativeTimeLabelState {
    int32_t                     tag{kTagRelativeTimeLabel};
    SaoUiRelativeTimeLabelSpec  spec{};
    LabelState                  label;
    int64_t                     last_epoch_ms{0};
    int64_t                     last_base_epoch_ms{0};
    mutable std::mutex          mtx;
};

struct DurationLabelState {
    int32_t                 tag{kTagDurationLabel};
    SaoUiDurationLabelSpec  spec{};
    LabelState              label;
    int64_t                 last_duration_ms{0};
    mutable std::mutex      mtx;
};

// ---------------------------------------------------------------------------
// Text sizing helpers (fixed-width first slice, matches header contract).
// ---------------------------------------------------------------------------

int32_t glyph_advance_for_slot(int32_t font_slot) {
    return (font_slot == SAO_UI_FONT_CJK) ? kCjkGlyphAdvancePx
                                          : kAsciiGlyphAdvancePx;
}

// UTF-8 → glyph count (naive: counts non-continuation bytes; good
// enough for the 8-px measurement grid).  '\n' still counts as one
// glyph but is stripped by the line-break pass separately.
size_t utf8_glyph_count(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++n;   // count leading bytes only
    }
    return n;
}

// Split text on '\n' into logical lines.  No re-wrapping unless
// spec.wrap == true and available_w constrains us.
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    lines.push_back(cur);
    return lines;
}

// Word-wrap a single logical line to available_w using the fixed-width
// glyph advance.  Simple word-boundary greedy fit — the first slice
// doesn't need break-iterator sophistication because the DPS/BuffMon
// panels feed short single-line labels.
std::vector<std::string> wrap_line(const std::string& line,
                                    int32_t available_w,
                                    int32_t advance_px,
                                    float letter_spacing_px) {
    std::vector<std::string> out;
    if (available_w <= 0 || advance_px <= 0) {
        out.push_back(line);
        return out;
    }
    const int32_t effective_advance = advance_px +
        static_cast<int32_t>(letter_spacing_px < 0 ? 0 : letter_spacing_px);
    if (effective_advance <= 0) { out.push_back(line); return out; }
    const size_t max_glyphs_per_line =
        static_cast<size_t>(std::max(int32_t{1}, available_w / effective_advance));
    // Word-wrap: split on ASCII space; if a token is longer than the
    // line budget, hard-break it.
    std::string cur;
    size_t cur_glyphs = 0;
    auto flush_line = [&]() { out.push_back(cur); cur.clear(); cur_glyphs = 0; };
    std::string token;
    auto emit_token = [&](const std::string& t) {
        const size_t tglyphs = utf8_glyph_count(t);
        if (cur_glyphs > 0 && cur_glyphs + tglyphs + 1 > max_glyphs_per_line) {
            flush_line();
        }
        if (tglyphs > max_glyphs_per_line) {
            // Hard-break: split by glyph count (naive by byte).
            for (char c : t) {
                if (cur_glyphs >= max_glyphs_per_line) flush_line();
                cur.push_back(c);
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++cur_glyphs;
            }
            return;
        }
        if (cur_glyphs > 0) { cur.push_back(' '); ++cur_glyphs; }
        cur += t;
        cur_glyphs += tglyphs;
    };
    for (char c : line) {
        if (c == ' ') {
            if (!token.empty()) { emit_token(token); token.clear(); }
        } else {
            token.push_back(c);
        }
    }
    if (!token.empty()) emit_token(token);
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

// Compute measured (width, height) for a label.  `available_w<=0` →
// unconstrained (single logical line width).
void measure_label_locked(LabelState& s, int32_t available_w,
                          int32_t* out_w, int32_t* out_h) {
    const int32_t advance = glyph_advance_for_slot(s.spec.font_slot);
    const int32_t line_h  = s.spec.font_size_px > 0 ? s.spec.font_size_px : 16;
    auto logical = split_lines(s.text);
    std::vector<std::string> visual;
    visual.reserve(logical.size());
    for (auto& ll : logical) {
        if (s.spec.wrap && available_w > 0) {
            auto wrapped = wrap_line(ll, available_w, advance,
                                     s.spec.letter_spacing_px);
            for (auto& w : wrapped) visual.push_back(std::move(w));
        } else {
            visual.push_back(ll);
        }
    }
    if (s.spec.max_lines > 0 &&
        static_cast<int32_t>(visual.size()) > s.spec.max_lines) {
        visual.resize(static_cast<size_t>(s.spec.max_lines));
        // truncated tail marker
        if (!visual.empty()) visual.back() += "\xE2\x80\xA6";   // '…'
    }
    int32_t max_w = 0;
    for (auto& v : visual) {
        const int32_t w = static_cast<int32_t>(utf8_glyph_count(v)) * advance +
            static_cast<int32_t>(s.spec.letter_spacing_px * (utf8_glyph_count(v) > 0
                ? static_cast<float>(utf8_glyph_count(v) - 1) : 0.0f));
        max_w = std::max(max_w, w);
    }
    const int32_t total_h = static_cast<int32_t>(visual.size()) * line_h;
    if (out_w) *out_w = max_w;
    if (out_h) *out_h = total_h;
    s.cached_max_width = available_w;
    s.cached_width = max_w;
    s.cached_height = total_h;
    s.measure_dirty = false;
}

// ---------------------------------------------------------------------------
// Format helpers (fmt_clock / fmt_dur / fmt_rel semantics from
// gui_modules/sao_panel_components.py, memory [ACT时间显示三类分开]).
// ---------------------------------------------------------------------------

// fmt_dur: non-negative ms.
// < 60s   → '2.6s'
// < 1 h   → 'M:SS'
// else    → 'H:MM:SS'
void fmt_dur_into(uint64_t duration_ms, char* buf, size_t buf_size) {
    if (buf == nullptr || buf_size == 0) return;
    const double total_s = static_cast<double>(duration_ms) / 1000.0;
    if (total_s < 60.0) {
        std::snprintf(buf, buf_size, "%.1fs", total_s);
    } else if (total_s < 3600.0) {
        const int m = static_cast<int>(total_s) / 60;
        const int s = static_cast<int>(total_s) % 60;
        std::snprintf(buf, buf_size, "%d:%02d", m, s);
    } else {
        const int h = static_cast<int>(total_s) / 3600;
        const int rem = static_cast<int>(total_s) % 3600;
        const int m = rem / 60;
        const int s = rem % 60;
        std::snprintf(buf, buf_size, "%d:%02d:%02d", h, m, s);
    }
}

// fmt_clock: absolute epoch-ms → 'HH:MM:SS' or 'HH:MM'.
void fmt_clock_into(int64_t epoch_ms, bool with_seconds,
                    char* buf, size_t buf_size) {
    if (buf == nullptr || buf_size == 0) return;
    if (epoch_ms <= 0) {
        std::snprintf(buf, buf_size, "--");
        return;
    }
    const std::time_t t = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    if (with_seconds) {
        std::snprintf(buf, buf_size, "%02d:%02d:%02d",
                      lt.tm_hour, lt.tm_min, lt.tm_sec);
    } else {
        std::snprintf(buf, buf_size, "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
}

// fmt_rel: signed delta ms → '+2.6s' / '-1:23'.  When base==0, we
// fall back to fmt_clock (matches sao_panel_components.fmt_rel).
void fmt_rel_into(int64_t epoch_ms, int64_t base_epoch_ms,
                  char* buf, size_t buf_size) {
    if (buf == nullptr || buf_size == 0) return;
    if (base_epoch_ms == 0) {
        fmt_clock_into(epoch_ms, true, buf, buf_size);
        return;
    }
    const int64_t delta = epoch_ms - base_epoch_ms;
    const uint64_t abs_delta =
        static_cast<uint64_t>(delta < 0 ? -delta : delta);
    char sub[32] = {0};
    fmt_dur_into(abs_delta, sub, sizeof(sub));
    std::snprintf(buf, buf_size, "%s%s", delta >= 0 ? "+" : "-", sub);
}

// fmt_signed: pre-computed delta (may be zero).  Zero → 'T0'.
void fmt_signed_into(int32_t delta_seconds, char* buf, size_t buf_size) {
    if (buf == nullptr || buf_size == 0) return;
    if (delta_seconds == 0) {
        std::snprintf(buf, buf_size, "T0");
        return;
    }
    const int32_t abs_s = delta_seconds < 0 ? -delta_seconds : delta_seconds;
    const uint64_t ms = static_cast<uint64_t>(abs_s) * 1000ull;
    char sub[32] = {0};
    fmt_dur_into(ms, sub, sizeof(sub));
    std::snprintf(buf, buf_size, "%s%s", delta_seconds > 0 ? "+" : "-", sub);
}

// ---------------------------------------------------------------------------
// Registry-backed handle helpers.  The opaque handle points to a retained
// generation shell, never to widget state.  Registry/family/kind validation
// therefore completes before state memory can be accessed.
// ---------------------------------------------------------------------------

template <typename State>
std::shared_ptr<State> acquire_text_state(
    sao_ui_widget_handle_t handle, int32_t kind) {
    return std::static_pointer_cast<State>(
        sao::ui::detail::acquire_widget_handle(
            handle, sao::ui::detail::WidgetHandleFamily::text, kind));
}

std::shared_ptr<LabelState> as_label(sao_ui_widget_handle_t handle) {
    return acquire_text_state<LabelState>(handle, kTagLabel);
}

std::shared_ptr<ClockLabelState> as_clock(sao_ui_widget_handle_t handle) {
    return acquire_text_state<ClockLabelState>(handle, kTagClockLabel);
}

std::shared_ptr<RelativeTimeLabelState> as_reltime(
    sao_ui_widget_handle_t handle) {
    return acquire_text_state<RelativeTimeLabelState>(
        handle, kTagRelativeTimeLabel);
}

std::shared_ptr<DurationLabelState> as_duration(
    sao_ui_widget_handle_t handle) {
    return acquire_text_state<DurationLabelState>(handle, kTagDurationLabel);
}

template <typename State>
sao_status_t publish_text_state(
    int32_t kind, std::shared_ptr<State> state,
    sao_ui_widget_handle_t* out_handle) {
    void* const handle = sao::ui::detail::register_widget_handle(
        sao::ui::detail::WidgetHandleFamily::text, kind, std::move(state));
    if (handle == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    *out_handle = reinterpret_cast<sao_ui_widget_handle_t>(handle);
    return SAO_STATUS_OK;
}

void apply_label_spec_no_lock(LabelState& s, const SaoUiLabelSpec* spec) {
    s.spec = *spec;
    s.text = spec->text_utf8 ? spec->text_utf8 : "";
    // NUL out the borrowed pointer so nothing accidentally dereferences
    // it after the caller frees the caller-owned buffer.
    s.spec.text_utf8 = nullptr;
    s.measure_dirty = true;
    s.cached_max_width = -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Label ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_label_create(
    void* /*d3d_device_ptr*/,
    const SaoUiLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    try {
        auto state = std::make_shared<LabelState>();
        apply_label_spec_no_lock(*state, spec);
        return publish_text_state(kTagLabel, std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_label_update(
    sao_ui_widget_handle_t handle,
    const SaoUiLabelSpec* spec) {
    auto s = as_label(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (spec == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    apply_label_spec_no_lock(*s, spec);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_label_set_text(
    sao_ui_widget_handle_t handle,
    const char* text_utf8) {
    auto s = as_label(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->text = text_utf8 ? text_utf8 : "";
    s->measure_dirty = true;
    s->cached_max_width = -1;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_label_measure(
    sao_ui_widget_handle_t handle,
    int32_t max_width,
    int32_t* out_width,
    int32_t* out_height) {
    auto s = as_label(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (!s->measure_dirty && s->cached_max_width == max_width) {
        if (out_width)  *out_width  = s->cached_width;
        if (out_height) *out_height = s->cached_height;
        return SAO_STATUS_OK;
    }
    measure_label_locked(*s, max_width, out_width, out_height);
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Clock label ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_clock_label_create(
    void* /*d3d_device_ptr*/,
    const SaoUiClockLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    try {
        auto state = std::make_shared<ClockLabelState>();
        state->spec = *spec;
        state->last_epoch_ms = spec->epoch_ms;
        apply_label_spec_no_lock(state->label, &spec->label);
        char buf[32] = {0};
        fmt_clock_into(state->last_epoch_ms, spec->with_seconds, buf, sizeof(buf));
        state->label.text = buf;
        return publish_text_state(kTagClockLabel, std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_clock_label_update(
    sao_ui_widget_handle_t handle,
    int64_t epoch_ms) {
    auto s = as_clock(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->last_epoch_ms = epoch_ms;
    char buf[32] = {0};
    fmt_clock_into(epoch_ms, s->spec.with_seconds, buf, sizeof(buf));
    std::lock_guard<std::mutex> lk2(s->label.mtx);
    s->label.text = buf;
    s->label.measure_dirty = true;
    s->label.cached_max_width = -1;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Relative time label ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_relative_time_label_create(
    void* /*d3d_device_ptr*/,
    const SaoUiRelativeTimeLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    try {
        auto state = std::make_shared<RelativeTimeLabelState>();
        state->spec = *spec;
        state->last_epoch_ms = spec->epoch_ms;
        state->last_base_epoch_ms = spec->base_epoch_ms;
        apply_label_spec_no_lock(state->label, &spec->label);
        char buf[32] = {0};
        fmt_rel_into(state->last_epoch_ms, state->last_base_epoch_ms, buf, sizeof(buf));
        state->label.text = buf;
        return publish_text_state(kTagRelativeTimeLabel, std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_relative_time_label_update(
    sao_ui_widget_handle_t handle,
    int64_t epoch_ms,
    int64_t base_epoch_ms) {
    auto s = as_reltime(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->last_epoch_ms = epoch_ms;
    s->last_base_epoch_ms = base_epoch_ms;
    char buf[32] = {0};
    fmt_rel_into(epoch_ms, base_epoch_ms, buf, sizeof(buf));
    std::lock_guard<std::mutex> lk2(s->label.mtx);
    s->label.text = buf;
    s->label.measure_dirty = true;
    s->label.cached_max_width = -1;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Duration label ABI.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_duration_label_create(
    void* /*d3d_device_ptr*/,
    const SaoUiDurationLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle) {
    if (spec == nullptr || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    try {
        auto state = std::make_shared<DurationLabelState>();
        state->spec = *spec;
        const int64_t clamped = spec->duration_ms < 0 ? 0 : spec->duration_ms;
        state->last_duration_ms = clamped;
        apply_label_spec_no_lock(state->label, &spec->label);
        char buf[32] = {0};
        fmt_dur_into(static_cast<uint64_t>(clamped), buf, sizeof(buf));
        state->label.text = buf;
        return publish_text_state(kTagDurationLabel, std::move(state), out_handle);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_duration_label_update(
    sao_ui_widget_handle_t handle,
    int64_t duration_ms) {
    auto s = as_duration(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lk(s->mtx);
    const int64_t clamped = duration_ms < 0 ? 0 : duration_ms;
    s->last_duration_ms = clamped;
    char buf[32] = {0};
    fmt_dur_into(static_cast<uint64_t>(clamped), buf, sizeof(buf));
    std::lock_guard<std::mutex> lk2(s->label.mtx);
    s->label.text = buf;
    s->label.measure_dirty = true;
    s->label.cached_max_width = -1;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Wave 4 helper API (not part of widget_text.h yet).
//
// The three formatters take STRUCTURALLY-DIFFERENT parameter types so
// the compiler catches "wrong quantity to wrong widget" mismatches:
//   * clock  ← int64_t   epoch_ms      (may be very large; must be 64-bit)
//   * rel    ← int32_t   delta_seconds (SIGNED; -2^31..+2^31)
//   * dur    ← uint64_t  duration_ms   (NON-NEGATIVE; won't cast from -1)
//
// The narrower parameter types (int32/uint64) don't cross-cast silently
// from each other or from int64_t without a diagnostic on -Wnarrowing
// / -Wsign-conversion / -Wconversion, giving callers the compile-time
// gate the memory note demands.  These are the WAVE4 STRICT formatters
// referenced by test_widget_time_labels_wave4.cpp; the label update
// paths call directly into fmt_*_into which take the same types.
// ---------------------------------------------------------------------------

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_clock_label_format(
    sao_ui_widget_handle_t handle,
    char* buf,
    size_t buf_size) {
    auto s = as_clock(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (buf == nullptr || buf_size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    fmt_clock_into(s->last_epoch_ms, s->spec.with_seconds, buf, buf_size);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_relative_time_label_format(
    sao_ui_widget_handle_t handle,
    char* buf,
    size_t buf_size) {
    auto s = as_reltime(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (buf == nullptr || buf_size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    fmt_rel_into(s->last_epoch_ms, s->last_base_epoch_ms, buf, buf_size);
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_duration_label_format(
    sao_ui_widget_handle_t handle,
    char* buf,
    size_t buf_size) {
    auto s = as_duration(handle);
    if (s == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (buf == nullptr || buf_size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(s->mtx);
    fmt_dur_into(static_cast<uint64_t>(s->last_duration_ms), buf, buf_size);
    return SAO_STATUS_OK;
}

// Strict-typed helpers exposed for tests / callers who want to format
// a bare value without a widget handle.  Distinct parameter types are
// the compile-time contract enforcement documented in the header
// comment above — do NOT collapse these into a single overloaded
// function.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_format_clock_strict(
    int64_t epoch_ms,               // ABSOLUTE epoch — 64-bit only
    bool with_seconds,
    char* buf,
    size_t buf_size) {
    fmt_clock_into(epoch_ms, with_seconds, buf, buf_size);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_format_relative_strict(
    int32_t delta_seconds,          // SIGNED delta — 32-bit
    char* buf,
    size_t buf_size) {
    fmt_signed_into(delta_seconds, buf, buf_size);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_format_duration_strict(
    uint64_t duration_ms,           // NON-NEGATIVE — unsigned 64-bit
    char* buf,
    size_t buf_size) {
    fmt_dur_into(duration_ms, buf, buf_size);
}

// ---------------------------------------------------------------------------
// Shared destroy — the dispatch tag tells us which struct to delete.
// Exposed under the standard sao_ui_widget_destroy_wave4 name to avoid
// colliding with the d2d_widgets.cpp stub (which returns NOT_IMPL).
// ---------------------------------------------------------------------------

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_text_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    auto state = sao::ui::detail::retire_widget_handle(
        handle, sao::ui::detail::WidgetHandleFamily::text);
    if (state == nullptr) return;
    uint32_t removed = 0;
    (void)sao_ui_widget_release_event_handlers(handle, &removed);
}
