// SAO Auto — modal dialog (Alert / Confirm / Info / Input).
//
// Python source of truth:
//   - `sao_auto/python/sao_theme/dialogs.py`
//     (SAODialog / _clip_reveal / SAOLeaderboardDialog)
//   - `sao_auto/python/sao_theme/file_picker.py`  (SAOFilePicker —
//     related expand-then-reveal path)
//   - `sao_auto/python/sao_theme/utils.py`
//     (_make_aa_icon_button — the Yes/No SVG-style icon buttons)
//
// SAODialog is the three-section (title 68 / content / buttons 83)
// modal that opens via a width-expand animation (135px → 375px, ~500ms)
// then clip-reveals text.  Close is a shrink animation.  It uses the
// SaoToplevel mirror_z=2000 layer to always sit above panels, per
// dialogs.py's _SAO_DIALOG_MIRROR_Z note.
//
// The dialog is intentionally *non-blocking* — Python calls it
// callback-driven (no wait_window, no grab_set) to avoid the
// overrideredirect+grab silent-hang bug.  We keep the same
// convention here.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_dialog_s* sao_ui_dialog_handle_t;

// ── Dialog kind ───────────────────────────────────────────────────
// Mirrors SAODialog.showinfo / showwarning / showerror / ask.  The
// visual difference is icon-only (title/content/buttons all identical
// in the Python impl); we retain the enum so plugins can request the
// canonical icon per intent.
enum SaoUiDialogKind : int32_t {
    SAO_UI_DIALOG_INFO    = 0,
    SAO_UI_DIALOG_WARNING = 1,
    SAO_UI_DIALOG_ERROR   = 2,
    SAO_UI_DIALOG_ASK     = 3,   // Yes/No, shows both OK + Cancel icons
    SAO_UI_DIALOG_INPUT   = 4,   // extends ASK with a text entry
};

// ── Button layout ─────────────────────────────────────────────────
// Anti-aliased circular icons from utils._make_aa_icon_button.
// close button = red (SAOColors.CLOSE_RED = #d13d4f)
// ok button    = blue (SAOColors.OK_BLUE  = #428ce6)
enum SaoUiDialogButton : int32_t {
    SAO_UI_DIALOG_BTN_OK       = 0,
    SAO_UI_DIALOG_BTN_CANCEL   = 1,
    SAO_UI_DIALOG_BTN_YES      = 2,   // alias for OK in ASK variant
    SAO_UI_DIALOG_BTN_NO       = 3,   // alias for CANCEL in ASK variant
    SAO_UI_DIALOG_BTN_CUSTOM   = 4,   // reserved for label overrides
    SAO_UI_DIALOG_BTN_DISMISS  = 5,   // Esc / focus-out
};

// Additional custom label (INFO/WARNING/ERROR default to just OK;
// ASK defaults to OK + CANCEL).  buttons_utf8 is optional (nullptr =
// use kind default).  Each label ≤ 32 bytes.
struct SaoUiDialogButtonSpec {
    SaoUiDialogButton kind;
    const char*       label_utf8;      // nullable — uses canonical text
    uint32_t          color_argb;      // 0 = use kind's default colour
};

// ── Spec ──────────────────────────────────────────────────────────
// title/message MUST be valid UTF-8 (Python does clip-reveal via char
// index; ASCII/multi-byte-safe here means we walk graphemes).
struct SaoUiDialogSpec {
    SaoUiDialogKind kind;
    const char* title_utf8;
    const char* message_utf8;

    // Optional input entry (kind == SAO_UI_DIALOG_INPUT).
    const char* input_prompt_utf8;     // shown above the entry
    const char* input_default_utf8;    // pre-filled text
    int32_t     input_max_length;      // 0 = no cap

    // Custom button set (nullptr = use kind default).
    const SaoUiDialogButtonSpec* buttons;
    size_t                        button_count;

    // Sizing overrides (0 = use Python defaults 375×240)
    int32_t width;
    int32_t height;

    // Animation timings (0 = use defaults matching Python constants)
    int32_t expand_ms;                 // default 500
    int32_t clip_reveal_ms_title;      // default 400
    int32_t clip_reveal_ms_message;    // default 350
    int32_t shrink_ms;                 // default 350

    // Theme override (SAO_UI_THEME_COUNT = inherit)
    SaoUiThemeId theme_override;

    // z-order: dialogs default to mirror_z=2000 to sit above every
    // plugin panel (see _SAO_DIALOG_MIRROR_Z note in dialogs.py).
    // Override for stackable modals (nested confirm).
    int32_t mirror_z;                  // 0 = use 2000 default

    bool    draggable;                 // default true (Python)
    bool    dismiss_on_focus_out;      // default false
    bool    dismiss_on_esc;            // default true
    bool    _pad;
};

// ── Result callback ───────────────────────────────────────────────
// input_text_utf8 is only valid for INPUT dialogs; nullptr otherwise.
// The buffer is owned by the dialog implementation and remains valid
// for the duration of the callback.
typedef void (SAO_UI_CALL* sao_ui_dialog_result_callback_t)(
    SaoUiDialogButton pressed,
    const char* input_text_utf8,
    size_t input_text_len,
    void* user_data);

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    sao_ui_dialog_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_dialog_destroy(
    sao_ui_dialog_handle_t handle);

// Configure masking before showing an INPUT dialog. Existing spec layout is
// unchanged; non-input dialogs ignore the option. Returns BUSY while visible.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_set_input_password(sao_ui_dialog_handle_t handle,
                                                                     bool enabled);

// Fire-and-forget modal show.  Never blocks.  The callback fires
// once, then the internal window animates out and cleans up.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_show(
    sao_ui_dialog_handle_t handle,
    const SaoUiDialogSpec* spec,
    sao_ui_dialog_result_callback_t callback,
    void* user_data);

// Force-close.  Fires callback with pressed = DISMISS.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_hide(
    sao_ui_dialog_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_is_visible(
    sao_ui_dialog_handle_t handle, bool* out_visible);

// ── Convenience one-shot helpers ──────────────────────────────────
// These wrap create+show+destroy in the common case.
// The handle-based API above is preferred for repeated modals from
// the same plugin (avoids repeated compositor layer alloc).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_show_info(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8,
    const char* message_utf8,
    sao_ui_dialog_result_callback_t callback,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_show_warning(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8,
    const char* message_utf8,
    sao_ui_dialog_result_callback_t callback,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_show_error(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8,
    const char* message_utf8,
    sao_ui_dialog_result_callback_t callback,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_show_ask(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    const char* title_utf8,
    const char* message_utf8,
    sao_ui_dialog_result_callback_t callback,
    void* user_data);

// ── Dialog layout and state machine ─────────────────────────────────
// The Python authority uses a fixed three-section dialog by default:
// 375×240 final geometry, 135px initial width, 68px header, 87px content,
// two 1px separators, and an 83px footer.  The renderer consumes this
// snapshot rather than duplicating those constants.
struct SaoUiDialogLayoutSnapshot {
    int32_t width;
    int32_t height;
    int32_t initial_width;
    int32_t header_height;
    int32_t content_height;
    int32_t footer_height;
    int32_t separator_height;
    int32_t expand_ms;
    int32_t title_reveal_delay_ms;
    int32_t title_reveal_ms;
    int32_t message_reveal_delay_ms;
    int32_t message_reveal_ms;
    int32_t shrink_ms;
    int32_t mirror_z;
    float   alpha;
};

// The state machine models the four visible transitions in dialogs.py:
//   IDLE → EXPANDING (500ms) → CLIP_REVEALED → SHRINKING (350ms) → IDLE
// The tick loop consumes dt_ms and advances the state; the caller
// polls sao_ui_dialog_get_state to drive its renderer.  Keyboard
// dispatch (Tab / Shift+Tab / Enter / Esc) fires the result callback
// with the pressed button — the same one the mouse hit would produce.
enum SaoUiDialogState : int32_t {
    SAO_UI_DIALOG_STATE_IDLE          = 0,
    SAO_UI_DIALOG_STATE_EXPANDING     = 1,
    SAO_UI_DIALOG_STATE_CLIP_REVEALED = 2,
    SAO_UI_DIALOG_STATE_SHRINKING     = 3,
};

// Snapshot of one button in the current dialog spec — filled in by
// sao_ui_dialog_get_button_at for the renderer.
struct SaoUiDialogButtonInfo {
    SaoUiDialogButton kind;
    // Effective label — falls back to canonical text for the kind if
    // spec.label_utf8 was nullptr.  Owned by the dialog handle; valid
    // until next show/refresh call.
    const char* label_utf8;
    uint32_t    color_argb;
    bool        is_focused;
    bool        _pad[3];
};

// Advance the state machine by dt_ms milliseconds.  A dt_ms of 0 is a
// legal no-op probe.  Fires the result callback exactly once, at the
// end of SHRINKING → IDLE.  Safe to call whether or not a spec is
// currently active (returns NOT_INITIALIZED if IDLE with no pending
// dismiss).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_tick(
    sao_ui_dialog_handle_t handle, int32_t dt_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_get_state(
    sao_ui_dialog_handle_t handle, SaoUiDialogState* out_state);

// Renderer-facing snapshot of the effective Python-parity geometry and
// animation timings. Valid while the dialog is showing or shrinking.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_get_layout_snapshot(
    sao_ui_dialog_handle_t handle,
    SaoUiDialogLayoutSnapshot* out_snapshot);

// Force-close the dialog with an explicit pressed button.  Transitions
// IDLE/EXPANDING/CLIP_REVEALED → SHRINKING; the callback fires when
// SHRINKING completes.  If called during SHRINKING, replaces the
// pending result.  A pressed value of DISMISS returns dismissed=true
// (matches ESC behavior).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_dismiss(
    sao_ui_dialog_handle_t handle, SaoUiDialogButton pressed);

// Set / query current keyboard focus (button_index in [0, button_count)).
// Passing -1 clears focus; out-of-range returns INVALID_ARGUMENT.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_set_button_focus(
    sao_ui_dialog_handle_t handle, int32_t button_index);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_get_button_focus(
    sao_ui_dialog_handle_t handle, int32_t* out_index);

// Virtual-key dispatch.  VK values match Win32:
//   VK_TAB    = 0x09  (advances focus; SHIFT toggled via shift_held)
//   VK_RETURN = 0x0D  (fires focused button → dismiss with that result)
//   VK_ESCAPE = 0x1B  (dismiss with DISMISS button)
// Returns OK if the key was consumed, NOT_INITIALIZED if the dialog is
// not in a receptive state (SHRINKING or IDLE with no spec).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_dispatch_key(
    sao_ui_dialog_handle_t handle, uint32_t vk, bool shift_held);

// Read out the visible button set (from the currently-showing spec or
// the kind default if no explicit buttons were provided).  out_count
// is the total; if buffer is non-null, up to *buffer_capacity infos
// are copied in.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_get_button_count(
    sao_ui_dialog_handle_t handle, int32_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_dialog_get_button_at(
    sao_ui_dialog_handle_t handle, int32_t index,
    SaoUiDialogButtonInfo* out_info);

#ifdef __cplusplus
}  // extern "C"
#endif
