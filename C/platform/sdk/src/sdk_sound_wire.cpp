// sdk_sound_wire.cpp — SDK sound semantic wire + font loading.
//
// Phase 8 (Python parity closure) — port of python/utils/sao_sound.py.
//   all playback       = platform/ui XAudio2 mixer and its global policy.
//   font load          = AddFontResourceExW(FR_PRIVATE).
//   flash overlay      = routed to platform/ui/src/level_up_effect_overlay.cpp.

#define SAO_SDK_BUILDING_DLL
#include "sao/sdk/sao_sdk_sound.h"
#include "sao/ui/abi.h"
#include "sao/ui/sound.h"

#include <cstdint>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
constexpr size_t kMaximumUtf8PathBytes = 64u * 1024u;

bool bounded_utf8_to_wide(const char* value, std::wstring* out) {
    if (out == nullptr || value == nullptr) return false;
    size_t byte_length = 0u;
    for (; byte_length < kMaximumUtf8PathBytes; ++byte_length) {
        if (value[byte_length] == '\0') break;
    }
    if (byte_length == 0u || byte_length == kMaximumUtf8PathBytes ||
        byte_length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int wide_length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(byte_length),
        nullptr, 0);
    if (wide_length <= 0) return false;
    out->assign(static_cast<size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(byte_length),
            out->data(), wide_length) != wide_length) {
        out->clear();
        return false;
    }
    return true;
}

#endif

} // namespace

extern "C" sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_procedural(
    const SaoSdkContext* /*ctx*/, sao_sdk_sound_kind_t kind) {
    try {
#if defined(_WIN32)
        SaoUiSoundCue cue = SAO_UI_SOUND_SYSTEM;
        if (kind == SAO_SDK_SOUND_BEEP_READY || kind == SAO_SDK_SOUND_BEEP_CONFIRM)
            cue = SAO_UI_SOUND_CLICK;
        else if (kind == SAO_SDK_SOUND_BEEP_ALERT)
            cue = SAO_UI_SOUND_WARNING;
        else if (kind != SAO_SDK_SOUND_BEEP_LEVELUP)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return sao_ui_sound_play(cue, 70) == SAO_STATUS_OK ? SAO_SDK_OK : SAO_SDK_ERR_UNSUPPORTED;
#else
        (void)kind;
        return SAO_SDK_ERR_UNSUPPORTED;
#endif
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_play_wav(
    const SaoSdkContext* /*ctx*/, const char* path_utf8) {
    try {
#if defined(_WIN32)
        if (path_utf8 == nullptr || path_utf8[0] == '\0')
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return sao_ui_sound_play_wav_utf8(path_utf8, 70, 0) == SAO_STATUS_OK
                   ? SAO_SDK_OK
                   : SAO_SDK_ERR_UNSUPPORTED;
#else
        (void)path_utf8;
        return SAO_SDK_ERR_UNSUPPORTED;
#endif
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_load_font(
    const SaoSdkContext* /*ctx*/, const char* font_path_utf8) {
    try {
#if defined(_WIN32)
        std::wstring wide;
        if (!bounded_utf8_to_wide(font_path_utf8, &wide))
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        int added = AddFontResourceExW(wide.c_str(), FR_PRIVATE, nullptr);
        return added > 0 ? SAO_SDK_OK : SAO_SDK_ERR_UNSUPPORTED;
#else
        (void)font_path_utf8;
        return SAO_SDK_ERR_UNSUPPORTED;
#endif
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

// Forward — impl in platform/ui/src/level_up_effect_overlay.cpp.
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_level_up_effect_overlay_flash(uint32_t duration_ms);

extern "C" sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_flash_overlay(
    const SaoSdkContext* /*ctx*/, uint32_t duration_ms) {
    // Route to the overlay renderer. If the launcher hasn't bound a
    // compositor via sao_ui_level_up_effect_overlay_set_compositor yet, the
    // call is a silent no-op (safe on plugin load pre-UI).
    try {
        sao_ui_level_up_effect_overlay_flash(duration_ms);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}
