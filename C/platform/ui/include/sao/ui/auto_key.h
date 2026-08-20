// SAO Auto — game-agnostic auto-key / SendInput bridge.
//
// This is the *platform-level* SendInput wrapper.  It emits synthetic
// keyboard and mouse events via Win32 SendInput.  Callers include the
// per-game plugins (`auto_key_engine.py`, `mp_input.py`, hide/seek etc.)
// through the plugin SDK.  No game-specific key mappings live here.
//
// ── SendInput race handling ─────────────────────────────────────
//   SendInput is atomic per call (kernel splices the events into the
//   input stream in one commit), but hold-then-release is two calls
//   with an intentional gap.  This wrapper spawns a lightweight timer
//   thread so hold_ms can span multiple frames without blocking the
//   caller.  Every outstanding hold has an owner token and is drained
//   in ~destructor / cancel path so no key gets stuck if the process
//   exits mid-hold.
//
// ── UTF-16 text ─────────────────────────────────────────────────
//   Text injection uses KEYEVENTF_UNICODE with wVk = 0.  Each wchar_t
//   emits a scan-code == wchar and KEYEVENTF_UNICODE, so the input
//   subsystem synthesizes the correct WM_CHAR without touching the
//   keyboard layout.  Surrogate pairs go through as two consecutive
//   scan codes (Windows handles the reassembly).
//
// ── Arbitration surface (game-agnostic) ────────────────────────
//   The public API includes a lightweight `arbitrate` gate that lets
//   callers ask "am I allowed to send this VK right now?"  The gate
//   consults an optional policy table (see `input_arbitration.h`) so
//   two plugins can share the same physical key without stepping on
//   each other and so a user-driven physical press wins over an
//   automation-driven press.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Arbitration policy ─────────────────────────────────────────
//
// GAME_ONLY  — physical input only; automation blocked.  Used when a
//              user has explicitly reserved a key for gameplay only.
// PLUGIN_ONLY — automation only; physical input suppressed at the LL
//              hook level (the caller is responsible for the hook).
// SHARED     — both allowed (default).
// BLOCKED    — neither allowed (temporary lockout window).
enum sao_ui_auto_key_policy_e : int32_t {
    SAO_UI_AUTO_KEY_POLICY_SHARED       = 0,
    SAO_UI_AUTO_KEY_POLICY_GAME_ONLY    = 1,
    SAO_UI_AUTO_KEY_POLICY_PLUGIN_ONLY  = 2,
    SAO_UI_AUTO_KEY_POLICY_BLOCKED      = 3,
};

// Modifier bits use the same ABI as the input-router hotkey masks.
enum sao_ui_auto_key_modifier_e : uint32_t {
    SAO_UI_AUTO_KEY_MOD_CTRL  = 1u << 0,
    SAO_UI_AUTO_KEY_MOD_ALT   = 1u << 1,
    SAO_UI_AUTO_KEY_MOD_SHIFT = 1u << 2,
    SAO_UI_AUTO_KEY_MOD_WIN   = 1u << 3,
};
#define SAO_UI_AUTO_KEY_MODIFIER_MASK 0x0Fu

// Mouse button selector for auto-key mouse click.
enum sao_ui_auto_key_mouse_button_e : int32_t {
    SAO_UI_AUTO_KEY_MOUSE_LEFT   = 0,
    SAO_UI_AUTO_KEY_MOUSE_RIGHT  = 1,
    SAO_UI_AUTO_KEY_MOUSE_MIDDLE = 2,
    SAO_UI_AUTO_KEY_MOUSE_X1     = 3,
    SAO_UI_AUTO_KEY_MOUSE_X2     = 4,
};

// ── SendInput-style keyboard event ─────────────────────────────
//
// Emit a single virtual-key press.  When `hold_ms` == 0 the release
// follows the press immediately (same SendInput batch).  When
// `hold_ms` > 0 the release is queued on the background timer thread
// so the caller may return while the key is still down.  `modifiers`
// is a bitmask of Win32 VK_* that must be held during the emission;
// each modifier is pressed before the main VK and released after it.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_send_key(
    uint32_t virtual_key,
    uint32_t modifiers_mask,
    uint32_t hold_ms);

// Multi-key simultaneous press with a single hold window.  All keys
// go down atomically (single SendInput batch), then release atomically
// after `hold_ms`.  `vk_array` may not be null when `count` > 0.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_send_key_combo(
    const uint32_t* vk_array,
    size_t count,
    uint32_t hold_ms);

// Inject the raw UTF-16 code units as WM_CHAR events (via
// KEYEVENTF_UNICODE).  `text_utf16` must be a null-terminated wchar_t
// sequence; `count` is optional (0 → strlen).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_send_text(
    const uint16_t* text_utf16,
    size_t count);

// Emit a mouse click at absolute screen coordinates (px).  When
// x_screen_px == y_screen_px == INT32_MIN the click is issued at the
// current cursor location (no MOUSEEVENTF_ABSOLUTE flag).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_send_mouse_click(
    int32_t x_screen_px,
    int32_t y_screen_px,
    int32_t button);

// GetAsyncKeyState wrapper.  is_down_out receives true when the high
// bit is set (currently held); false otherwise.  Never blocks.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_get_key_state(
    uint32_t virtual_key,
    bool* is_down_out);

// Consult the policy gate for a VK before emitting.  `policy` is the
// caller's declared source (SHARED for a generic plugin, GAME_ONLY for
// a user-forced physical-only VK, etc.).  Returns SAO_STATUS_OK and
// writes `allowed_out=true` when the emission is permitted; writes
// false otherwise.  Never returns SAO_STATUS_ERR_ACCESS_DENIED so the
// caller can branch on `allowed_out` without status-code plumbing.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_auto_key_arbitrate(
    int32_t policy,
    uint32_t virtual_key,
    bool* allowed_out);

#ifdef __cplusplus
}  // extern "C"
#endif
