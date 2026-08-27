// SAO Auto — game-agnostic alerts / TTS / banner / sound bridge.
//
// This module owns:
//   * TTS via Windows SAPI (SPVoice) — an in-process voice used by
//     boss-timer callouts, dodge cues, whatever the caller wants.
//   * Banner queue — text banners with a duration + color hint, drained
//     by the UI compositor for on-screen display.  This module is only
//     the queue back-end; the compositor is the consumer.
//   * Sound playback via winmm PlaySoundW — fire-and-forget WAV / SND
//     resource play with an optional stop token.
//
// Everything here is game-agnostic: the caller supplies the text /
// path / color; no boss names, no skill IDs.  The game-specific glue
// lives in the plugin (e.g. bossraid_mechanics_system).
//
// ── SAPI initialisation ────────────────────────────────────────
//   SAPI requires COM.  We do a `CoInitializeEx(COINIT_MULTITHREADED)`
//   lazily on first speak() call so callers who never speak don't pay
//   the COM startup cost.  The SPVoice object is a per-module singleton
//   guarded by a mutex; parallel speak() calls serialise on the mutex
//   (SAPI itself is single-threaded).
//
// ── Banner queue ownership ─────────────────────────────────────
//   The queue lives here (thread-safe deque).  Producers call
//   banner_show and receive a `banner_id`.  Consumers (the UI
//   compositor) drain via `banner_pop_pending` on the render tick.
//   `banner_hide(id)` marks the entry cancelled so a consumer that
//   still has it can skip rendering.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── TTS ─────────────────────────────────────────────────────────

// Speak the UTF-16 text via SAPI SPVoice.  `voice_id` is a UTF-8
// token that matches a SAPI voice's `Name`; NULL / empty selects the
// system default.  `rate` in [-10, 10] (SPVoice::SetRate range).
// `volume` in [0, 100] (SPVoice::SetVolume range).  The call is
// non-blocking (SPVoice::Speak with SPF_ASYNC).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_speak(
    const uint16_t* text_utf16,
    const char*     voice_id_utf8,
    int32_t         rate,
    int32_t         volume);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_stop(void);

// Enumerate the installed SAPI voices.  When `capacity==0` writes the
// required count into `out_count` and returns SAO_STATUS_OK (query
// mode).  Otherwise writes UTF-8 voice names into `out_names` (each
// null-terminated) up to `capacity`.  `name_stride_bytes` is the
// per-slot buffer size (recommend 128).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_enum_voices(
    char*   out_names,
    size_t  capacity,
    size_t  name_stride_bytes,
    size_t* out_count);

// ── Banner ──────────────────────────────────────────────────────

// Show a banner.  `color_id` is an opaque theme token consumed by the
// compositor (0 → default).  Returns a positive `banner_id` on success.
// The banner is queued for the compositor to drain on its next tick.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_show(
    const uint16_t* text_utf16,
    uint32_t        duration_ms,
    int32_t         color_id,
    uint64_t*       out_banner_id);

// Hide a banner previously scheduled with `banner_show`.  Idempotent —
// hiding an already-hidden or unknown id returns SAO_STATUS_OK.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_hide(
    uint64_t banner_id);

// Compositor-facing drain.  Copies up to `capacity` pending banner
// records into `out_records`; writes the copy count into `out_count`.
// Passing a null / zero-capacity buffer returns the pending count in
// `out_count` (query mode).
struct SaoUiAlertBanner {
    uint64_t banner_id;
    // Duration remaining in milliseconds when the record was drained.
    // Consumer decrements per-frame; when it reaches 0 the banner is
    // considered expired.
    int32_t  duration_ms;
    int32_t  color_id;
    // UTF-16 payload — copy into a stable location if the consumer
    // needs to retain past the current tick.  `text_utf16` is a
    // pointer into internally managed storage that becomes invalid
    // after `banner_hide` on the same id.
    const uint16_t* text_utf16;
    size_t          text_len;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_banner_drain(
    SaoUiAlertBanner* out_records,
    size_t            capacity,
    size_t*           out_count);

// ── Sound ───────────────────────────────────────────────────────

// Play a WAV file at `path_utf16` asynchronously via winmm
// PlaySoundW(SND_ASYNC | SND_FILENAME).  `volume` is 0..100 mapped to
// waveOutSetVolume on the default device.  Returns a positive sound_id
// on success; SAO_STATUS_ERR_NOT_FOUND when the file does not exist.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_sound_play(
    const uint16_t* path_utf16,
    int32_t         volume,
    uint64_t*       out_sound_id);

// Stop a previously-started sound.  Passing 0 stops every currently-
// playing sound (PlaySoundW(NULL, NULL, 0)).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_alerts_sound_stop(
    uint64_t sound_id);

#ifdef __cplusplus
}  // extern "C"
#endif
