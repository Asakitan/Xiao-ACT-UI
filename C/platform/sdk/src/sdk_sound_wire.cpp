// sdk_sound_wire.cpp — procedural beep + wav playback + font loading.
//
// Phase 8 (Python parity closure) — port of python/utils/sao_sound.py.
//   procedural LevelUp = 4-harmonic square + ADSR envelope, ~380ms.
//   wav play           = PlaySoundW (SND_ASYNC|SND_FILENAME).
//   font load          = AddFontResourceExW(FR_PRIVATE).
//   flash overlay      = routed to platform/ui/src/level_up_effect_overlay.cpp.

#include "sao/sdk/sao_sdk_sound.h"
#include "sao/ui/abi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace {

constexpr uint32_t kSampleRate = 44100;

std::vector<int16_t> synthesize_levelup() {
    // Envelope: A=5ms D=30ms S=100ms R=250ms.
    const uint32_t attack_n = 5 * kSampleRate / 1000;
    const uint32_t decay_n = 30 * kSampleRate / 1000;
    const uint32_t sustain_n = 100 * kSampleRate / 1000;
    const uint32_t release_n = 250 * kSampleRate / 1000;
    const uint32_t total = attack_n + decay_n + sustain_n + release_n;
    std::vector<int16_t> out(total);
    const double harmonics[] = {440.0, 660.0, 880.0, 1320.0};
    for (uint32_t i = 0; i < total; ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        double amp = 0.0;
        for (double f : harmonics) {
            // square wave: sign(sin)
            amp += (std::sin(2.0 * 3.14159265358979323846 * f * t) >= 0 ? 1.0 : -1.0);
        }
        amp /= 4.0;
        // envelope
        double env = 0.0;
        if (i < attack_n) env = static_cast<double>(i) / attack_n;
        else if (i < attack_n + decay_n) {
            double x = static_cast<double>(i - attack_n) / decay_n;
            env = 1.0 - 0.4 * x; // fade to 0.6 sustain
        } else if (i < attack_n + decay_n + sustain_n) {
            env = 0.6;
        } else {
            double x = static_cast<double>(i - attack_n - decay_n - sustain_n) / release_n;
            env = 0.6 * (1.0 - x);
        }
        double sample = amp * env * 0.5;
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        out[i] = static_cast<int16_t>(sample * 32767.0);
    }
    return out;
}

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

// Write a mono 16-bit PCM RIFF header + samples to a temp .wav file, then play.
bool play_pcm_samples(const std::vector<int16_t>& samples) {
    wchar_t tmp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp_dir) == 0) return false;
    wchar_t tmp_file[MAX_PATH];
    if (GetTempFileNameW(tmp_dir, L"sao_snd_", 0, tmp_file) == 0) return false;
    // Overwrite tmp_file with a .wav.
    HANDLE h = CreateFileW(tmp_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    uint8_t header[44] = {};
    std::memcpy(header + 0, "RIFF", 4);
    uint32_t chunk_size = 36 + data_bytes;
    std::memcpy(header + 4, &chunk_size, 4);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    uint32_t sub1 = 16;
    std::memcpy(header + 16, &sub1, 4);
    uint16_t audio_fmt = 1;
    std::memcpy(header + 20, &audio_fmt, 2);
    uint16_t channels = 1;
    std::memcpy(header + 22, &channels, 2);
    uint32_t rate = kSampleRate;
    std::memcpy(header + 24, &rate, 4);
    uint32_t byte_rate = kSampleRate * 2;
    std::memcpy(header + 28, &byte_rate, 4);
    uint16_t block_align = 2;
    std::memcpy(header + 32, &block_align, 2);
    uint16_t bps = 16;
    std::memcpy(header + 34, &bps, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &data_bytes, 4);
    DWORD written = 0;
    WriteFile(h, header, sizeof(header), &written, nullptr);
    WriteFile(h, samples.data(), data_bytes, &written, nullptr);
    CloseHandle(h);
    return PlaySoundW(tmp_file, nullptr, SND_ASYNC | SND_FILENAME) != FALSE;
}
#endif

} // namespace

extern "C" sao_sdk_status_t SAO_SDK_CALL sao_sdk_sound_procedural(
    const SaoSdkContext* /*ctx*/, sao_sdk_sound_kind_t kind) {
    try {
#if defined(_WIN32)
        if (kind == SAO_SDK_SOUND_BEEP_LEVELUP) {
            auto samples = synthesize_levelup();
            return play_pcm_samples(samples) ? SAO_SDK_OK : SAO_SDK_ERR_UNSUPPORTED;
        }
        Beep(1200, 120);
        return SAO_SDK_OK;
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
        std::wstring wide;
        if (!bounded_utf8_to_wide(path_utf8, &wide))
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return PlaySoundW(wide.c_str(), nullptr, SND_ASYNC | SND_FILENAME)
                   ? SAO_SDK_OK : SAO_SDK_ERR_UNSUPPORTED;
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