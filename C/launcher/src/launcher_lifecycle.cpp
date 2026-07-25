#include "launcher_lifecycle.h"

#include "sao/launcher/app.h"

#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
#include "sao/server/freetier/telemetry_client/telemetry_client.h"
#include "sao_core/sao_status.h"
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>

namespace sao::launcher {
namespace {

bool isDualRunChild() noexcept {
    wchar_t role[64]{};
    return GetEnvironmentVariableW(
               SAO_DUAL_RUN_ENV_VAR_NAME, role,
               static_cast<DWORD>(_countof(role))) > 0 &&
        role[0] != L'\0';
}

int32_t elapsedMilliseconds(int64_t started) noexcept {
    if (started == 0) return 0;
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&now) ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0;
    }
    const auto ticks = std::max<int64_t>(0, now.QuadPart - started);
    const auto milliseconds =
        ticks / frequency.QuadPart * 1000 +
        ticks % frequency.QuadPart * 1000 / frequency.QuadPart;
    return static_cast<int32_t>(
        std::min<int64_t>(milliseconds, INT32_MAX));
}

#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
bool initializeTelemetry(std::array<char, 128>& anon_id) noexcept {
    char endpoint[2048]{};
    const DWORD endpoint_length = GetEnvironmentVariableA(
        "SAO_TELEMETRY_ENDPOINT", endpoint,
        static_cast<DWORD>(_countof(endpoint)));
    const char* effective_endpoint =
        endpoint_length > 0 && endpoint_length < _countof(endpoint)
        ? endpoint
        : "";
#ifndef SAO_LAUNCHER_VERSION
#define SAO_LAUNCHER_VERSION "0.0.0"
#endif
    if (sao_telemetry_init(
            effective_endpoint, SAO_LAUNCHER_VERSION, nullptr) != SAO_OK) {
        return false;
    }
    if (sao_telemetry_get_anon_id(anon_id.data(), anon_id.size()) != SAO_OK) {
        (void)sao_telemetry_shutdown();
        anon_id[0] = '\0';
        return false;
    }
    return true;
}
#endif

} // namespace

sao_status_t prepareLauncherLifecycle(
    LauncherLifecycleDecision& decision,
    bool force_cpp_only) noexcept {
    decision = {};
    if (force_cpp_only) {
        sao_launcher_dual_run_config_default(&decision.dual_config);
        decision.selected_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
        decision.dual_config.mode = decision.selected_mode;
        return SAO_STATUS_OK;
    }

    (void)sao_launcher_dual_run_config_load(&decision.dual_config);
    LARGE_INTEGER started{};
    if (QueryPerformanceCounter(&started)) {
        decision.start_qpc = started.QuadPart;
    }

    decision.child_process = isDualRunChild();
    if (decision.child_process) {
        decision.selected_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
        decision.dual_config.mode = decision.selected_mode;
        return SAO_STATUS_OK;
    }

    std::array<char, 128> anon_id{};
#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
    decision.telemetry_started = initializeTelemetry(anon_id);
#endif

    decision.selected_mode = decision.dual_config.mode;
    const sao_status_t status = sao_rollout_dispatch(
        anon_id[0] ? anon_id.data() : nullptr,
        &decision.dual_config, &decision.selected_mode);
    if (status != SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
        if (decision.telemetry_started) {
            (void)sao_telemetry_shutdown();
            decision.telemetry_started = false;
        }
#endif
        return status;
    }

    decision.dual_config.mode = decision.selected_mode;
    decision.active = true;
    return SAO_STATUS_OK;
}

void completeLauncherLifecycle(
    LauncherLifecycleDecision& decision,
    int exit_code,
    const char* failure_hint) noexcept {
    if (decision.active) {
        if (exit_code == SAO_EXIT_OK ||
            exit_code == SAO_EXIT_HANDOFF_TO_PYTHON) {
            (void)sao_rollout_record_success(
                decision.selected_mode,
                elapsedMilliseconds(decision.start_qpc));
        } else if (exit_code != SAO_EXIT_ALREADY_RUNNING &&
                   exit_code != SAO_EXIT_BAD_ARGS) {
            (void)sao_rollout_record_failure(
                decision.selected_mode, exit_code,
                failure_hint ? failure_hint : "launcher");
        }
        (void)sao_rollout_check_auto_retreat(nullptr);
        decision.active = false;
    }

#if defined(SAO_LAUNCHER_HAS_TELEMETRY_CLIENT)
    if (decision.telemetry_started) {
        (void)sao_telemetry_shutdown();
        decision.telemetry_started = false;
    }
#endif
}

} // namespace sao::launcher
