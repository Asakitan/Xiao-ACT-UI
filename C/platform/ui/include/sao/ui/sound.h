// SAO Auto — native named sound effects.
//
// Cue names and default settings mirror python/utils/sao_sound.py. The
// playback volume is min(requested_volume, global_volume), matching the
// Python sound manager.

#pragma once

#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

enum SaoUiSoundCue : int32_t {
    SAO_UI_SOUND_CLICK = 0,
    SAO_UI_SOUND_MENU_OPEN = 1,
    SAO_UI_SOUND_MENU_CLOSE = 2,
    SAO_UI_SOUND_PANEL = 3,
    SAO_UI_SOUND_SUBMENU = 4,
    SAO_UI_SOUND_ALERT = 5,
    SAO_UI_SOUND_ALERT_CLOSE = 6,
    SAO_UI_SOUND_WELCOME = 7,
    SAO_UI_SOUND_ALO_WELCOME = 8,
    SAO_UI_SOUND_LINK_START = 9,
    SAO_UI_SOUND_NERVEGEAR = 10,
    // Appended in ABI minor 11.  Existing cue numbers are frozen.
    SAO_UI_SOUND_MESSAGE = 11,
    SAO_UI_SOUND_SYSTEM = 12,
    SAO_UI_SOUND_WARNING = 13,
    SAO_UI_SOUND_EMERGENCY = 14,
    SAO_UI_SOUND_COUNT = 15,
};

// Semantic calls let a producer express confirmation/cancellation without
// depending on a visual surface's concrete cue choice.
enum SaoUiSoundSemantic : int32_t {
    SAO_UI_SOUND_SEMANTIC_CONFIRM = 0,
    SAO_UI_SOUND_SEMANTIC_CANCEL = 1,
};

// A group owns a bounded interaction or timeline.  Stopping it silences only
// voices submitted through that group, leaving unrelated UI feedback intact.
typedef uint64_t sao_ui_sound_group_t;

// An event scope retains only its highest-priority offered cue until commit.
// While a scope is current on a thread, ordinary sao_ui_sound_play calls are
// offered automatically; nested commits merge into their parent scope.
typedef uint64_t sao_ui_sound_event_scope_t;
enum SaoUiSoundPriority : int32_t {
    SAO_UI_SOUND_PRIORITY_FEEDBACK = 0,
    SAO_UI_SOUND_PRIORITY_NAVIGATION = 1,
    SAO_UI_SOUND_PRIORITY_PANEL = 2,
    SAO_UI_SOUND_PRIORITY_ALERT = 3,
    SAO_UI_SOUND_PRIORITY_EMERGENCY = 4,
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_play(SaoUiSoundCue cue, int32_t requested_volume);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_play_in_group(SaoUiSoundCue cue,
                                                               int32_t requested_volume,
                                                               sao_ui_sound_group_t group);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_play_semantic(SaoUiSoundSemantic semantic,
                                                               int32_t requested_volume,
                                                               sao_ui_sound_group_t group);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_play_wav_utf8(const char* path_utf8,
                                                               int32_t requested_volume,
                                                               sao_ui_sound_group_t group);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_play_wav_utf16(const uint16_t* path_utf16,
                                                                int32_t requested_volume,
                                                                sao_ui_sound_group_t group);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_group_create(sao_ui_sound_group_t* out_group);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_group_stop(sao_ui_sound_group_t group);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_group_destroy(sao_ui_sound_group_t group);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_event_begin(sao_ui_sound_group_t group,
                                                             sao_ui_sound_event_scope_t* out_scope);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_event_offer(sao_ui_sound_event_scope_t scope,
                                                             SaoUiSoundCue cue,
                                                             int32_t requested_volume,
                                                             SaoUiSoundPriority priority);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_event_commit(sao_ui_sound_event_scope_t scope);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_event_cancel(sao_ui_sound_event_scope_t scope);
// Idempotent teardown. A later play lazily starts a fresh engine.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_shutdown(void);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_set_enabled(bool enabled);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_get_enabled(bool* out_enabled);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_set_volume(int32_t volume);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_get_volume(int32_t* out_volume);

#ifdef __cplusplus
} // extern "C"
#endif
