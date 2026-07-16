// SAO Auto — launcher/shutdown.h
//
// Reverse-order teardown.  Called by App::shutdown().  Each function is
// individually safe: calling it after its subsystem already teardown'd
// (or was never brought up) is a no-op.

#pragma once

namespace sao::launcher {

struct AppState;

void takeUiOffline(AppState& state) noexcept;
bool shutdownPlugins(AppState& state) noexcept;
bool tearDownPlatform(AppState& state) noexcept;
bool shutdownSecurity(bool initialized) noexcept;
bool shutdownShell(AppState& state) noexcept;
bool shutdownLicense(AppState& state) noexcept;
void releaseSingleInstanceMutex(AppState& state) noexcept;

// Convenience: run every step above in reverse-init order.  App::shutdown
// calls this.
bool runFullShutdown(AppState& state, bool security_initialized) noexcept;

} // namespace sao::launcher
