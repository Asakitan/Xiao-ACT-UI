// SAO Auto — display-synced frame pacer.
//
// Python authoritative source: `sao_auto/python/render/overlay_scheduler.py` (395 lines)
//
// One `after`-loop-equivalent tick at the monitor's refresh rate
// (auto-detected on Windows via GetDeviceCaps(VREFRESH); clamps
// 60-240 Hz).  Uses a high-resolution perf-counter deadline so the
// cadence does not drift.
//
// ── timeBeginPeriod(1) requirement ──────────────────────────
//   Without `winmm.timeBeginPeriod(1)`, Windows' scheduler quantum
//   is ~15.6 ms and any `Sleep(1)` / `WaitForSingleObject(1)` rounds
//   up to that.  Overlays then degrade to ~64 Hz best-case and
//   ~10-20 Hz under load.  Engaged while the scheduler is running;
//   released on stop() so the process doesn't leave the system-wide
//   timer pinned high.
//
// ── Pressure levels ────────────────────────────────────────
//   Idle panels (non-animating) throttle based on measured wall-time
//   pressure (`peak_recent_worker_wall_ms`).  Mapping (60 Hz frame
//   budget = 16.7 ms):
//     peak <= 12 ms  → floor 0  (everything fits)
//     peak <= 25 ms  → floor 1  (one panel pushed past budget)
//     peak <= 50 ms  → floor 2  (sustained 30 fps composes)
//     peak  > 50 ms  → floor 3  (heavy burst + combined load)
//   Animating panels always tick every frame regardless of pressure.
//
// ── Combat + menu pressure signals ─────────────────────────
//   `set_combat_load(true)` / `set_menu_open(true)` inject explicit
//   pressure levels (combat=1, menu=2).  These originate from panel
//   observers and let idle throttle react instantly instead of
//   waiting for the wall-time poll to catch up.
//
// ── Phase offset (stable) ──────────────────────────────────
//   Each job gets a deterministic phase offset from its ident string
//   (stable_phase_offset).  When idle_skip_n > 1, throttled ticks
//   stagger across the ident space instead of firing all at once
//   (which would spike the render lane pool).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_scheduler_s* sao_ui_scheduler_handle_t;

struct SaoSchedulerConfig {
    // 0 → auto-detect via GetDeviceCaps(VREFRESH), clamped 60-240 Hz.
    // Non-zero overrides (test-only).
    int32_t     target_hz;

    // Cap on idle skip-n even under extreme pressure.  Default 4
    // (~15 Hz idle); pass 0 for library default.
    int32_t     max_idle_skip_n;

    // Enable the winmm.timeBeginPeriod(1) engage.  True by default;
    // pass false to skip (for tests or when the caller already engaged
    // period reduction).
    bool        engage_time_period;

    // Enable pressure-floor wall-time polling.  True by default.
    bool        enable_pressure_floor;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_create(
    const SaoSchedulerConfig* config,
    sao_ui_scheduler_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_scheduler_destroy(
    sao_ui_scheduler_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_start(
    sao_ui_scheduler_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_stop(
    sao_ui_scheduler_handle_t handle);

// Register a tick job.
// `tick_fn`      — called on the scheduler thread every N frames per
//                  pressure calculation.
// `animating_fn` — polled each frame to decide if this job should
//                  always tick regardless of pressure.
// `visibility_fn`— optional; skips tick when returns false (avoids
//                  work for hidden panels).  NULL → always visible.
// ident must be unique across the scheduler.
typedef void (SAO_UI_CALL* sao_ui_scheduler_tick_fn_t)(
    double now_sec, void* user_data);
typedef bool (SAO_UI_CALL* sao_ui_scheduler_animating_fn_t)(void* user_data);
typedef bool (SAO_UI_CALL* sao_ui_scheduler_visibility_fn_t)(void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_register(
    sao_ui_scheduler_handle_t handle,
    const char* ident_utf8,
    sao_ui_scheduler_tick_fn_t tick_fn,
    sao_ui_scheduler_animating_fn_t animating_fn,
    sao_ui_scheduler_visibility_fn_t visibility_fn,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_unregister(
    sao_ui_scheduler_handle_t handle,
    const char* ident_utf8);

// Pressure signals — expected to flip on/off based on panel observers.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_set_combat_load(
    sao_ui_scheduler_handle_t handle, bool active);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_set_menu_open(
    sao_ui_scheduler_handle_t handle, bool active);

// Explicit override — 0/1/2/3 for none/combat/menu/both.  -1 → auto.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_set_render_pressure(
    sao_ui_scheduler_handle_t handle, int32_t level);

// Read stats.
struct SaoSchedulerStats {
    int32_t   target_hz;
    int32_t   current_idle_skip_n;
    int32_t   wall_pressure_floor;
    uint32_t  job_count;
    double    last_frame_ms;
    double    avg_frame_ms;
    uint64_t  frame_count;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_scheduler_get_stats(
    sao_ui_scheduler_handle_t handle,
    SaoSchedulerStats* out_stats);

// Detect refresh rate directly (helper without owning a scheduler).
// Returns Hz clamped to 60-240; 60 on failure.
SAO_UI_API int32_t SAO_UI_CALL sao_ui_scheduler_detect_refresh_hz(void);

#ifdef __cplusplus
}  // extern "C"
#endif
