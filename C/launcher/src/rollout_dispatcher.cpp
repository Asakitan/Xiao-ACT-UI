// SAO Auto - launcher/rollout_dispatcher.cpp
//
// Rollout policy dispatcher.
//
// Sits between the launcher's init pipeline and dual_run.cpp.  Given the
// anon_id + the dual_run config, decides which mode the current process
// should adopt.
//
// Priority ladder:
//   1. rollout.user_override != UNSET/AUTO -> use that as CPP or PYTHON
//   2. rollout.cpp_percent == 100          -> CPP_ONLY
//   3. rollout.cpp_percent == 0            -> PYTHON_ONLY
//   4. bucket < rollout.cpp_percent        -> CPP_ONLY
//   5. bucket >= rollout.cpp_percent       -> PYTHON_ONLY
//   6. bucket unknown (no anon_id)         -> dual_run.mode

#include "sao/launcher/rollout.h"

#include <cstring>
#include <string>

extern "C" sao_status_t sao_rollout_dispatch(
    const char* anon_id,
    const sao_dual_run_config* dual_cfg,
    sao_launcher_dual_run_mode_t* out_mode) {

    if (!out_mode) return SAO_STATUS_INVALID_ARGUMENT;
    if (!dual_cfg) return SAO_STATUS_INVALID_ARGUMENT;

    // Start with the dual_run fallback so callers see a safe default even
    // if rollout config load fails.
    *out_mode = dual_cfg->mode;

    sao_rollout_config cfg{};
    sao_status_t rc = sao_rollout_config_load(&cfg);
    if (rc != SAO_STATUS_OK) {
        // Fall through: keep the dual_run.mode fallback.
        return SAO_STATUS_OK;
    }

    // 1) Explicit user override wins immediately.
    if (cfg.user_override == SAO_ROLLOUT_USER_OVERRIDE_CPP) {
        *out_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
        return SAO_STATUS_OK;
    }
    if (cfg.user_override == SAO_ROLLOUT_USER_OVERRIDE_PYTHON) {
        *out_mode = SAO_DUAL_RUN_MODE_PYTHON_ONLY;
        return SAO_STATUS_OK;
    }

    // 2) & 3) - percent extremes short-circuit before bucket computation.
    if (cfg.cpp_percent >= 100) {
        *out_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
        return SAO_STATUS_OK;
    }
    if (cfg.cpp_percent <= 0) {
        *out_mode = SAO_DUAL_RUN_MODE_PYTHON_ONLY;
        return SAO_STATUS_OK;
    }

    // 4) & 5) - bucket-driven.
    int32_t bucket = sao_rollout_compute_bucket(anon_id, SAO_ROLLOUT_DEFAULT_SALT);
    if (bucket < 0) {
        // 6) Bucket unknown - fall back to dual_run.mode.  Leave *out_mode
        // unchanged from the initial assignment above.
        return SAO_STATUS_OK;
    }

    if (bucket < cfg.cpp_percent) {
        *out_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
    } else {
        *out_mode = SAO_DUAL_RUN_MODE_PYTHON_ONLY;
    }
    return SAO_STATUS_OK;
}
