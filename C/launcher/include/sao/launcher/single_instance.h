// SAO Auto — launcher/single_instance.h
//
// Uses a named mutex "Global\SaoAuto.Instance.<hash-of-exe-path>" to prevent
// two concurrent launches from stomping on each other.  When a second
// instance detects the mutex is already held, it forwards its command line
// to the running process via a WM_COPYDATA broadcast on the well-known
// SaoAuto message window class, then exits SAO_EXIT_ALREADY_RUNNING.

#pragma once

#include <windows.h>

namespace sao::launcher {

// Try to acquire the single-instance mutex.  On success, `mutex_out`
// receives a handle the caller must keep alive for the lifetime of the
// process.
//
// Returns:
//   true  — acquired, mutex_out valid
//   false — another instance already running, mutex_out == nullptr
bool acquireSingleInstance(HANDLE& mutex_out) noexcept;

// Release the mutex.  Called by App::shutdown().
void releaseSingleInstance(HANDLE mutex) noexcept;

// When acquireSingleInstance() reports another instance, this forwards our
// command line to it (so double-clicking the icon focuses the running
// window, and a --file=X argument opens that file in the existing
// instance).  Best-effort; failure is silent.
void forwardCommandLineToRunningInstance(const wchar_t* cmdline) noexcept;

} // namespace sao::launcher
