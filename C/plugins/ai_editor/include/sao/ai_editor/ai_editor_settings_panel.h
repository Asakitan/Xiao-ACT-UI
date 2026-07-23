// AI Editor settings panel — compositor-native, scope-aware settings UI.
//
// The panel borrows the platform compositor and the headless AI Editor
// launcher. Settings metadata and values are exchanged with the subprocess
// through JSON-RPC settings.describe / settings.load / settings.save calls.
// No top-level Win32 window is created by this API.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_status.h"
#include "sao/ui/compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ai_editor_settings_panel_s* sao_ai_editor_settings_panel_t;

#define SAO_AI_EDITOR_SETTINGS_PANEL_ID "sao.ai_editor.settings"

// Create a hidden settings panel. The compositor and launcher are borrowed;
// the caller must retire this panel before destroying either dependency. A
// null launcher is accepted for offline/headless UI validation. Create,
// show, hide, tick, and callback-driven mutations require the compositor
// owner thread; foreign calls fail before registration or state mutation.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_create(
    sao_ui_compositor_handle_t borrowed_compositor, sao_ai_editor_launcher_t borrowed_launcher,
    sao_ai_editor_settings_panel_t* out_panel);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_show(sao_ai_editor_settings_panel_t panel);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_hide(sao_ai_editor_settings_panel_t panel);

// Hide also dismisses and fully advances any active settings dialog before
// returning, so no modal remains visible after the panel is hidden.

// Owner-thread service. Advances the modal dialog state machine, starts at
// most one background settings.describe/load/save or read-only config.load
// request, publishes completed results, and republishes dirty UI without
// blocking on named-pipe I/O.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_tick(sao_ai_editor_settings_panel_t panel);

// Retryable owner-thread teardown. After handle/lifetime pin checks, the
// compositor owner-thread preflight completes before background RPC lifecycle
// state is probed. Returns BUSY while an API/action callback or background RPC
// is in flight; the caller retains ownership and must retry. A failed handler
// rollback remains fail-closed and retryable instead of reporting the panel as
// active when one of its callbacks is detached.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_settings_panel_try_destroy(sao_ai_editor_settings_panel_t panel);

#if defined(SAO_AI_EDITOR_TESTING)
int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_dispatch_action_for_testing(
    sao_ai_editor_settings_panel_t panel, const char* action_id_utf8,
    const uint8_t* payload_json_utf8, size_t payload_len);

int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_snapshot_json_for_testing(
    sao_ai_editor_settings_panel_t panel, char* buffer_utf8, size_t buffer_cap, size_t* out_len);

int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_submit_dialog_for_testing(
    sao_ai_editor_settings_panel_t panel, const char* input_text_utf8, size_t input_text_len);

int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_set_teardown_failures_for_testing(
    sao_ai_editor_settings_panel_t panel, bool fail_unregister_once, bool fail_restore_action,
    bool fail_restore_event);

int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_get_teardown_state_for_testing(
    sao_ai_editor_settings_panel_t panel, bool* out_accepting, bool* out_action_handler_attached,
    bool* out_event_handler_attached, bool* out_teardown_failed);

void SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_test_set_owner_preflight_pause(int32_t target);

int32_t SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_test_owner_preflight_waiting_target(void);

bool SAO_AI_EDITOR_CALL sao_ai_editor_settings_panel_test_destroy_registry_probe_completed(void);
#endif

#ifdef __cplusplus
} // extern "C"
#endif
