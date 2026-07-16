#pragma once

#include "sao/launcher/rollout.h"

#include <cstdint>

namespace sao::launcher {

struct LauncherLifecycleDecision {
    sao_dual_run_config dual_config{};
    sao_launcher_dual_run_mode_t selected_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
    int64_t start_qpc = 0;
    bool active = false;
    bool child_process = false;
    bool telemetry_started = false;
};

sao_status_t prepareLauncherLifecycle(
    LauncherLifecycleDecision& decision) noexcept;

void completeLauncherLifecycle(
    LauncherLifecycleDecision& decision,
    int exit_code,
    const char* failure_hint) noexcept;

} // namespace sao::launcher
