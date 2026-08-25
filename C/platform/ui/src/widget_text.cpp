// SAO Auto — text-family widgets and time-display labels.
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
// The three time-display types use STRUCTURALLY-DIFFERENT helper
// formatters exported at the bottom (int64_t epoch_ms / int32_t
// delta_seconds / uint64_t duration_ms) so callers get compile-time
// prevention of the classic "wrong quantity to wrong widget" bug
// documented in memory [ACT时间显示三类分开].  See
// tests/test_widget_time_labels.cpp for the parity matrix.
//
// UTF-8 no BOM.

#include "sao/ui/widget_text.h"
#include "sao/ui/widget_kit.h"

#include "panel_theme_internal.h"

#include "widget_typed_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
// Internal widget kinds (extends the d2d_widgets kind enum).
// ---------------------------------------------------------------------------

namespace {

// Fixed-width glyph approximation matches the memory-note "8px char"
// first-slice contract — measurements use the 8-px monospace grid
// used by fmt_dur alignment inside star_resonance panels, plus a
// per-font-slot multiplier for CJK / condensed SAO subtitle glyphs.
constexpr int32_t kAsciiGlyphAdvancePx = 8;
constexpr int32_t kCjkGlyphAdvancePx   = 16;

// Widget-kind tag stored at head of every text-widget struct so
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

struct TextPropsSnapshot {
    std::string text;
    int64_t first{};
    int64_t second{};
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
size_t utf8_glyph_count(std::string_view text) {
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
    const float spacing = std::max(0.0F, letter_spacing_px);
    const float effective_advance = static_cast<float>(advance_px) + spacing;
    if (effective_advance <= 0.0F) { out.push_back(line); return out; }
    const size_t max_glyphs_per_line = static_cast<size_t>(std::max(
        1.0F, std::floor((static_cast<float>(available_w) + spacing) /
                         effective_advance)));
    // Word-wrap: split on ASCII space; if a token is longer than the
    // line budget, hard-break it on code-point boundaries (a multi-byte
    // UTF-8 sequence always stays together in one line).
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
            // Hard-break: walk decoded code points and cut only between
            // them, so continuation bytes never end up on their own line.
            size_t pos = 0;
            size_t line_glyphs = cur_glyphs;
            while (pos < t.size()) {
                const unsigned char lead = static_cast<unsigned char>(t[pos]);
                size_t cp_len = 1;
                if ((lead & 0xE0) == 0xC0) cp_len = 2;
                else if ((lead & 0xF0) == 0xE0) cp_len = 3;
                else if ((lead & 0xF8) == 0xF0) cp_len = 4;
                if (pos + cp_len > t.size()) cp_len = t.size() - pos;
                if (line_glyphs >= max_glyphs_per_line) { flush_line(); line_glyphs = 0; }
                cur.append(t, pos, cp_len);
                ++line_glyphs;
                pos += cp_len;
            }
            cur_glyphs = line_glyphs;
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

float label_letter_spacing(const SaoUiLabelSpec& spec) noexcept {
    return std::max(0.0F, spec.letter_spacing_px);
}

int32_t label_line_width(const SaoUiLabelSpec& spec,
                         std::string_view line) noexcept {
    const size_t glyphs = utf8_glyph_count(line);
    if (glyphs == 0)
        return 0;
    const int32_t advance = glyph_advance_for_slot(spec.font_slot);
    const float width = static_cast<float>(glyphs) * static_cast<float>(advance) +
                        label_letter_spacing(spec) *
                            static_cast<float>(glyphs - 1U);
    return static_cast<int32_t>(std::ceil(width));
}

std::string utf8_prefix_glyphs(std::string_view text, size_t glyph_limit) {
    size_t offset = 0;
    size_t glyphs = 0;
    while (offset < text.size() && glyphs < glyph_limit) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U)
            bytes = 2;
        else if ((lead & 0xF0U) == 0xE0U)
            bytes = 3;
        else if ((lead & 0xF8U) == 0xF0U)
            bytes = 4;
        offset += std::min(bytes, text.size() - offset);
        ++glyphs;
    }
    return std::string(text.substr(0, offset));
}

std::string ellipsize_label_line(std::string_view text, int32_t max_width,
                                 const SaoUiLabelSpec& spec) {
    constexpr std::string_view ellipsis = "\xE2\x80\xA6";
    if (max_width <= 0)
        return std::string(text) + std::string(ellipsis);
    const int32_t advance = glyph_advance_for_slot(spec.font_slot);
    const float effective_advance = static_cast<float>(advance) +
                                    label_letter_spacing(spec);
    if (effective_advance <= 0.0F)
        return {};
    const size_t capacity = static_cast<size_t>(std::max(
        0.0F, std::floor((static_cast<float>(max_width) +
                          label_letter_spacing(spec)) /
                         effective_advance)));
    if (capacity == 0)
        return {};
    std::string result = utf8_prefix_glyphs(text, capacity - 1U);
    result.append(ellipsis);
    return result;
}

// Build the final visual line set for a label given an available width:
// splits on '\n', word-wraps when spec.wrap, truncates to max_lines with
// an ellipsis, and (single-line no-wrap case) ellipsizes the whole
// result when it cannot fit the available width.  Returns the measured
// width/height for the produced lines.
std::vector<std::string> label_visual_lines_locked(LabelState& s,
                                                   int32_t available_w,
                                                   int32_t* out_w,
                                                   int32_t* out_h) {
    const int32_t advance = glyph_advance_for_slot(s.spec.font_slot);
    const int32_t line_h  = s.spec.font_size_px > 0 ? s.spec.font_size_px : 16;
    auto logical = split_lines(s.text);
    std::vector<std::string> visual;
    visual.reserve(logical.size());
    for (auto& ll : logical) {
        if (s.spec.wrap && available_w > 0) {
            auto wrapped = wrap_line(ll, available_w, advance,
                                     label_letter_spacing(s.spec));
            for (auto& w : wrapped) visual.push_back(std::move(w));
        } else {
            visual.push_back(ll);
        }
    }
    if (s.spec.max_lines > 0 &&
        static_cast<int32_t>(visual.size()) > s.spec.max_lines) {
        visual.resize(static_cast<size_t>(s.spec.max_lines));
        if (!visual.empty())
            visual.back() = ellipsize_label_line(visual.back(), available_w,
                                                  s.spec);
    }
    // Single-line no-wrap: ellipsize to the available width so paint
    // never emits unbounded overflow long chains.
    if (visual.size() == 1 && !s.spec.wrap && available_w > 0 &&
        label_line_width(s.spec, visual[0]) > available_w) {
        visual[0] = ellipsize_label_line(visual[0], available_w, s.spec);
    }
    int32_t max_w = 0;
    for (const auto& line : visual)
        max_w = std::max(max_w, label_line_width(s.spec, line));
    const int32_t total_h = static_cast<int32_t>(visual.size()) * line_h;
    const int32_t measured_width =
        std::min(max_w, available_w > 0 ? available_w : max_w);
    if (out_w) *out_w = measured_width;
    if (out_h) *out_h = total_h;
    s.cached_max_width = available_w;
    s.cached_width = measured_width;
    s.cached_height = total_h;
    s.measure_dirty = false;
    return visual;
}

// Compute measured (width, height) for a label.  `available_w<=0` →
// unconstrained (single logical line width).
void measure_label_locked(LabelState& s, int32_t available_w,
                          int32_t* out_w, int32_t* out_h) {
    (void)label_visual_lines_locked(s, available_w, out_w, out_h);
}

sao_status_t paint_label_line(sao_ui_paint_ctx_handle_t context, float x,
                              float y, std::string_view line, float font_size,
                              uint32_t foreground,
                              const SaoUiLabelSpec& spec) {
    const float spacing = label_letter_spacing(spec);
    if (spacing <= 0.0F) {
        const std::string text(line);
        return sao_ui_paint_ctx_draw_utf8(context, x, y, text.c_str(),
                                          font_size, foreground);
    }
    const float step = static_cast<float>(glyph_advance_for_slot(spec.font_slot)) +
                       spacing;
    size_t offset = 0;
    float cursor = x;
    while (offset < line.size()) {
        const unsigned char lead = static_cast<unsigned char>(line[offset]);
        size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U)
            bytes = 2;
        else if ((lead & 0xF0U) == 0xE0U)
            bytes = 3;
        else if ((lead & 0xF8U) == 0xF0U)
            bytes = 4;
        bytes = std::min(bytes, line.size() - offset);
        const std::string glyph(line.substr(offset, bytes));
        const sao_status_t status = sao_ui_paint_ctx_draw_utf8(
            context, cursor, y, glyph.c_str(), font_size, foreground);
        if (status != SAO_STATUS_OK)
            return status;
        cursor += step;
        offset += bytes;
    }
    return SAO_STATUS_OK;
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
    // Unsigned magnitude avoids the INT64_MIN negation overflow.
    const uint64_t abs_delta = delta < 0
        ? (0ull - static_cast<uint64_t>(delta))
        : static_cast<uint64_t>(delta);
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
// Text-family helper API (not part of widget_text.h yet).
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
// gate the memory note demands.  These are the strict formatters
// referenced by test_widget_time_labels.cpp; the label update
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

sao_status_t sao::ui::detail::widget_text_apply_props(
    sao_ui_widget_handle_t handle, int32_t kind, const WidgetPropsJson& props,
    WidgetPropsSnapshot* out_snapshot) noexcept {
    if (out_snapshot == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_snapshot = {};
    try {
        auto snapshot = std::make_shared<TextPropsSnapshot>();
        switch (kind) {
        case kTagLabel: {
            if (!widget_props_has_only(props, {"text"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto state = as_label(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->text = state->text;
            }
            const auto text = props.find("text");
            if (text != props.end()) {
                if (!text->is_string())
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const std::string replacement = text->get<std::string>();
                const sao_status_t status = sao_ui_label_set_text(handle, replacement.c_str());
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kTagClockLabel: {
            if (!widget_props_has_only(props, {"epoch_ms"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto state = as_clock(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->first = state->last_epoch_ms;
            }
            const auto epoch = props.find("epoch_ms");
            int64_t replacement = 0;
            if (epoch != props.end()) {
                if (!widget_props_i64(*epoch, &replacement))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const sao_status_t status = sao_ui_clock_label_update(handle, replacement);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kTagRelativeTimeLabel: {
            if (!widget_props_has_only(props, {"epoch_ms", "base_epoch_ms"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto state = as_reltime(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->first = state->last_epoch_ms;
                snapshot->second = state->last_base_epoch_ms;
            }
            int64_t epoch = snapshot->first;
            int64_t base = snapshot->second;
            const auto epoch_property = props.find("epoch_ms");
            const auto base_property = props.find("base_epoch_ms");
            if ((epoch_property != props.end() && !widget_props_i64(*epoch_property, &epoch)) ||
                (base_property != props.end() && !widget_props_i64(*base_property, &base))) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if (epoch_property != props.end() || base_property != props.end()) {
                const sao_status_t status = sao_ui_relative_time_label_update(handle, epoch, base);
                if (status != SAO_STATUS_OK)
                    return status;
            }
            break;
        }
        case kTagDurationLabel: {
            if (!widget_props_has_only(props, {"duration_ms"}))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            auto state = as_duration(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                snapshot->first = state->last_duration_ms;
            }
            const auto duration = props.find("duration_ms");
            int64_t replacement = 0;
            if (duration != props.end()) {
                if (!widget_props_i64(*duration, &replacement))
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                const sao_status_t status = sao_ui_duration_label_update(handle, replacement);
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

sao_status_t sao::ui::detail::widget_text_restore_props(
    sao_ui_widget_handle_t handle, int32_t kind,
    const WidgetPropsSnapshot& snapshot) noexcept {
    const auto previous = std::static_pointer_cast<TextPropsSnapshot>(snapshot);
    if (previous == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    switch (kind) {
    case kTagLabel:
        return sao_ui_label_set_text(handle, previous->text.c_str());
    case kTagClockLabel:
        return sao_ui_clock_label_update(handle, previous->first);
    case kTagRelativeTimeLabel:
        return sao_ui_relative_time_label_update(handle, previous->first, previous->second);
    case kTagDurationLabel:
        return sao_ui_duration_label_update(handle, previous->first);
    default:
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }
}

sao_status_t sao::ui::detail::widget_text_paint(
    sao_ui_widget_handle_t handle, int32_t kind,
    sao_ui_paint_ctx_handle_t context, int32_t x, int32_t y,
    int32_t width, int32_t height) noexcept {
    try {
        std::string text;
        SaoUiLabelSpec spec{};
        switch (kind) {
        case kTagLabel: {
            auto state = as_label(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(state->mtx);
            text = state->text;
            spec = state->spec;
            break;
        }
        case kTagClockLabel: {
            auto state = as_clock(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(state->mtx);
            std::lock_guard<std::mutex> label_lock(state->label.mtx);
            text = state->label.text;
            spec = state->label.spec;
            break;
        }
        case kTagRelativeTimeLabel: {
            auto state = as_reltime(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(state->mtx);
            std::lock_guard<std::mutex> label_lock(state->label.mtx);
            text = state->label.text;
            spec = state->label.spec;
            break;
        }
        case kTagDurationLabel: {
            auto state = as_duration(handle);
            if (state == nullptr)
                return SAO_STATUS_ERR_HANDLE_INVALID;
            std::lock_guard<std::mutex> lock(state->mtx);
            std::lock_guard<std::mutex> label_lock(state->label.mtx);
            text = state->label.text;
            spec = state->label.spec;
            break;
        }
        default:
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }
        const bool high_contrast = sao::ui::detail::panel_theme_high_contrast();
        if (spec.bg_argb != 0) {
            const uint32_t background = high_contrast
                                            ? sao::ui::detail::panel_theme_color(
                                                  SAO_UI_TOKEN_APP_BG)
                                            : spec.bg_argb;
            const sao_status_t fill_status = sao_ui_paint_ctx_fill_rect(
                context, static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(width), static_cast<float>(height), background);
            if (fill_status != SAO_STATUS_OK)
                return fill_status;
        }
        const uint32_t foreground =
            spec.fg_argb == 0 || high_contrast
                ? sao::ui::detail::panel_theme_color(SAO_UI_TOKEN_APP_TEXT)
                : spec.fg_argb;
        const float font_size = static_cast<float>(
            spec.font_size_px > 0 ? spec.font_size_px : std::clamp(height - 4, 5, 16));
        // Resolve the center point inside the widget rect (the center is
        // the fixed reference for both align and anchor semantics).
        const float cx = static_cast<float>(x) + static_cast<float>(width) * 0.5f;
        const float cy = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
        // Horizontal origin from align + anchor column.
        //   LEFT   → west edge (+pad)
        //   CENTER → center
        //   RIGHT  → east edge (-pad)
        // anchor column shifts the block within the rect when align is unset.
        const int32_t anchor = spec.anchor;
        const bool anchor_column_west  = (anchor == SAO_UI_ANCHOR_NW ||
                                          anchor == SAO_UI_ANCHOR_W ||
                                          anchor == SAO_UI_ANCHOR_SW);
        const bool anchor_column_east  = (anchor == SAO_UI_ANCHOR_NE ||
                                          anchor == SAO_UI_ANCHOR_E ||
                                          anchor == SAO_UI_ANCHOR_SE);
        const bool anchor_row_north    = (anchor == SAO_UI_ANCHOR_NW ||
                                          anchor == SAO_UI_ANCHOR_NE ||
                                          anchor == SAO_UI_ANCHOR_N);
        const bool anchor_row_south    = (anchor == SAO_UI_ANCHOR_SW ||
                                          anchor == SAO_UI_ANCHOR_SE ||
                                          anchor == SAO_UI_ANCHOR_S);
        const int32_t pad = 2;
        const int32_t available_w = std::max(0, width - pad * 2);
        const int32_t line_h = spec.font_size_px > 0 ? spec.font_size_px : 16;
        LabelState visual_state;
        visual_state.spec = spec;
        visual_state.spec.text_utf8 = nullptr;
        visual_state.text = std::move(text);
        int32_t block_height = 0;
        const std::vector<std::string> lines = label_visual_lines_locked(
            visual_state, available_w, nullptr, &block_height);
        float origin_y = static_cast<float>(y) + static_cast<float>(pad);
        if (anchor_row_south) {
            origin_y = static_cast<float>(y + height - pad - block_height);
        } else if (!anchor_row_north) {
            origin_y = cy - static_cast<float>(block_height) * 0.5F;
        }
        // Draw each line, skipping lines completely outside the widget
        // vertical range (the paint context clip also constrains us).
        float draw_y = origin_y;
        const float bottom_bound = static_cast<float>(y) + static_cast<float>(height);
        for (const auto& line : lines) {
            if (draw_y + font_size <= static_cast<float>(y) ||
                draw_y >= bottom_bound) {
                draw_y += static_cast<float>(line_h);
                continue;
            }
            const int32_t line_width = label_line_width(spec, line);
            float line_x = static_cast<float>(x + pad);
            if (spec.align == SAO_UI_ALIGN_CENTER) {
                line_x = cx - static_cast<float>(line_width) * 0.5F;
            } else if (spec.align == SAO_UI_ALIGN_RIGHT || anchor_column_east) {
                line_x = static_cast<float>(x + width - pad - line_width);
            } else if (!anchor_column_west && spec.align != SAO_UI_ALIGN_LEFT) {
                line_x = static_cast<float>(x + pad);
            }
            const sao_status_t draw_status = paint_label_line(
                context, line_x, draw_y, line, font_size, foreground, spec);
            if (draw_status != SAO_STATUS_OK)
                return draw_status;
            draw_y += static_cast<float>(line_h);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Shared destroy — the dispatch tag tells us which struct to delete.
// Exposed under a text-family-specific name to avoid colliding with the
// d2d_widgets.cpp stub (which returns NOT_IMPL).
// ---------------------------------------------------------------------------

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_text_family_destroy(sao_ui_widget_handle_t handle) {
    if (handle == nullptr) return;
    auto state = sao::ui::detail::retire_widget_handle(
        handle, sao::ui::detail::WidgetHandleFamily::text);
    if (state == nullptr) return;
    uint32_t removed = 0;
    (void)sao::ui::detail::release_widget_event_handlers(handle, &removed);
}
