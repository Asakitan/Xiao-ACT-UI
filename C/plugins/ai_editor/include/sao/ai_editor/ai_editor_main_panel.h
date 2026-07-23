// AI Editor main panel — in-process SDK panel exposing the subprocess
// backend on a compositor layer, replacing the historical stand-alone
// Win32 window (SaoAiEditor.exe --headless still owns the MCP / native
// runtime; UI is now a plugin panel in SaoAuto.exe).
//
// Wire-up:
//   * Launcher spawns the subprocess in --headless mode. Its owner-thread
//     UI service borrows the platform compositor and creates/shows/ticks this
//     panel only after the named-pipe handshake is ready.
//   * The panel provides an asynchronous native Chat + History loop. A single
//     background worker serializes bounded JSON-RPC calls while owner-thread
//     tick only merges completions/events, drains sao.event envelopes through
//     events.drain, reconciles with run.status when needed, and publishes a
//     size-bounded ui_spec with a compact core-chat fallback. Stop/New Chat
//     also cancel the current Starting generation at RPC step boundaries.
//     Composer input uses the compositor-native non-blocking dialog.
//   * Settings / GPU Hunt remain lazily-created compositor child panels. A
//     child hide failure leaves the parent main panel visible for retry.
//   * The panel is unregistered before sao_ai_editor_destroy() so the
//     Settings, GPU Hunt, and main layers are torn down on the compositor
//     owner thread before subprocess shutdown. BUSY keeps ownership with the
//     caller for a later retry.
//
// This is the modern peer to gpu_hunt_central_panel (SDK panel API +
// action handler + body ui_spec).  Zero Win32 controls in the main
// process — everything renders on a compositor layer.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ui/compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ai_editor_main_panel_s* sao_ai_editor_main_panel_t;

#define SAO_AI_EDITOR_MAIN_PANEL_ID "sao.ai_editor.main"

// Create the panel and attach it to the given compositor.  Ownership of
// the compositor and launcher handles remains with the caller; the
// panel only borrows them.  The launcher may be NULL for headless tests
// that only exercise the action_handler wiring (Send buttons then
// short-circuit with an error message). Create/show/hide/tick and mutating
// test dispatch are compositor-owner-thread operations; foreign calls return
// SAO_AI_EDITOR_ERR_PERMISSION_DENIED without changing panel state.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_create(
    sao_ui_compositor_handle_t borrowed_compositor, sao_ai_editor_launcher_t borrowed_launcher,
    sao_ai_editor_main_panel_t* out_panel);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_show(sao_ai_editor_main_panel_t panel);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_hide(sao_ai_editor_main_panel_t panel);

// Owner-thread periodic service. Merges background RPC completions, schedules
// bounded events.drain delivery for the active run plus run.status fallback,
// refreshes the budgeted Chat/History ui_spec, and ticks the composer dialog
// plus any lazily-created Settings/GPU Hunt child panel. body_set_spec remains
// idempotent on unchanged JSON.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_tick(sao_ai_editor_main_panel_t panel);

// Retire the panel.  Unregisters the action handler, destroys the
// compositor layer, and releases the internal state.  Returns BUSY if a
// pending action_handler callback, queued/in-flight RPC, or active run still
// exists; caller must retry. Compositor owner preflight occurs before the
// destroy claim or handler/state mutation. A failed teardown rollback reports
// the handler restoration failure rather than pretending the original step
// succeeded.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_try_destroy(sao_ai_editor_main_panel_t panel);

// Test seam — synthetically dispatch an action_id as if the user
// clicked the corresponding widget.  Only compiled when
// SAO_AI_EDITOR_TESTING is defined; returns SAO_AI_EDITOR_ERR_HANDLE_INVALID
// otherwise.  Payload UTF-8 JSON may be NULL/0-length.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_dispatch_action_for_testing(
    sao_ai_editor_main_panel_t panel, const char* action_id_utf8, const uint8_t* payload_json_utf8,
    size_t payload_len);

// Test seam — read the current output text (accumulated backend
// responses).  Copies UTF-8 into caller buffer; returns required length
// via out_len even on short buffers.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_snapshot_output_for_testing(
    sao_ai_editor_main_panel_t panel, char* buffer_utf8, size_t buffer_cap, size_t* out_len);

#if defined(SAO_AI_EDITOR_TESTING)
void SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_test_set_owner_preflight_pause(int32_t target);

int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_test_owner_preflight_waiting_target(void);

bool SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_test_destroy_registry_probe_completed(void);
#endif

#ifdef __cplusplus
} // extern "C"
#endif
