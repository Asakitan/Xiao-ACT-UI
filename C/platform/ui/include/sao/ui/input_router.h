// SAO Auto — deepened input router (event dispatch + focus stack + hotkey).
//
// The legacy `input.h` handles the coarse click-through / hotkey / cursor
// concerns of the single overlay HWND.  This header deepens the widget-
// facing side: turns raw Win32 WM_* into typed `SaoUiInputEvent`, routes
// them through the panel → layout → widget stack, maintains a focus
// stack, and implements the hotkey matcher from memory
// [快捷键架构]:
//     * subset matching (F5 fires for both {F5} and {CTRL+F5} bindings)
//     * most-specific-wins tie-break
//     * modifier reality (bitmask read from GetAsyncKeyState, LL-hook
//       key-up drops are immune)
//     * plugin keys forced into CTRL+combo to avoid clobbering main UI
//       F5-F12 mapping.
//
// Python source alignment:
//   * `render/overlay_host.py::wnd_proc`         — raw event source
//   * `sao_gui_hotkey.py`                        — hotkey matcher
//   * `sao_gui/panel.py::_on_click / _on_move`   — panel-level dispatch
//   * `gui_modules/sao_panel_components.py`
//         .attach_tooltip + _bind_click / _bind_hover — widget subscribe
//
// Layered API — pick the surface you need:
//   * host WM_* → SaoUiInputEvent   (feed_raw_win32_message)
//   * SaoUiInputEvent → panel/widget (route_event)
//   * focus stack (push/pop/current)
//   * hotkey binding + subset matcher

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_layout.h"

#ifndef SAO_UI_STATUS_ERR_BUSY
#define SAO_UI_STATUS_ERR_BUSY ((sao_status_t) - 102)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_input_router_deep_s* sao_ui_input_router_deep_handle_t;
typedef uint64_t sao_ui_hotkey_binding_t;

// ─── Typed event kinds ───────────────────────────────────────────────
enum sao_ui_input_event_kind_e : int32_t {
    SAO_UI_INPUT_MOUSE_MOVE = 0,
    SAO_UI_INPUT_MOUSE_DOWN = 1,
    SAO_UI_INPUT_MOUSE_UP = 2,
    SAO_UI_INPUT_MOUSE_WHEEL = 3,
    SAO_UI_INPUT_MOUSE_ENTER = 4, // synthetic — router only
    SAO_UI_INPUT_MOUSE_LEAVE = 5, // synthetic
    SAO_UI_INPUT_KEY_DOWN = 6,
    SAO_UI_INPUT_KEY_UP = 7,
    SAO_UI_INPUT_KEY_CHAR = 8, // WM_CHAR (Unicode codepoint)
    SAO_UI_INPUT_FOCUS_GAIN = 9,
    SAO_UI_INPUT_FOCUS_LOSE = 10,
    SAO_UI_INPUT_TOUCH_BEGIN = 11,
    SAO_UI_INPUT_TOUCH_UPDATE = 12,
    SAO_UI_INPUT_TOUCH_END = 13,
};

enum sao_ui_mouse_button_e : int32_t {
    SAO_UI_MOUSE_LEFT = 0,
    SAO_UI_MOUSE_RIGHT = 1,
    SAO_UI_MOUSE_MIDDLE = 2,
    SAO_UI_MOUSE_X1 = 3,
    SAO_UI_MOUSE_X2 = 4,
};

// Modifier mask — matches the `sao_ui_hotkey_modifier_e` values in
// legacy input.h (CTRL/ALT/SHIFT/WIN) plus a "wildcard" bit used by
// subset matcher.
enum sao_ui_modifier_e : uint32_t {
    SAO_UI_MOD_NONE = 0,
    SAO_UI_MOD_CTRL_BIT = 1u << 0,
    SAO_UI_MOD_ALT_BIT = 1u << 1,
    SAO_UI_MOD_SHIFT_BIT = 1u << 2,
    SAO_UI_MOD_WIN_BIT = 1u << 3,
    // Bits reserved for capslock / numlock (mouse chords etc.).
    SAO_UI_MOD_CAPS_BIT = 1u << 4,
    SAO_UI_MOD_NUM_BIT = 1u << 5,
    // Wildcard "either state matches" — used only in bindings, never in
    // observed events.  When set on a binding bit, the binding matches
    // regardless of the observed bit's value.
    SAO_UI_MOD_ANY_CTRL = 1u << 16,
    SAO_UI_MOD_ANY_ALT = 1u << 17,
    SAO_UI_MOD_ANY_SHIFT = 1u << 18,
    SAO_UI_MOD_ANY_WIN = 1u << 19,
};

struct SaoUiInputEvent {
    int32_t kind; // sao_ui_input_event_kind_e
    // Timestamp in monotonic microseconds — matches
    // SaoUiRenderHookPayload.frame_time_us so ordering is unambiguous.
    int64_t time_us;

    // Screen (host-local) coordinates.  Mouse events: cursor pos at
    // event.  Key events: cursor pos when event was captured.
    int32_t screen_x_px;
    int32_t screen_y_px;

    // Mouse-specific.
    int32_t button;      // sao_ui_mouse_button_e
    int32_t wheel_delta; // signed multiple of 120
    int32_t click_count; // 1 = single, 2 = double, …
    bool drag_active;    // set true after threshold
    bool key_repeat;     // key-down repeat (lParam bit 30)
    uint8_t _pad[2];

    // Key-specific.
    uint32_t virtual_key;       // Win32 VK_*
    uint32_t scan_code;         // hardware scancode
    uint32_t unicode_codepoint; // for KEY_CHAR only

    // Modifier bitmask observed at event dispatch — read from
    // GetAsyncKeyState directly to survive the LL-hook key-up drop
    // discussed in memory [快捷键架构].
    uint32_t modifiers;

    // Touch-specific.
    int32_t touch_id;
    // Optional payload — free-form UTF-8 JSON (widget-produced events).
    const uint8_t* payload_json_utf8;
    size_t payload_len;
};

// ─── Manager lifecycle ───────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_deep_create(
    sao_ui_compositor_handle_t compositor, sao_ui_input_router_deep_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL
sao_ui_input_router_deep_destroy(sao_ui_input_router_deep_handle_t handle);

// ─── Raw Win32 feed ──────────────────────────────────────────────────
//
// The overlay host's WndProc forwards WM_MOUSE* / WM_KEY* / WM_CHAR
// here.  The router translates them into typed SaoUiInputEvent and
// invokes `route_event` internally.  Returns non-zero when the event
// was fully handled and the caller should suppress DefWindowProc.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_feed_raw_win32(sao_ui_input_router_deep_handle_t handle,
                                   uint32_t msg, // WM_MOUSEMOVE etc.
                                   uint64_t wparam, int64_t lparam, bool* out_consumed);

// ─── Explicit dispatch — for test rigs / synthetic events ────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_route_event(
    sao_ui_input_router_deep_handle_t handle, const SaoUiInputEvent* event, bool* out_consumed);

// ─── Hit-region declarations (optional per-panel refinement) ─────────
//
// A panel's default hit region is its final arranged rect (see
// panel_layout.h).  Panels with irregular shapes (circular HP orb,
// polygon HUD) can declare a custom region.
enum sao_ui_hit_shape_e : int32_t {
    SAO_UI_HIT_RECT = 0,
    SAO_UI_HIT_CIRCLE = 1,
    SAO_UI_HIT_POLYGON = 2,
    SAO_UI_HIT_ALPHA = 3, // per-pixel alpha of layer
};

struct SaoUiHitRegion {
    int32_t shape; // sao_ui_hit_shape_e
    // Rect: (x, y, w, h).  Circle: (cx, cy, r, _).  Polygon: point count
    // in x, points ptr in verts.
    int32_t x_or_cx;
    int32_t y_or_cy;
    int32_t w_or_r;
    int32_t h_or_pt_count;
    const int32_t* poly_verts_xy_pairs; // 2 * point_count int32s
    uint8_t alpha_threshold;            // 0..255; below = not a hit
    uint8_t _pad[7];
};

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_set_panel_region(sao_ui_input_router_deep_handle_t handle,
                                     sao_ui_panel_handle_t panel, const SaoUiHitRegion* region);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_clear_panel_region(
    sao_ui_input_router_deep_handle_t handle, sao_ui_panel_handle_t panel);

// ─── Focus stack ─────────────────────────────────────────────────────
//
// Focus semantics — one active widget in one active panel.  Modal
// panels push a barrier so events don't leak past.  Escape / click
// outside modal pops.  The stack survives layer destruction (if a
// focused widget is destroyed, focus falls back to the panel that
// pushed it). Retired input handles are pruned before every route/query.
// If a callback destroys a transition participant or starts a nested focus
// transition, the outer operation stops dispatch and returns
// SAO_UI_STATUS_ERR_BUSY.

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_set_focus_widget(
    sao_ui_input_router_deep_handle_t handle, sao_ui_widget_handle_t widget);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_focus_next(sao_ui_input_router_deep_handle_t handle,
                               bool reverse); // Shift+Tab semantics

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_get_focus(sao_ui_input_router_deep_handle_t handle,
                              sao_ui_widget_handle_t* out_widget, sao_ui_panel_handle_t* out_panel);

// Push a modal barrier for the given non-NULL panel.  Focus can't move
// outside this panel until the barrier is popped (typically on panel close).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_push_modal(
    sao_ui_input_router_deep_handle_t handle, sao_ui_panel_handle_t panel);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_pop_modal(sao_ui_input_router_deep_handle_t handle);
// With no active modal barrier pop_modal is a successful no-op and preserves
// the ordinary focus stack.

// ─── Hover tracking ──────────────────────────────────────────────────
//
// The router auto-synthesizes ENTER/LEAVE events when the hovered
// widget changes.  Callback receives the current hover target — NULL
// when no widget is hovered.
typedef void(SAO_UI_CALL* sao_ui_hover_change_cb_t)(sao_ui_widget_handle_t previous_widget,
                                                    sao_ui_widget_handle_t current_widget,
                                                    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_set_hover_change_handler(
    sao_ui_input_router_deep_handle_t handle, sao_ui_hover_change_cb_t callback, void* user_data);

// ─── Hotkey binding + subset matcher ─────────────────────────────────
//
// Memory [快捷键架构]:
//   * subset matching — F5 fires for both {F5} and {CTRL+F5}
//   * most-specific-wins — if both bindings exist, CTRL+F5 fires the
//     ctrl-variant (higher modifier count), plain F5 fires the plain
//   * modifier reality via GetAsyncKeyState (immune to LL-hook drop)
//   * plugin keys forced into CTRL+combo family (main UI reserves F5-F12
//     bare and CTRL-less).  Set `enforce_ctrl_prefix` = true when
//     registering plugin hotkeys.

enum sao_ui_hotkey_scope_e : int32_t {
    SAO_UI_HOTKEY_SCOPE_GLOBAL = 0,     // fires anywhere
    SAO_UI_HOTKEY_SCOPE_HOST_FOCUS = 1, // only when overlay has focus
    SAO_UI_HOTKEY_SCOPE_PANEL = 2,      // only when specific panel focused
};

struct SaoUiHotkeyBindingSpec {
    const char* binding_id_utf8;     // opaque plugin-defined
    uint32_t virtual_key;            // VK_F5 etc.
    uint32_t modifiers;              // SAO_UI_MOD_* bitmask
    int32_t scope;                   // sao_ui_hotkey_scope_e
    const char* scope_panel_id_utf8; // for SCOPE_PANEL
    bool enforce_ctrl_prefix;        // plugin hotkey guardrail
    bool prevent_default;            // suppress underlying game key
    bool allow_repeat;               // fire on key auto-repeat
    bool require_release;            // fire only on key-up
    uint8_t _pad[4];
};

typedef void(SAO_UI_CALL* sao_ui_hotkey_cb_t)(const char* binding_id_utf8,
                                              const SaoUiInputEvent* triggering_event,
                                              void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_register_hotkey(
    sao_ui_input_router_deep_handle_t handle, const char* plugin_id_utf8,
    const SaoUiHotkeyBindingSpec* spec, sao_ui_hotkey_cb_t callback, void* user_data,
    sao_ui_hotkey_binding_t* out_binding);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_unregister_hotkey(
    sao_ui_input_router_deep_handle_t handle, sao_ui_hotkey_binding_t binding);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_unregister_plugin_hotkeys(
    sao_ui_input_router_deep_handle_t handle, const char* plugin_id_utf8);

// Query which hotkeys would match a hypothetical (vk, modifiers) pair.
// Useful for the settings UI when the user is about to bind a key.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_find_conflicts(
    sao_ui_input_router_deep_handle_t handle, uint32_t virtual_key, uint32_t modifiers,
    sao_ui_hotkey_binding_t* out_bindings, size_t capacity, size_t* out_written);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_match_hotkey(
    sao_ui_input_router_deep_handle_t handle, const SaoUiInputEvent* event,
    sao_ui_hotkey_binding_t* out_binding);

// ─── Cursor override + capture ───────────────────────────────────────
//
// Some widgets (custom drag handles) need to grab the cursor.  When set,
// mouse events go to `captured` even outside its region until released.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_capture_mouse(
    sao_ui_input_router_deep_handle_t handle, sao_ui_widget_handle_t captured_widget);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_input_router_release_mouse(sao_ui_input_router_deep_handle_t handle);

#ifdef __cplusplus
} // extern "C"
#endif
