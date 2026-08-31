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
    SAO_UI_SOUND_COUNT = 11,
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_play(SaoUiSoundCue cue, int32_t requested_volume);
// Idempotent teardown. A later play lazily starts a fresh engine.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_shutdown(void);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_set_enabled(bool enabled);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_get_enabled(bool* out_enabled);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_set_volume(int32_t volume);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_sound_get_volume(int32_t* out_volume);

#ifdef __cplusplus
} // extern "C"
#endif
