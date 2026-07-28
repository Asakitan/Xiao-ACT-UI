// SAO Auto — SDK: sound effects (procedural + wav playback + font loading).
//
// Phase 8 (Python parity closure) — port of python/utils/sao_sound.py.

#pragma once

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sao_sdk_sound_kind_e {
    SAO_SDK_SOUND_BEEP_LEVELUP = 0,
    SAO_SDK_SOUND_BEEP_READY = 1,
    SAO_SDK_SOUND_BEEP_ALERT = 2,
    SAO_SDK_SOUND_BEEP_CONFIRM = 3,
} sao_sdk_sound_kind_t;

// 播放 procedural 8-bit 生成的音效 (matches Python play_levelup_sfx()).
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_procedural(
    const struct SaoSdkContext* ctx, sao_sdk_sound_kind_t kind);

// 播放 WAV 文件 (winsound.PlaySoundW alias).
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_play_wav(
    const struct SaoSdkContext* ctx, const char* path_utf8);

// 加载 SAO 私有字体 (AddFontResourceExW FR_PRIVATE).
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_load_font(
    const struct SaoSdkContext* ctx, const char* font_path_utf8);

// 全屏 LevelUp flash overlay (Python LevelUpEffect.show)。
SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_flash_overlay(
    const struct SaoSdkContext* ctx, uint32_t duration_ms);

#ifdef __cplusplus
}  // extern "C"
#endif
