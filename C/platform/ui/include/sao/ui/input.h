// SAO Auto — input routing (click-through + hotkey + focus shield).
//
// Python authoritative source:
//   `render/overlay_host.py` — WM_MOUSEACTIVATE / MA_NOACTIVATE
//   `render/overlay_compositor.py::_proxy_shield_activation` (line 350-397)
//   `sao_gui_hotkey.py`      — global hotkey listener
//   memory `input proxy逐像素+防焦点偷` (per-pixel input proxy)
//
// The overlay host installs a WH_MOUSE_LL / WH_KEYBOARD_LL pair.  This
// module owns the state machine that decides "does this click land on
// the overlay or fall through to the game?".
//
// ── Focus shield contract (mandatory) ──────────────────────────
//   WS_EX_NOACTIVATE alone is NOT enough for a Tk toplevel — Tk's
//   own message handling still promotes to foreground on click.
//   The fix: answer WM_MOUSEACTIVATE with MA_NOACTIVATE at the
//   WndProc level.  This must be done EXACTLY ONCE per proxy.
//   Re-subclassing on every tick forms a CallWindowProc chain of
//   death; each re-arm chains through the PREVIOUS proc; the chain
//   deepens until the per-message stack overflows and the process
//   crashes with no Python traceback ("freeze then crash-exit").
//   The Python code uses a `_sao_shielded` guard; the C++ port
//   uses `shield_arm_once` returning false on a second call.
//
// ── ctypes 64-bit trap (mirrored architecturally in C++) ─────
//   The Python code has a dedicated `_user32_subcls` copy of user32
//   with EXPLICIT ctypes.argtypes for HWND (pointer-width, not 32-bit
//   int).  Without this, GetAncestor truncates to 32-bit → each tick
//   sees a different value → shield_arm_once guard fails → chain
//   of death.  In C++ this is naturally correct via HWND (void*),
//   but the corresponding invariant is: never cast HWND through
//   int32_t.
//
// ── Cursor override ───────────────────────────────────────────
//   A layer can request the mouse cursor when hovered.  The router
//   arbitrates between competing requests: topmost-hovered layer wins.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/overlay_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_input_router_s* sao_ui_input_router_handle_t;

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_create(
    sao_ui_overlay_host_handle_t host,
    sao_ui_input_router_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_input_router_destroy(
    sao_ui_input_router_handle_t handle);

// Rebuild the click-through region from the current union of layers +
// widgets that accept input.  Called every frame after present.
// Consumes the compositor's layer set + input-proxy shapes and passes
// through SetWindowRgn (never NULL).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_rebuild_region(
    sao_ui_input_router_handle_t handle);

// Replace the current interactive rectangles in host-client coordinates.
// Call rebuild_region() after a layout/frame boundary to apply the temporal
// union through the overlay host.  Passing an empty list intentionally makes
// the complete render host click-through.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_set_regions(
    sao_ui_input_router_handle_t handle,
    const SaoOverlayHostInputRect* rects,
    size_t rect_count);

// ── Focus shield ─────────────────────────────────────────────

// Arm the focus shield on the given proxy HWND.  Must be called
// EXACTLY ONCE per HWND; a second call returns SAO_STATUS_ERR_ALREADY_
// EXISTS.  Idempotent from the caller's POV — safe to call from a
// 200 ms tick as long as the caller respects the return value and
// doesn't retry on a spurious "err" that turned out to be already-armed.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_shield_arm_once(
    sao_ui_input_router_handle_t handle,
    void* proxy_hwnd);

// Query whether the shield is armed for this HWND.
SAO_UI_API bool SAO_UI_CALL sao_ui_input_router_shield_armed(
    sao_ui_input_router_handle_t handle,
    void* proxy_hwnd);

// ── Global hotkey management ─────────────────────────────────

enum sao_ui_hotkey_modifier_e : uint32_t {
    SAO_UI_MOD_CTRL  = 1u << 0,
    SAO_UI_MOD_ALT   = 1u << 1,
    SAO_UI_MOD_SHIFT = 1u << 2,
    SAO_UI_MOD_WIN   = 1u << 3,
};

typedef void (SAO_UI_CALL* sao_ui_hotkey_callback_t)(
    uint32_t hotkey_id, void* user_data);

// Register a global hotkey.  hotkey_id must be unique — reuse
// returns SAO_STATUS_ERR_ALREADY_EXISTS.  virtual_key is a VK_ code
// (VK_F5=0x74 etc.).  Handles hotkey coalescing: registering CTRL+F5
// alongside F5 uses the most-specific-first match with modifier
// state from GetAsyncKeyState (see memory `快捷键架构` — three-
// listener architecture: Tk / webview / headless share the config
// parse layer, subset match + most-specific-first).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_register_global_hotkey(
    sao_ui_input_router_handle_t handle,
    uint32_t hotkey_id,
    uint32_t virtual_key,
    uint32_t modifier_mask,
    sao_ui_hotkey_callback_t callback,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_unregister_global_hotkey(
    sao_ui_input_router_handle_t handle,
    uint32_t hotkey_id);

// Enumerate current hotkey bindings (for the diagnostic panel).
struct SaoHotkeyBinding {
    uint32_t hotkey_id;
    uint32_t virtual_key;
    uint32_t modifier_mask;
    uint32_t _pad;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_list_hotkeys(
    sao_ui_input_router_handle_t handle,
    SaoHotkeyBinding* out_bindings, size_t capacity,
    size_t* out_count);

// ── Cursor override ──────────────────────────────────────────

enum sao_ui_cursor_e : int32_t {
    SAO_UI_CURSOR_ARROW = 0,
    SAO_UI_CURSOR_HAND  = 1,
    SAO_UI_CURSOR_TEXT  = 2,
    SAO_UI_CURSOR_RESIZE_NS = 3,
    SAO_UI_CURSOR_RESIZE_EW = 4,
    SAO_UI_CURSOR_RESIZE_NWSE = 5,
    SAO_UI_CURSOR_RESIZE_NESW = 6,
    SAO_UI_CURSOR_CROSSHAIR = 7,
    SAO_UI_CURSOR_WAIT      = 8,
    SAO_UI_CURSOR_HIDDEN    = 9,   // hide entire cursor over layer
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_set_cursor(
    sao_ui_input_router_handle_t handle,
    int32_t cursor_kind);

// Per-layer cursor override — when this layer is topmost-hovered,
// use this cursor.  0 → clear.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_set_layer_cursor(
    sao_ui_input_router_handle_t handle,
    const char* layer_name_utf8,
    int32_t cursor_kind);

// ── Low-level input hooks ────────────────────────────────────

// Route raw mouse/keyboard events from a WH_MOUSE_LL / WH_KEYBOARD_LL
// hook to the router.  Called only from the LL hook thread.
typedef bool (SAO_UI_CALL* sao_ui_low_level_hook_t)(
    uint32_t code, uint64_t wparam, uint64_t lparam, void* user_data);
    
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_set_ll_hook_callbacks(
    sao_ui_input_router_handle_t handle,
    sao_ui_low_level_hook_t mouse_callback,
    sao_ui_low_level_hook_t keyboard_callback,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_install_ll_hooks(
    sao_ui_input_router_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_router_uninstall_ll_hooks(
    sao_ui_input_router_handle_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
