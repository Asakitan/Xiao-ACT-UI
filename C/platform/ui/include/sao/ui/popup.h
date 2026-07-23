// SAO Auto — GPU-native transient popup menu.
//
// Python source of truth:
//   - `sao_auto/python/ui_gpu/popup.py`  (SAOPopUpMenu — 1507 lines,
//     the *production* popup that every entity menu ultimately opens.
//     See [entity菜单=ui_gpu管线] memory note.)
//   - `sao_auto/python/ui_gpu/composer.py`   (RGBA -> BGRA compose)
//   - `sao_auto/python/ui_gpu/menu_bar_layout.py`  (9-slot fisheye col)
//   - `sao_auto/python/ui_gpu/child_bar_layout.py` (slide-in rows)
//   - `sao_auto/python/ui_gpu/hud_layout.py`  (frame brackets + rails)
//   - `sao_auto/python/ui_gpu/hit_test.py`  (cursor → (kind, idx))
//   - `sao_auto/python/ui_gpu/state.py`  (PopupState dataclass)
//   - `sao_auto/python/sao_theme/popup_menu.py`  (legacy Tk shell —
//     only used for the left_widget factory host; the compose+present
//     path is fully in ui_gpu)
//
// The GPU popup owns one GLFW/DirectComposition layer and does its own
// compose in a worker thread.  Every menu (right-click, context,
// dropdown) lands here.  For the SAO main-ring menu, see menu.h; that
// wraps popup.h internally.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/menu.h"
#include "sao/ui/theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_popup_s* sao_ui_popup_handle_t;

// ── Entry POD ─────────────────────────────────────────────────────
// entry_id: what the result callback receives on click.  -1 reserved
// for separators.  Sub-menus use submenu_entries != nullptr with
// submenu_count > 0; the entry_id then identifies the "expand" event
// (chosen_entry_id in the callback, dismissed=false) even when the
// user is just navigating.
//
// ABI stability contract (intentional non-guard):
//   SaoUiPopupEntry deliberately has NO leading struct_size field. Entries
//   are almost always passed as an array (SaoUiPopupSpec::entries[]) whose
//   packing depends on element stride == sizeof(SaoUiPopupEntry), so adding
//   a per-element size field would either require a separate stride field
//   in every array ABI (SaoUiPopupSpec, refresh_entries) or force every
//   caller to reinitialise the field on each element with no independent
//   value. Instead, this struct is frozen: any future field additions must
//   ship as a new "v2" struct with a distinct type name and a distinct
//   registration entry point (mirroring the context/native provider v1→v2
//   pattern in the plugins ABI). Do NOT append fields here.
struct SaoUiPopupEntry {
    const char* label_utf8;
    const char* icon_utf8;              // optional glyph (nullable)
    const char* accelerator_utf8;       // right-aligned hint text (nullable)
    int32_t     entry_id;
    bool        enabled;
    bool        checked;
    bool        is_separator;
    bool        _pad;

    // Sub-menu (nullable).  Points to another SaoUiPopupEntry array.
    const struct SaoUiPopupEntry* submenu_entries;
    size_t                         submenu_count;
};

// ── Popup spec (build-time) ───────────────────────────────────────
// theme_override == SAO_UI_THEME_COUNT means "inherit the global
// theme"; any other value locks this popup to a specific palette
// (e.g. plugins can force a light popup over a dark app).
struct SaoUiPopupSpec {
    // Parent identity.  parent_hwnd may be 0 for a screen-wide popup;
    // for anchored popups it's the source window (used for z-order
    // + focus loss tracking).
    void*       parent_hwnd;
    int32_t     anchor_x;               // screen coord
    int32_t     anchor_y;
    int32_t     anchor_w;               // used for edge-flip logic
    int32_t     anchor_h;

    // Content
    const SaoUiPopupEntry* entries;
    size_t                 entry_count;

    // Theme
    SaoUiThemeId theme_override;        // SAO_UI_THEME_COUNT = inherit

    // Behavior
    bool        cascade_from_top;       // popup.py cascade_mode
    bool        allow_keyboard_nav;     // arrows + Enter; ESC always dismisses
    bool        dismiss_on_focus_out;
    bool        _pad;
    int32_t     fade_in_ms;             // default 450 (matches Python)
    int32_t     fade_out_ms;            // default 300
};

// ── Result / lifecycle callback ───────────────────────────────────
// chosen_entry_id: valid only when dismissed == false.
// screen_x/y: cursor position at the moment of click (or -1 if
//             keyboard-driven / dismissed).
typedef void (SAO_UI_CALL* sao_ui_popup_result_callback_t)(
    int32_t chosen_entry_id,
    bool dismissed,
    int32_t screen_x, int32_t screen_y,
    void* user_data);

// ── Compile-time invariants (memory-note trap doc) ────────────────
// From [连续反馈不对要停止调参数]:
//   "menu HUD content_w/h was cached but left/top read live → child
//    bar slide-in de-synced backdrop."
//
// Popup layout constants MUST stay compile-time.  Any runtime mutation
// (dynamic entry count → layout resize) recomputes ALL of these each
// tick — never partially cache.
struct SaoUiPopupLayoutConsts {
    // Menu column (fisheye) — mirrors menu_bar_layout.py
    int32_t menu_size;                  // 54
    int32_t menu_max_size;              // 70
    int32_t menu_slot;                  // 70
    int32_t menu_width;                 // 70
    int32_t menu_max_visible;           // 9
    // Child bar — mirrors child_bar_layout.py
    int32_t child_width;                // 240 (nominal)
    int32_t child_row_stride;           // ~40
    int32_t child_slide_ms;             // 240
    // HUD compose padding (matches MenuHudSpriteRenderer.gpu_pad)
    int32_t hud_pad;
    int32_t hud_margin;
    // Master gap between menu column and child column
    int32_t gap_menu_child;             // 25
};

SAO_UI_API const SaoUiPopupLayoutConsts* SAO_UI_CALL
    sao_ui_popup_layout_constants(void);

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_create(
    sao_ui_compositor_handle_t compositor,
    sao_ui_theme_handle_t theme,
    sao_ui_popup_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_popup_destroy(
    sao_ui_popup_handle_t handle);

// ── Show / hide ───────────────────────────────────────────────────
// The result callback fires once per popup lifecycle — either on
// entry click, ESC, or focus-out (spec.dismiss_on_focus_out).
// After the callback fires, the popup destroys its GPU layer; the
// handle can be reused for the next show without recreating.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_show(
    sao_ui_popup_handle_t handle,
    const SaoUiPopupSpec* spec,
    sao_ui_popup_result_callback_t callback,
    void* user_data);

// Programmatic dismiss.  Fires the callback with dismissed=true.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_hide(
    sao_ui_popup_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_is_visible(
    sao_ui_popup_handle_t handle, bool* out_visible);

// ── Runtime mutation ──────────────────────────────────────────────
// Refresh the entry list without hiding.  The popup animates the
// difference (added rows slide in, removed rows fade out).
// Mirrors SAOPopUpMenu.refresh_child_menus.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_refresh_entries(
    sao_ui_popup_handle_t handle,
    const SaoUiPopupEntry* entries,
    size_t entry_count);

// Toggle an entry's checked or enabled state.  entry_id must match.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_set_entry_checked(
    sao_ui_popup_handle_t handle, int32_t entry_id, bool checked);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_set_entry_enabled(
    sao_ui_popup_handle_t handle, int32_t entry_id, bool enabled);

// ── Keyboard nav ──────────────────────────────────────────────────
enum SaoUiPopupNavKey : int32_t {
    SAO_UI_POPUP_KEY_UP    = 0,
    SAO_UI_POPUP_KEY_DOWN  = 1,
    SAO_UI_POPUP_KEY_LEFT  = 2,   // collapse sub-menu / dismiss
    SAO_UI_POPUP_KEY_RIGHT = 3,   // expand sub-menu
    SAO_UI_POPUP_KEY_ENTER = 4,   // activate hovered entry
    SAO_UI_POPUP_KEY_ESC   = 5,   // dismiss (callback with dismissed=true)
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_key_press(
    sao_ui_popup_handle_t handle,
    SaoUiPopupNavKey key);

// ── Hover / manual hit-test ───────────────────────────────────────
// Test a cursor position against the popup.  entry_id == -1 when
// off any entry.  submenu_depth is 0 for the root popup.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_popup_hit_test(
    sao_ui_popup_handle_t handle,
    int32_t x, int32_t y,
    int32_t* out_entry_id,
    int32_t* out_submenu_depth);

#ifdef __cplusplus
}  // extern "C"
#endif
