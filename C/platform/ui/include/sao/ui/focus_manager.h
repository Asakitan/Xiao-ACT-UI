// SAO Auto — keyboard focus manager.
//
// Centralises the per-panel focus ring + Tab/Shift-Tab traversal order
// that input_router.cpp already plumbs via sao_ui_widget_set_focused
// (the focused flag consumed by d2d_widgets.cpp paint_widget).  This
// manager owns the ordered list of focusable widget handles for the
// active panel and drives set_focused on enter/leave so the existing
// focus ring (accent token, paint_focus_ring) renders without any
// widget-side change.
//
// Game-agnostic: panels (workshop, plugin manager, any SDK-registered
// panel) register their focusable widgets and call tab_next/prev; the
// manager never touches plugin code.  The focus ring colour is the
// accent theme token (SAO_UI_TOKEN_APP_ACCENT); ring thickness is 1px
// (matches paint_focus_ring's outer = bounds + 1px).
//
// Conventions mirror widget_kit: opaque handle, lock-protected state,
// transactional JSON props for the order list, status-code returns.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_focus_s* sao_ui_focus_handle_t;

// ── Lifecycle ─────────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_create(
    sao_ui_focus_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_focus_destroy(
    sao_ui_focus_handle_t handle);

// ── Order management ──────────────────────────────────────────────
// Append a focusable widget to the traversal order.  Duplicate handles
// are ignored.  Returns SAO_STATUS_ERR_HANDLE_INVALID if widget is null.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_register(
    sao_ui_focus_handle_t handle,
    sao_ui_widget_handle_t widget);

// Remove a widget from the order (e.g. when it is destroyed).  Clears
// focus if the removed widget was focused.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_unregister(
    sao_ui_focus_handle_t handle,
    sao_ui_widget_handle_t widget);

// Replace the entire order atomically.  Duplicate handles in the new
// list are de-duplicated; order preserved on first occurrence.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_set_order(
    sao_ui_focus_handle_t handle,
    const sao_ui_widget_handle_t* widgets,
    size_t count);

// Clear the order and release focus.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_clear(
    sao_ui_focus_handle_t handle);

// ── Focus control ─────────────────────────────────────────────────
// Set focus to a specific widget (must be registered).  Clears the
// previous focused widget's flag and sets the new one's.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_set(
    sao_ui_focus_handle_t handle,
    sao_ui_widget_handle_t widget);

// Get the currently focused widget (nullptr if none).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_get(
    sao_ui_focus_handle_t handle,
    sao_ui_widget_handle_t* out_widget);

// Advance focus to the next / previous widget in Tab order.  reverse=
// true → Shift+Tab semantics.  Wraps around.  No-op if order is empty.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_tab_next(
    sao_ui_focus_handle_t handle,
    bool reverse);

// ── Transactional props ────────────────────────────────────────────
// Accepted shape: { "order": [<widget handle int>...] }
// Each entry is the integer representation of a sao_ui_widget_handle_t.
// Invalid / null handles are skipped; the rest set the order.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_apply_props(
    sao_ui_focus_handle_t handle,
    const uint8_t* props_json_utf8,
    size_t props_len);

// ── Introspection ──────────────────────────────────────────────────
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_order_count(
    sao_ui_focus_handle_t handle,
    size_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_focus_order_at(
    sao_ui_focus_handle_t handle,
    size_t index,
    sao_ui_widget_handle_t* out_widget);

#ifdef __cplusplus
} // extern "C"
#endif