// AI Editor main panel — in-process SDK panel exposing the subprocess
// backend on a compositor layer, replacing the historical stand-alone
// Win32 window (SaoAiEditor.exe --headless still owns the MCP / native
// runtime; UI is now a plugin panel in SaoAuto.exe).
//
// Wire-up:
//   * Launcher (tool_launch_internal.cpp::launch_ai_editor) spawns the
//     subprocess in --headless mode, then calls
//     sao_ai_editor_main_panel_create(compositor, launcher_handle, &panel).
//   * The panel routes user-triggered actions (Send Ping / Send Hello /
//     Clear) via sao_ai_editor_request() (synchronous named-pipe
//     request/response) and appends responses to the output textarea.
//   * The panel is unregistered before sao_ai_editor_destroy() so the
//     compositor layer is torn down cleanly on subprocess exit.
//
// This is the modern peer to gpu_hunt_central_panel (SDK panel API +
// action handler + body ui_spec).  Zero Win32 controls in the main
// process — everything renders on a compositor layer.

#pragma once

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
// short-circuit with an error message).
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_create(
    sao_ui_compositor_handle_t borrowed_compositor,
    sao_ai_editor_launcher_t borrowed_launcher,
    sao_ai_editor_main_panel_t* out_panel);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_show(
    sao_ai_editor_main_panel_t panel);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_hide(
    sao_ai_editor_main_panel_t panel);

// Optional periodic tick (currently just refreshes body if dirty).  Safe
// to call at any cadence; body_set_spec is idempotent on unchanged JSON.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_tick(
    sao_ai_editor_main_panel_t panel);

// Retire the panel.  Unregisters the action handler, destroys the
// compositor layer, and releases the internal state.  Returns BUSY if a
// pending action_handler callback is still in flight; caller must retry.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_main_panel_try_destroy(
    sao_ai_editor_main_panel_t panel);

// Test seam — synthetically dispatch an action_id as if the user
// clicked the corresponding widget.  Only compiled when
// SAO_AI_EDITOR_TESTING is defined; returns SAO_AI_EDITOR_ERR_HANDLE_INVALID
// otherwise.  Payload UTF-8 JSON may be NULL/0-length.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_dispatch_action_for_testing(
    sao_ai_editor_main_panel_t panel,
    const char* action_id_utf8,
    const uint8_t* payload_json_utf8,
    size_t payload_len);

// Test seam — read the current output text (accumulated backend
// responses).  Copies UTF-8 into caller buffer; returns required length
// via out_len even on short buffers.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_main_panel_snapshot_output_for_testing(
    sao_ai_editor_main_panel_t panel,
    char* buffer_utf8,
    size_t buffer_cap,
    size_t* out_len);

#ifdef __cplusplus
}  // extern "C"
#endif
