// SAO Auto — interactive widgets (button / dropdown / checkbox / slider).
//
// Python source alignment (memory [面板组件库支持颜色覆盖]):
//   * action_button      → sao_panel_components.action_button (fill /
//                          border / fg / canvas_bg / active / set_active)
//   * icon_button        → status buttons in sao_gui_dps / hp
//   * dropdown_button    → sao_panel_components.dropdown_button (memory
//                          [ACT全面板UX批次38bafcf])
//   * checkbox / radio   → tk.Checkbutton / tk.Radiobutton wrappers
//   * slider             → sao_panel_components sliders, DPS opacity
//
// Design contract:
//   * every widget reports events through a single callback pattern —
//     the router in `input_router.h` normalises Win32 messages into
//     SaoUiInputEvent and dispatches; individual widgets only need to
//     expose "invoke", "toggle", "value-changed" callbacks.
//   * colour overrides (fill / border / fg / canvas_bg / active_*) match
//     the Python component library exactly so a plugin theme built for
//     Tk transposes 1:1 to Direct2D.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

// A callback changed or retired the object while an outer transition was in
// progress.  The outer operation stops dispatch immediately and returns BUSY;
// callers may retry against the still-live handle.
#ifndef SAO_UI_STATUS_ERR_BUSY
#define SAO_UI_STATUS_ERR_BUSY ((sao_status_t) - 102)
#endif

// ─── Button kind — maps to `_accent()` in sao_panel_components ───────
enum sao_ui_button_kind_e : int32_t {
    SAO_UI_BTN_NORMAL = 0,
    SAO_UI_BTN_GOLD = 1,   // gold accent (default action)
    SAO_UI_BTN_CYAN = 2,   // info / accent
    SAO_UI_BTN_OK = 3,     // heal / good
    SAO_UI_BTN_DANGER = 4, // bad / error
};

// Optional colour overrides.  argb==0 → fall back to theme token via
// `kind`, matching the Python _resolve_color(fill, default) idiom.
struct SaoUiButtonColors {
    uint32_t fill_argb;
    uint32_t fill_hover_argb;
    uint32_t border_argb;
    uint32_t fg_argb;
    uint32_t canvas_bg_argb;
    // Toggled-on appearance (tabs, segmented pickers).  See
    // sao_panel_components._RoundedButton active_* fields.
    uint32_t active_fill_argb;
    uint32_t active_fg_argb;
    uint32_t active_border_argb;
    // Disabled state.
    uint32_t disabled_fill_argb;
    uint32_t disabled_fg_argb;
    uint32_t disabled_border_argb;
};

struct SaoUiButtonSpec {
    const char* text_utf8;
    int32_t kind;      // sao_ui_button_kind_e
    int32_t radius_px; // rounded corner
    int32_t pad_x_px;
    int32_t pad_y_px;
    bool active; // pre-styled 'selected' look
    bool disabled;
    uint8_t _pad[6];
    SaoUiButtonColors colors; // override; zeros = theme
};

typedef void(SAO_UI_CALL* sao_ui_click_cb_t)(void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_button_create(void* d3d_device_ptr,
                                                         const SaoUiButtonSpec* spec,
                                                         sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_button_update(sao_ui_widget_handle_t handle,
                                                         const SaoUiButtonSpec* spec);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_button_set_text(sao_ui_widget_handle_t handle,
                                                           const char* text_utf8);

// Toggle between the pre-styled 'active' and normal appearance.  Mirrors
// _RoundedButton.set_active().
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_button_set_active(sao_ui_widget_handle_t handle,
                                                             bool active);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_button_set_disabled(sao_ui_widget_handle_t handle,
                                                               bool disabled);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_button_set_click_handler(sao_ui_widget_handle_t handle,
                                                                    sao_ui_click_cb_t callback,
                                                                    void* user_data);

// ─── Icon button — button whose face is an image / glyph, not text ───
struct SaoUiIconButtonSpec {
    const void* icon_bgra_pixels; // 4bpp, top-down
    uint32_t icon_width;
    uint32_t icon_height;
    uint32_t icon_stride;
    const char* tooltip_utf8; // for attach_tooltip parity
    int32_t radius_px;
    int32_t pad_px;
    int32_t kind;
    SaoUiButtonColors colors;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_icon_button_create(void* d3d_device_ptr,
                                                              const SaoUiIconButtonSpec* spec,
                                                              sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_icon_button_set_click_handler(
    sao_ui_widget_handle_t handle, sao_ui_click_cb_t callback, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_icon_button_invoke(sao_ui_widget_handle_t handle);

// ─── Dropdown button — button that pops a menu of items on click ─────
//
// Python source: `sao_panel_components.dropdown_button`.  Items are
// (label, opaque id) pairs; the widget dispatches `selected_id` when a
// user picks an entry.  Separator entries carry the '-' marker.

#define SAO_UI_DROPDOWN_SEPARATOR (-1)

struct SaoUiDropdownEntry {
    const char* label_utf8;
    int32_t item_id; // caller-assigned; -1 = separator
    bool enabled;
    bool checked;
    uint8_t _pad[2];
};

struct SaoUiDropdownButtonSpec {
    const char* text_utf8;             // shown with '▾' suffix
    int32_t kind;                      // button kind
    const SaoUiDropdownEntry* entries; // may be NULL initially
    size_t entry_count;
    SaoUiButtonColors button_colors;
    uint32_t menu_bg_argb;
    uint32_t menu_fg_argb;
    uint32_t menu_active_bg_argb;
    uint32_t menu_active_fg_argb;
};

typedef void(SAO_UI_CALL* sao_ui_dropdown_pick_cb_t)(int32_t item_id, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dropdown_button_create(
    void* d3d_device_ptr, const SaoUiDropdownButtonSpec* spec, sao_ui_widget_handle_t* out_handle);

// Rebuild entries in place.  Matches the Python `btn._sao_dropdown_menu`
// pattern: caller can refresh entries when data changes without
// destroying + recreating the button.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dropdown_button_set_entries(
    sao_ui_widget_handle_t handle, const SaoUiDropdownEntry* entries, size_t entry_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dropdown_button_set_pick_handler(
    sao_ui_widget_handle_t handle, sao_ui_dropdown_pick_cb_t callback, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dropdown_button_select(sao_ui_widget_handle_t handle,
                                                                  int32_t item_id);

// ─── Dropdown popup state (append-only API) ─────────────────────────
// The open list is painted below the control by the widget's own paint
// pass and consumes hit tests through the panel input path.  Opening one
// dropdown closes any other open dropdown.

// Toggle the popup list.  Returns SAO_STATUS_OK when the state changed.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_toggle_popup(sao_ui_widget_handle_t handle);

// Close the popup and clear hover/entry state.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_close_popup(sao_ui_widget_handle_t handle);

// Query the open state.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_popup_is_open(sao_ui_widget_handle_t handle, bool* out_open);

// Cached popup geometry from the last paint (viewport coordinates of the
// owning panel content area).
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_popup_geometry(sao_ui_widget_handle_t handle, int32_t* out_x,
                                      int32_t* out_y, int32_t* out_w, int32_t* out_h);

// Hit test a point against the open popup.  Updates the hovered entry;
// *out_consumed is true when the point lies inside the popup (the caller
// must route the event to this widget).  *out_entry receives the hovered
// entry index, or -1 when outside.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_popup_hit(sao_ui_widget_handle_t handle, int32_t x, int32_t y,
                                 int32_t* out_entry, bool* out_consumed);

// Apply a click that was routed to this widget while its popup was open:
// selects the hovered entry when present; otherwise closes the popup.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_popup_click(sao_ui_widget_handle_t handle);

// Keyboard handling for the open popup: Up/Down/Home/End move the hover,
// Enter/Space select, Escape closes.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_dropdown_button_popup_key(sao_ui_widget_handle_t handle, uint32_t virtual_key);

// Global single-open popup probe: returns the open dropdown handle (when
// one exists) and, when the point hits its popup rect, the entry index
// with *out_consumed = true.  Coordinates are viewport-local of the
// owning panel content area.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_dropdown_popup_hit_global(int32_t x, int32_t y,
                                        sao_ui_widget_handle_t* out_owner,
                                        int32_t* out_entry, bool* out_consumed);

// Returns true when any dropdown popup is currently open.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_dropdown_any_open(bool* out_open);

// Close whatever dropdown popup is globally open.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_dropdown_close_popup_global(void);

// ─── Checkbox / radio ────────────────────────────────────────────────
struct SaoUiCheckboxSpec {
    const char* label_utf8;
    bool checked;
    bool disabled;
    uint8_t _pad[6];
    uint32_t fg_argb;
    uint32_t check_argb;
    uint32_t box_argb;
    uint32_t box_border_argb;
    int32_t font_size_px;
    int32_t box_size_px;
};

typedef void(SAO_UI_CALL* sao_ui_toggle_cb_t)(bool checked, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_checkbox_create(void* d3d_device_ptr,
                                                           const SaoUiCheckboxSpec* spec,
                                                           sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_checkbox_set_checked(sao_ui_widget_handle_t handle,
                                                                bool checked);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_checkbox_set_toggle_handler(
    sao_ui_widget_handle_t handle, sao_ui_toggle_cb_t callback, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_checkbox_toggle(sao_ui_widget_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_checkbox_get_checked(sao_ui_widget_handle_t handle,
                                                                bool* out_checked);

// Radio group — one widget per radio; all members share `group_id`,
// selection is mutually exclusive within the group.
struct SaoUiRadioSpec {
    const char* label_utf8;
    int32_t group_id; // caller-defined
    int32_t value_id; // this radio's value in the group
    bool selected;
    bool disabled;
    uint8_t _pad[6];
    uint32_t fg_argb;
    uint32_t dot_argb;
    uint32_t ring_argb;
    int32_t font_size_px;
    int32_t ring_size_px;
    uint8_t _pad2[4];
};

typedef void(SAO_UI_CALL* sao_ui_radio_pick_cb_t)(int32_t group_id, int32_t value_id,
                                                  void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_radio_create(void* d3d_device_ptr,
                                                        const SaoUiRadioSpec* spec,
                                                        sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_radio_set_group_pick_handler(
    sao_ui_widget_handle_t handle, sao_ui_radio_pick_cb_t callback, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_radio_set_selected(sao_ui_widget_handle_t handle,
                                                              bool selected);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_radio_get_selected(sao_ui_widget_handle_t handle,
                                                              bool* out_selected);

// ─── Slider — continuous or discrete value control ───────────────────
struct SaoUiSliderSpec {
    float value; // clamped to [min, max]
    float min_value;
    float max_value;
    float step; // 0 → continuous
    bool vertical;
    bool disabled;
    bool show_value_label; // draw current numeric value beside thumb
    uint8_t _pad[5];
    uint32_t track_argb;
    uint32_t track_fill_argb;
    uint32_t thumb_argb;
    uint32_t thumb_border_argb;
    int32_t track_thickness_px;
    int32_t thumb_size_px;
};

typedef void(SAO_UI_CALL* sao_ui_slider_change_cb_t)(float new_value, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_slider_create(void* d3d_device_ptr,
                                                         const SaoUiSliderSpec* spec,
                                                         sao_ui_widget_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_slider_set_value(sao_ui_widget_handle_t handle,
                                                            float value);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_slider_set_change_handler(
    sao_ui_widget_handle_t handle, sao_ui_slider_change_cb_t callback, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_slider_get_value(sao_ui_widget_handle_t handle,
                                                            float* out_value);

// Shared by input_router focus traversal. Non-input widgets report false;
// disabled input controls are skipped without invoking callbacks.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_input_is_focusable(sao_ui_widget_handle_t handle,
                                                                     bool* out_focusable);

// Input-family handles are stable process-local opaque tokens.  Generation is
// assigned at creation and never reused.  Retired handles return
// SAO_STATUS_ERR_HANDLE_INVALID and are safe to query after destroy.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_input_get_generation(sao_ui_widget_handle_t handle, uint64_t* out_generation);

#ifdef __cplusplus
} // extern "C"
#endif
