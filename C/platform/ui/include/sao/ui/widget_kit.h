// SAO Auto — widget kit umbrella header.
//
// The widget kit is game-agnostic.  Plugins compose these widgets into
// their game-specific panels (star_resonance DPS/BossHP/HP, etc.) via
// `sao_ui_panel_register()` in `panel.h`.  Widgets are typed handles;
// creation takes a spec struct so the API is stable across languages
// (Python via SDK, Lua via `sao_ui_scriptable_canvas.h`, etc.).
//
// The header is split into topic modules to stay under the 350-line
// budget while giving each family room to grow:
//   * widget_text.h       — label / rich text / text field + clock/rel/dur
//   * widget_input.h      — button / icon button / dropdown / checkbox / radio / slider
//   * widget_container.h  — panel / scroll / tab / grid
//   * widget_data.h       — progress / gauge / badge / tooltip / more
//   * widget_table.h      — table (columns + row upsert / sort / filter) + tree
//   * widget_chart.h      — time series / bar / line
//
// The umbrella owns:
//   * common lifecycle helpers (destroy / paint / hit test)
//   * the widget kind registry and version metadata used for
//     forward-compat plugin binaries.
//
// See `panel_layout.h` for how widgets are placed inside a panel, and
// `input_router.h` for how events are dispatched to them.  The Python
// reference implementation lives in `gui_modules/sao_panel_components.py`,
// `sao_theme/*`, and the SAO panels under
// `plugins/star_resonance_plugin/panels/`.

#pragma once

#include "sao/ui/abi.h"
#include "sao/ui/widget_text.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_container.h"
#include "sao/ui/widget_data.h"
#include "sao/ui/widget_table.h"
#include "sao/ui/widget_chart.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Widget kit version — bumped whenever a spec struct grows ────────
#define SAO_UI_WIDGET_KIT_VERSION_MAJOR 1u
#define SAO_UI_WIDGET_KIT_VERSION_MINOR 2u
#define SAO_UI_WIDGET_KIT_VERSION \
    ((SAO_UI_WIDGET_KIT_VERSION_MAJOR << 16) | \
     SAO_UI_WIDGET_KIT_VERSION_MINOR)

SAO_UI_API uint32_t SAO_UI_CALL sao_ui_widget_kit_version(void);

// ─── Widget kind enumeration ─────────────────────────────────────────
//
// Every widget type produced by widget_text/input/container/data/chart
// is discoverable through `sao_ui_widget_get_kind(handle)`.  The values
// deliberately continue past the legacy `sao_ui_widget_kind_e` in
// `d2d_widgets.h` so callers can migrate incrementally.
enum sao_ui_widget_kind_ext_e : int32_t {
    // Reserved 0..15 for the legacy d2d_widgets kinds.
    SAO_UI_WIDGET_LABEL               = 100,
    SAO_UI_WIDGET_RICH_TEXT           = 101,
    SAO_UI_WIDGET_TEXT_FIELD          = 102,
    SAO_UI_WIDGET_CLOCK_LABEL         = 103,  // fmt_clock
    SAO_UI_WIDGET_RELATIVE_TIME_LABEL = 104,  // fmt_rel/signed
    SAO_UI_WIDGET_DURATION_LABEL      = 105,  // fmt_dur

    SAO_UI_WIDGET_BUTTON              = 120,
    SAO_UI_WIDGET_ICON_BUTTON         = 121,
    SAO_UI_WIDGET_DROPDOWN_BUTTON_EXT = 122,
    SAO_UI_WIDGET_CHECKBOX_EXT        = 123,
    SAO_UI_WIDGET_RADIO_EXT           = 124,
    SAO_UI_WIDGET_SLIDER_EXT          = 125,

    SAO_UI_WIDGET_PANEL               = 130,
    SAO_UI_WIDGET_SCROLL_VIEW         = 131,
    SAO_UI_WIDGET_TAB_VIEW            = 132,
    SAO_UI_WIDGET_GRID                = 133,

    SAO_UI_WIDGET_PROGRESS_BAR        = 140,
    SAO_UI_WIDGET_GAUGE               = 141,
    SAO_UI_WIDGET_STATUS_BADGE_EXT    = 142,
    SAO_UI_WIDGET_TOOLTIP_EXT         = 143,
    SAO_UI_WIDGET_MORE_INDICATOR_EXT  = 144,
    SAO_UI_WIDGET_TABLE_EXT           = 145,
    SAO_UI_WIDGET_TREE_VIEW           = 146,
    SAO_UI_WIDGET_METRIC              = 147,
    SAO_UI_WIDGET_EMPTY_STATE         = 148,

    SAO_UI_WIDGET_TIME_SERIES_CHART   = 150,
    SAO_UI_WIDGET_BAR_CHART           = 151,
    SAO_UI_WIDGET_LINE_CHART          = 152,
    SAO_UI_WIDGET_SPARKLINE           = 153,

    SAO_UI_WIDGET_SCRIPTABLE_CANVAS   = 160,
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_get_kind(
    sao_ui_widget_handle_t handle,
    int32_t* out_kind);

// ─── Event handler registry (uniform across all widget kinds) ────────
//
// Widgets emit typed events (click / hover / focus / value change).
// Rather than a bespoke setter per kind, the router-friendly API is a
// single generic subscription — the widget still exposes the
// kind-specific setters (button click, slider change) for callers that
// want strong typing.
enum sao_ui_widget_event_type_e : int32_t {
    SAO_UI_EVT_CLICK         = 0,
    SAO_UI_EVT_DOUBLE_CLICK  = 1,
    SAO_UI_EVT_RIGHT_CLICK   = 2,
    SAO_UI_EVT_HOVER_ENTER   = 3,
    SAO_UI_EVT_HOVER_LEAVE   = 4,
    SAO_UI_EVT_FOCUS_GAINED  = 5,
    SAO_UI_EVT_FOCUS_LOST    = 6,
    SAO_UI_EVT_VALUE_CHANGED = 7,
    SAO_UI_EVT_TEXT_CHANGED  = 8,
    SAO_UI_EVT_SELECTION_CHANGED = 9,       // table row select / tree node
    SAO_UI_EVT_DRAG_BEGIN    = 10,
    SAO_UI_EVT_DRAG_END      = 11,
    SAO_UI_EVT_SCROLL        = 12,
};

typedef void (SAO_UI_CALL* sao_ui_widget_event_cb_t)(
    int32_t event_type,                     // sao_ui_widget_event_type_e
    const uint8_t* event_payload_json_utf8, // widget-specific extras
    size_t payload_len,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_add_event_handler(
    sao_ui_widget_handle_t handle,
    int32_t event_type,
    sao_ui_widget_event_cb_t callback,
    void* user_data,
    uint64_t* out_subscription_token);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_remove_event_handler(
    sao_ui_widget_handle_t handle,
    uint64_t subscription_token);

// Dispatches handlers in registration order.  The registry lock is not held
// while callbacks run, so callbacks may remove handlers or recursively
// dispatch another event on the same widget.  Payload storage remains owned
// by the caller and only needs to stay alive for this call.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_dispatch_event(
    sao_ui_widget_handle_t handle,
    int32_t event_type,
    const uint8_t* event_payload_json_utf8,
    size_t payload_len);

// Removes every generic event handler associated with this widget owner.
// Widget destruction calls this automatically; the explicit form supports
// plugin/owner teardown before the caller releases the widget itself.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_release_event_handlers(
    sao_ui_widget_handle_t handle,
    uint32_t* out_removed_count);

// ─── Paint hook — used by compositor to render into a paint ctx ──────
//
// `sao_ui_widget_paint()` already lives in d2d_widgets.h; declared there
// so consumers can call it without dragging the umbrella in.  This
// wrapper is here purely for documentation completeness.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_paint_at(
    sao_ui_widget_handle_t handle,
    sao_ui_paint_ctx_handle_t ctx,
    int32_t x, int32_t y,
    int32_t width, int32_t height,
    float opacity_0_to_1);

// Optional renderer for an extended widget kind.  The provider borrows the
// widget and paint context for the duration of the callback and never owns
// either object.  paint_at applies clipping and opacity through the paint
// context before invoking the provider.
typedef sao_status_t (SAO_UI_CALL* sao_ui_widget_renderer_cb_t)(
    sao_ui_widget_handle_t handle,
    sao_ui_paint_ctx_handle_t ctx,
    int32_t x, int32_t y,
    int32_t width, int32_t height,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_register_renderer_provider(
    int32_t widget_kind,
    sao_ui_widget_renderer_cb_t callback,
    void* user_data,
    uint64_t* out_provider_token);

// Retires the provider and waits for every in-flight paint callback before
// returning OK, after which user_data may be released.  Calling this for the
// active provider from inside its own callback returns SAO_UI_STATUS_ERR_BUSY
// and leaves the provider registered for a later teardown call.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_unregister_renderer_provider(
    uint64_t provider_token);

// ─── Preferred-size query (used by panel_layout measure phase) ───────
struct SaoUiWidgetSizeHint {
    int32_t min_width_px;
    int32_t min_height_px;
    int32_t preferred_width_px;
    int32_t preferred_height_px;
    int32_t max_width_px;                   // 0 → unbounded
    int32_t max_height_px;
    float   flex_grow;                      // fraction of extra space taken
    float   flex_shrink;                    // reduction weight when short
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_get_size_hint(
    sao_ui_widget_handle_t handle,
    int32_t available_width_px,
    int32_t available_height_px,
    SaoUiWidgetSizeHint* out_hint);

#ifdef __cplusplus
}  // extern "C"
#endif
