// SAO Auto — launcher/shutdown.cpp
//
// Every function below is individually safe to call on an uninitialised
// subsystem — the corresponding init_pipeline stub reports SAO_STATUS_OK
// on shutdown when nothing was ever brought up.

#include "sao/launcher/shutdown.h"
#include "sao/launcher/app.h"
#include "sao/launcher/single_instance.h"
#include "sao/launcher/init_pipeline.h"

namespace sao::launcher {

void takeUiOffline(AppState& state) noexcept {
    if (state.platform_ctx) {
        (void)sao_ui_take_offline(static_cast<sao_platform_ctx*>(state.platform_ctx));
    }
}

bool shutdownPlugins(AppState& state) noexcept {
    if (state.plugins_registry) {
        const sao_status_t status = sao_plugins_shutdown(
            static_cast<sao_plugins_registry*>(state.plugins_registry));
        if (status != SAO_STATUS_OK) {
            return false;
        }
        state.plugins_registry = nullptr;
    }
    return true;
}

bool tearDownPlatform(AppState& state) noexcept {
    if (state.platform_ctx) {
        if (sao_platform_teardown(
                static_cast<sao_platform_ctx*>(state.platform_ctx)) ==
            SAO_STATUS_OK) {
            state.platform_ctx = nullptr;
        } else {
            return false;
        }
    }
    return true;
}

bool shutdownSecurity(bool initialized) noexcept {
    if (initialized && sao_security_shutdown() != SAO_STATUS_OK) {
        return false;
    }
    return true;
}

bool shutdownShell(AppState& state) noexcept {
    if (state.shell_active) {
        if (sao_shell_shutdown() != SAO_STATUS_OK) return false;
        state.shell_active = false;
    }
    return true;
}

bool shutdownLicense(AppState& state) noexcept {
    if (state.license_active) {
        if (sao_license_shutdown() != SAO_STATUS_OK) return false;
        state.license_active = false;
    }
    return true;
}

void releaseSingleInstanceMutex(AppState& /*state*/) noexcept {
    // The mutex handle is owned by App itself, not AppState — this hook
    // exists so the pipeline layout stays symmetric.  Nothing to do here.
}

bool runFullShutdown(AppState& state, bool security_initialized) noexcept {
    takeUiOffline(state);
    if (!shutdownPlugins(state)) {
        return false;
    }
    if (!tearDownPlatform(state)) {
        return false;
    }
    if (!shutdownSecurity(security_initialized)) {
        return false;
    }
    if (!shutdownShell(state)) {
        return false;
    }
    if (!shutdownLicense(state)) {
        return false;
    }
    releaseSingleInstanceMutex(state);
    return true;
}

} // namespace sao::launcher
