// SAO Auto — text-family widgets (label / rich text / editable field).
//
// Game-agnostic building blocks used by plugin-registered game panels
// (star_resonance DPS/BossHP/HP/BuffMon/Alert/Graph, etc.).  Widgets are
// created against a compositor layer + a Direct2D paint context; the
// backing implementation lives in `platform/ui/src/`.
//
// Python source alignment:
//   * label / rich text  → PIL text draws in sao_gui_*.py + tk.Label in
//                          gui_modules/sao_panel_components.py
//   * clock / rel / dur  → fmt_clock / fmt_rel / fmt_dur in
//                          sao_panel_components.py + Web PanelTime dual
//   * text field         → gui_modules.sao_panel_components.sao_entry

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"  // sao_ui_widget_handle_t + paint ctx

#ifdef __cplusplus
extern "C" {
#endif

// ─── ARGB helpers ────────────────────────────────────────────────────
// 0 alpha = "use theme token"; the widget resolves against its owning
// theme via a token key set separately (sao_ui_widget_set_theme_token).
#define SAO_UI_ARGB(a, r, g, b) \
    ((uint32_t)(((a) & 0xFF) << 24) | \
     (uint32_t)(((r) & 0xFF) << 16) | \
     (uint32_t)(((g) & 0xFF) <<  8) | \
     (uint32_t)((b) & 0xFF))

// ─── text alignment / anchor ─────────────────────────────────────────
//
// NB: the enum tag differs from d2d_widgets.h's `sao_ui_text_align_e`
// on purpose — the two headers ship distinct enumerator sets
// (SAO_UI_TEXT_ALIGN_* legacy vs. SAO_UI_ALIGN_* extended with JUSTIFY),
// so both must coexist under different tags while keeping the same
// int32_t underlying type.  Callers hand the enum value in through
// the `int32_t align` field on each spec struct.
enum sao_ui_text_align_ext_e : int32_t {
    SAO_UI_ALIGN_LEFT   = 0,
    SAO_UI_ALIGN_CENTER = 1,
    SAO_UI_ALIGN_RIGHT  = 2,
    SAO_UI_ALIGN_JUSTIFY = 3,
};

enum sao_ui_text_anchor_e : int32_t {
    SAO_UI_ANCHOR_NW = 0, SAO_UI_ANCHOR_N = 1, SAO_UI_ANCHOR_NE = 2,
    SAO_UI_ANCHOR_W  = 3, SAO_UI_ANCHOR_C = 4, SAO_UI_ANCHOR_E  = 5,
    SAO_UI_ANCHOR_SW = 6, SAO_UI_ANCHOR_S = 7, SAO_UI_ANCHOR_SE = 8,
};

enum sao_ui_font_weight_e : int32_t {
    SAO_UI_WEIGHT_NORMAL = 400,
    SAO_UI_WEIGHT_BOLD   = 700,
};

enum sao_ui_font_slot_e : int32_t {
    // 1:1 with utils.sao_sound.get_sao_font / get_cjk_font.
    SAO_UI_FONT_SAO = 0,  // SAOUI.ttf ASCII
    SAO_UI_FONT_CJK = 1,  // ZhuZiAYuanJWD / msyh fallback
    SAO_UI_FONT_MONO = 2,
};

// ─── Label — single-line or multi-line static text ───────────────────
struct SaoUiLabelSpec {
    const char* text_utf8;
    uint32_t    fg_argb;                    // 0 → theme token 'value_fg'
    uint32_t    bg_argb;                    // 0 → transparent
    int32_t     font_slot;                  // sao_ui_font_slot_e
    int32_t     font_size_px;
    int32_t     font_weight;                // sao_ui_font_weight_e
    int32_t     align;                      // sao_ui_text_align_e
    int32_t     anchor;                     // sao_ui_text_anchor_e
    int32_t     max_lines;                  // 0 → unlimited; 1 → truncate w/ '…'
    bool        wrap;                       // word-wrap on width overflow
    float       letter_spacing_px;          // tracked text (SAO subtitle look)
    uint8_t     _pad[4];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_label_create(
    void* d3d_device_ptr,
    const SaoUiLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_label_update(
    sao_ui_widget_handle_t handle,
    const SaoUiLabelSpec* spec);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_label_set_text(
    sao_ui_widget_handle_t handle,
    const char* text_utf8);

// Measure the label's intrinsic size (post-wrap, post-letter-spacing).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_label_measure(
    sao_ui_widget_handle_t handle,
    int32_t max_width,
    int32_t* out_width,
    int32_t* out_height);

// ─── Rich text — multi-run styled text (mixed color / weight / slot) ─
struct SaoUiTextRun {
    const char* text_utf8;
    uint32_t    fg_argb;
    int32_t     font_slot;
    int32_t     font_size_px;
    int32_t     font_weight;
    bool        underline;
    bool        strikethrough;
    uint8_t     _pad[2];
};

struct SaoUiRichTextSpec {
    const SaoUiTextRun* runs;
    size_t              run_count;
    uint32_t            bg_argb;
    int32_t             align;
    int32_t             anchor;
    int32_t             line_height_px;    // 0 → auto from largest run
    int32_t             max_lines;
    bool                wrap;
    uint8_t             _pad[7];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_rich_text_create(
    void* d3d_device_ptr,
    const SaoUiRichTextSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_rich_text_update(
    sao_ui_widget_handle_t handle,
    const SaoUiRichTextSpec* spec);

// ─── Text field — editable single-line input ─────────────────────────
struct SaoUiTextFieldSpec {
    const char* placeholder_utf8;
    const char* initial_text_utf8;
    uint32_t    fg_argb;
    uint32_t    bg_argb;
    uint32_t    border_argb;
    uint32_t    focus_border_argb;         // cyan focus ring
    int32_t     font_slot;
    int32_t     font_size_px;
    int32_t     max_length;
    bool        password_mode;             // render '•' instead of chars
    bool        readonly;
    uint8_t     _pad[6];
};

typedef void (SAO_UI_CALL* sao_ui_text_change_cb_t)(
    const char* new_text_utf8, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_text_field_create(
    void* d3d_device_ptr,
    const SaoUiTextFieldSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_text_field_set_text(
    sao_ui_widget_handle_t handle,
    const char* text_utf8);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_text_field_get_text(
    sao_ui_widget_handle_t handle,
    char* out_utf8_buffer,
    size_t buffer_capacity,
    size_t* out_bytes_written);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_text_field_set_change_handler(
    sao_ui_widget_handle_t handle,
    sao_ui_text_change_cb_t callback,
    void* user_data);

// ─── Time-display labels (3 separate widgets, do NOT mix inputs!) ────
//
// Alignment with memory [ACT时间显示三类分开]:
//   * clock  ← absolute epoch milliseconds  → fmt_clock  'HH:MM:SS'
//   * rel    ← signed offset milliseconds   → fmt_rel/signed '+2.6s' / '-1:23'
//   * dur    ← non-negative duration ms     → fmt_dur    '2.6s' / 'M:SS'
//
// Handing the wrong quantity to the wrong widget is the specific bug the
// memory note warns about — the widget's declared input type is the API
// contract that keeps callers honest.

struct SaoUiClockLabelSpec {
    int64_t     epoch_ms;                  // ABSOLUTE epoch milliseconds
    bool        with_seconds;              // 'HH:MM:SS' vs 'HH:MM'
    uint8_t     _pad[7];
    SaoUiLabelSpec label;
};

struct SaoUiRelativeTimeLabelSpec {
    int64_t     epoch_ms;                  // absolute time to format
    int64_t     base_epoch_ms;             // 0 → fall back to fmt_clock
    bool        force_signed;              // 'T0' when delta==0 if false; else '+0.0s'
    uint8_t     _pad[7];
    SaoUiLabelSpec label;
};

struct SaoUiDurationLabelSpec {
    int64_t     duration_ms;               // NON-NEGATIVE duration; negatives clamp to 0
    uint8_t     _pad[8];
    SaoUiLabelSpec label;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_clock_label_create(
    void* d3d_device_ptr,
    const SaoUiClockLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_clock_label_update(
    sao_ui_widget_handle_t handle,
    int64_t epoch_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_relative_time_label_create(
    void* d3d_device_ptr,
    const SaoUiRelativeTimeLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_relative_time_label_update(
    sao_ui_widget_handle_t handle,
    int64_t epoch_ms,
    int64_t base_epoch_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_duration_label_create(
    void* d3d_device_ptr,
    const SaoUiDurationLabelSpec* spec,
    sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_duration_label_update(
    sao_ui_widget_handle_t handle,
    int64_t duration_ms);

// ─── Shared destroy / visibility / theme (usable across text family) ─
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_set_visible(
    sao_ui_widget_handle_t handle, bool visible);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_set_opacity(
    sao_ui_widget_handle_t handle, float opacity_0_to_1);

#ifdef __cplusplus
}  // extern "C"
#endif
