// SAO Auto — launcher/shutdown.cpp
//
// Every function below is individually safe to call on an uninitialised
// subsystem — the corresponding init_pipeline stub reports SAO_STATUS_OK
// on shutdown when nothing was ever brought up.

#include "sao/launcher/shutdown.h"
#include "sao/launcher/app.h"
#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/single_instance.h"

#include <windows.h>

#include <cstdio>

namespace sao::launcher {

namespace {

void traceShutdownFailure(const char* stage, int attempt) noexcept {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"SAO_LAUNCHER_STARTUP_DIAGNOSTICS", enabled, 2u) == 0u) {
        return;
    }
    char line[192]{};
    const int length = sprintf_s(line, sizeof(line), "SAO_SHUTDOWN stage=%s attempt=%d\r\n",
                                 stage, attempt);
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE || length <= 0)
        return;
    DWORD written = 0u;
    (void)WriteFile(output, line, static_cast<DWORD>(length), &written, nullptr);
}

} // namespace

bool takeUiOffline(AppState& state) noexcept {
    if (!state.ui_online) return true;
    if (state.platform_ctx == nullptr) return false;
    if (sao_ui_take_offline(static_cast<sao_platform_ctx*>(state.platform_ctx)) !=
        SAO_STATUS_OK) {
        return false;
    }
    state.ui_online = false;
    return true;
}

bool shutdownPlugins(AppState& state) noexcept {
    if (state.plugins_registry) {
        if (state.platform_ctx != nullptr &&
            sao_platform_bind_plugins(static_cast<sao_platform_ctx*>(state.platform_ctx),
                                      nullptr) != SAO_STATUS_OK) {
            return false;
        }
        const sao_status_t status =
            sao_plugins_shutdown(static_cast<sao_plugins_registry*>(state.plugins_registry));
        if (status != SAO_STATUS_OK) {
            return false;
        }
        state.plugins_registry = nullptr;
    }
    return true;
}

bool tearDownPlatform(AppState& state) noexcept {
    if (state.platform_ctx) {
        if (sao_platform_teardown(static_cast<sao_platform_ctx*>(state.platform_ctx)) ==
            SAO_STATUS_OK) {
            state.platform_ctx = nullptr;
        } else {
            return false;
        }
    }
    return true;
}

bool shutdownSecurity(bool& initialized) noexcept {
    if (!initialized)
        return true;
    if (sao_security_shutdown() != SAO_STATUS_OK)
        return false;
    initialized = false;
    return true;
}

void serviceShutdownRetry(AppState& state) noexcept {
    if (state.platform_ctx != nullptr) {
        if (state.ui_online &&
            sao_ui_take_offline(static_cast<sao_platform_ctx*>(state.platform_ctx)) ==
                SAO_STATUS_OK) {
            state.ui_online = false;
        }
        (void)sao_ui_tick(static_cast<sao_platform_ctx*>(state.platform_ctx), 1u);
    }
    Sleep(2u);
}

bool shutdownShell(AppState& state) noexcept {
    if (state.shell_active) {
        if (sao_shell_shutdown() != SAO_STATUS_OK)
            return false;
        state.shell_active = false;
    }
    return true;
}

bool shutdownLicense(AppState& state) noexcept {
    if (state.license_active) {
        if (sao_license_shutdown() != SAO_STATUS_OK)
            return false;
        state.license_active = false;
    }
    return true;
}

void releaseOwnedSingleInstanceMutex(HANDLE& mutex) noexcept {
    if (mutex != nullptr) {
        releaseSingleInstance(mutex);
        mutex = nullptr;
    }
}

bool runFullShutdown(AppState& state, bool& security_initialized) noexcept {
    constexpr int kMaximumAttempts = 3;
    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        const char* failed_stage = nullptr;
        if (!takeUiOffline(state))
            failed_stage = "ui";
        else if (!shutdownPlugins(state))
            failed_stage = "plugins";
        else if (!tearDownPlatform(state))
            failed_stage = "platform";
        else if (!shutdownSecurity(security_initialized))
            failed_stage = "security";
        else if (!shutdownShell(state))
            failed_stage = "shell";
        else if (!shutdownLicense(state))
            failed_stage = "license";
        if (failed_stage == nullptr) {
            return true;
        }
        traceShutdownFailure(failed_stage, attempt + 1);
        if (attempt + 1 < kMaximumAttempts)
            serviceShutdownRetry(state);
    }
    return false;
}

} // namespace sao::launcher
