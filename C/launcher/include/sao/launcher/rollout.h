// SAO Auto - launcher/rollout.h
//
// Telemetry-driven gradual rollout and auto-retreat system.
//
// Layered on top of dual_run.h.  Whereas dual_run picks between C++ and
// Python for a single machine based on a static config, rollout adds a
// gradual, telemetry-driven ramp:
//
//   * gray-scale percentage (cpp_percent 0..100)
//   * per-machine deterministic bucketing (HMAC-SHA-256 over anon_id)
//   * explicit user override (cpp | python | auto)
//   * telemetry feedback loop -> auto-retreat halves cpp_percent after
//     3-of-5 recent CPP starts fail.
//
// The rollout layer decides an override MODE for the process; dispatcher
// then either uses that mode directly or falls back to the dual_run mode.
//
// Contract
// --------
// * All entry points are extern "C", parameters and returns are POD.
// * State lives at %APPDATA%\SaoAuto\rollout.json (config) and
//   %APPDATA%\SaoAuto\rollout_stats.json (recent-run window).
// * Telemetry events published via
//   sao::server::freetier::telemetry_client.  When
//   telemetry_init has NOT been called the record_* calls degrade
//   silently to updating local counters only.
// * Bucket derivation is process-agnostic: same anon_id + same salt
//   yields the same 0-99 bucket everywhere.

#pragma once

#include "sao/launcher/dual_run.h"
#include "sao/launcher/init_pipeline.h"

#include <cstdint>
#include <windows.h>

extern "C" {

// ---------------------------------------------------------------------------
// Status codes for the rollout layer.  Kept in the sao_status_t space.
// ---------------------------------------------------------------------------
#define SAO_ROLLOUT_CONFIG_PARSE_FAILED   -700
#define SAO_ROLLOUT_CONFIG_WRITE_FAILED   -701
#define SAO_ROLLOUT_STATS_WRITE_FAILED    -702
#define SAO_ROLLOUT_TELEMETRY_FAILED      -703
#define SAO_ROLLOUT_INVALID_ANON_ID       -704

// ---------------------------------------------------------------------------
// User override values.  When user_override is set explicitly, it wins over
// the bucket/percent computation.  "unset" (0) means "let the bucket decide".
// ---------------------------------------------------------------------------
typedef int32_t sao_rollout_user_override_t;

#define SAO_ROLLOUT_USER_OVERRIDE_UNSET   0
#define SAO_ROLLOUT_USER_OVERRIDE_CPP     1
#define SAO_ROLLOUT_USER_OVERRIDE_PYTHON  2
#define SAO_ROLLOUT_USER_OVERRIDE_AUTO    3  // same as UNSET but sticky in JSON

// Default salt used when deriving buckets.  Callers may pass a different
// salt (e.g., experiment name) but the default is the empty string; anon
// UUIDs already have enough entropy for uniform distribution.
#define SAO_ROLLOUT_DEFAULT_SALT ""

// ---------------------------------------------------------------------------
// Retreat history entry.  Recorded whenever auto_retreat cuts cpp_percent
// because of a burst of recent failures.
// ---------------------------------------------------------------------------
#define SAO_ROLLOUT_MAX_RETREAT_HISTORY 32
#define SAO_ROLLOUT_MAX_RECENT_RESULTS  5

typedef struct sao_rollout_retreat_entry {
    int64_t  ts_ms;              // milliseconds since Unix epoch
    int32_t  old_percent;
    int32_t  new_percent;
    char     reason[64];         // "3_of_5_failed", "manual_retreat", ...
    uint64_t watermark;            // stats snapshot consumed by this retreat
} sao_rollout_retreat_entry;

typedef struct sao_rollout_config {
    int32_t schema;              // currently 1
    int32_t cpp_percent;         // 0..100
    sao_rollout_user_override_t user_override;

    int32_t                     retreat_history_count;
    sao_rollout_retreat_entry   retreat_history[SAO_ROLLOUT_MAX_RETREAT_HISTORY];
} sao_rollout_config;

// ---------------------------------------------------------------------------
// Recent-run stats.  Kept separate from the primary config so the frequent
// per-run writes don't rewrite the retreat_history array.
// ---------------------------------------------------------------------------
typedef struct sao_rollout_stats_entry {
    int64_t  ts_ms;
    int32_t  mode;               // sao_launcher_dual_run_mode_t value
    int32_t  success;            // 1 = success, 0 = failure
    int32_t  duration_ms;
    int32_t  reason_code;        // 0 for success, non-zero for failure
} sao_rollout_stats_entry;

typedef struct sao_rollout_stats {
    int32_t                  recent_count;
    sao_rollout_stats_entry  recent[SAO_ROLLOUT_MAX_RECENT_RESULTS];
    int64_t                  total_successes;
    int64_t                  total_failures;
} sao_rollout_stats;

// ---------------------------------------------------------------------------
// Config load / save
// ---------------------------------------------------------------------------

// Populate a default config (schema=1, cpp_percent=100, user_override=UNSET,
// no retreat history).
void sao_rollout_config_default(sao_rollout_config* cfg);

// Load from %APPDATA%\SaoAuto\rollout.json.  If the file is missing the
// function fills ``cfg_out`` with defaults and returns SAO_STATUS_OK.
sao_status_t sao_rollout_config_load(sao_rollout_config* cfg_out);

// Load from an explicit path.  Missing file -> defaults + SAO_STATUS_OK.
sao_status_t sao_rollout_config_load_from_path(
    const wchar_t* path, sao_rollout_config* cfg_out);

// Persist back to %APPDATA%\SaoAuto\rollout.json (creating the parent dir
// if needed).
sao_status_t sao_rollout_config_save(const sao_rollout_config* cfg);
sao_status_t sao_rollout_config_save_to_path(
    const wchar_t* path, const sao_rollout_config* cfg);

// ---------------------------------------------------------------------------
// Bucket derivation
// ---------------------------------------------------------------------------

// Compute a bucket in [0, 99] for the given anon_id and salt.  Uses
// HMAC-SHA-256(salt, anon_id) and reduces the leading 64 bits mod 100 to
// avoid modulo bias.  If ``anon_id`` is NULL/empty, returns -1.
int32_t sao_rollout_compute_bucket(const char* anon_id, const char* salt);

// Decide whether the current process should use the CPP path.  User override
// wins; otherwise bucket < cpp_percent means "use CPP".  A negative bucket
// is treated as "unknown" and the function returns 1 (default: use CPP)
// when cpp_percent >= 100, 0 otherwise.
int32_t sao_rollout_should_use_cpp(int32_t bucket,
                                    const sao_rollout_config* cfg);

// ---------------------------------------------------------------------------
// Stats + telemetry
// ---------------------------------------------------------------------------

// Load / save the recent-run stats file.
sao_status_t sao_rollout_stats_load(sao_rollout_stats* stats_out);
sao_status_t sao_rollout_stats_load_from_path(
    const wchar_t* path, sao_rollout_stats* stats_out);
sao_status_t sao_rollout_stats_save(const sao_rollout_stats* stats);
sao_status_t sao_rollout_stats_save_to_path(
    const wchar_t* path, const sao_rollout_stats* stats);

// Record a successful CPP launch (or the successful use of a mode).  Updates
// the stats window and publishes a telemetry event
// (``sao.rollout.launch_success``).
sao_status_t sao_rollout_record_success(sao_launcher_dual_run_mode_t mode,
                                         int32_t duration_ms);

// Record a failure.  ``reason_code`` is a caller-defined tag (e.g., an
// sao_status_t propagated from a subsystem).  ``stack_hint`` is a short
// (< 64 char) UTF-8 label that helps operators triage; NULL is accepted.
sao_status_t sao_rollout_record_failure(sao_launcher_dual_run_mode_t mode,
                                         int32_t reason_code,
                                         const char* stack_hint);

// Inspect the recent-run window and, if 3-of-the-last-5 CPP starts failed,
// halve ``cpp_percent`` and append a retreat_history entry.  Returns 1 if a
// retreat fired (and writes ``new_percent_out`` when non-null), 0 otherwise.
// Publishes ``sao.rollout.auto_retreat`` on retreat.
int32_t sao_rollout_check_auto_retreat(int32_t* new_percent_out);

// ---------------------------------------------------------------------------
// Dispatcher
//
// Given the caller's anon_id and the dual_run config, decide which mode the
// current process should adopt.  The rollout layer only picks between
// CPP-only and PYTHON-only for the primary path; if it can't reach a
// decision (missing anon_id, cpp_percent literally 0 with UNSET override)
// the fallback is ``dual_run.mode``.
//
// Priority ladder:
//   1. user_override != UNSET/AUTO       -> use that as CPP or PYTHON
//   2. cpp_percent == 100                -> CPP_ONLY
//   3. cpp_percent == 0                  -> PYTHON_ONLY
//   4. bucket < cpp_percent              -> CPP_ONLY
//   5. bucket >= cpp_percent             -> PYTHON_ONLY
//   6. bucket unknown (missing anon_id)  -> dual_run.mode
// ---------------------------------------------------------------------------
sao_status_t sao_rollout_dispatch(const char* anon_id,
                                   const sao_dual_run_config* dual_cfg,
                                   sao_launcher_dual_run_mode_t* out_mode);

// ---------------------------------------------------------------------------
// Test-only hooks.  Point stats / config I/O at a scratch dir, or stub
// out telemetry (which by default just no-ops when telemetry_init has
// not been called on the process).
// ---------------------------------------------------------------------------

// Redirect %APPDATA%\SaoAuto\rollout.json + rollout_stats.json to a
// caller-owned directory.  Pass NULL to restore the default.
void sao_rollout_test_set_appdata_dir(const wchar_t* dir);

// Freeze the current wall-clock time returned by internal helpers.  Pass 0
// to unfreeze.  Used by tests that need deterministic ts_ms fields.
void sao_rollout_test_set_now_ms(int64_t frozen_ms);

// Intercept telemetry publishes.  Rather than routing to the real
// telemetry_client, the hook (when set) receives (event_name, props_json)
// verbatim.  Pass NULL to restore the default (call telemetry_client).
typedef void (*sao_rollout_test_telemetry_hook_t)(
    const char* event_name, const char* props_json);

void sao_rollout_test_set_telemetry_hook(sao_rollout_test_telemetry_hook_t hook);

// Fully reset the rollout module's in-memory state (test override paths,
// telemetry hook, frozen clock).  Does NOT touch on-disk files.
void sao_rollout_reset_for_test(void);

} // extern "C"
