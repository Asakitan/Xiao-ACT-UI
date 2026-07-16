// SAO Auto — launcher/args.h
//
// Minimal command-line parser.  We deliberately avoid pulling in cli11
// (part of the tools/) here — the launcher must have zero third-party
// deps that could delay startup or introduce failure modes before
// crash_handler is wired.

#pragma once

#include <windows.h>
#include <cstddef>

namespace sao::launcher {

struct AppState;

// Parse GetCommandLineW() into an AppState.  Returns true on success.
// Sets non-fatal flags directly; on --help / --version, prints and returns
// false with `should_exit_out` set to true and `exit_code_out` populated.
bool parseCommandLine(AppState& state, bool& should_exit_out, int& exit_code_out) noexcept;

// Testable form: takes an explicit argv array (already parsed by
// CommandLineToArgvW).
bool parseCommandLineFromArgv(int argc,
                              wchar_t* const argv[],
                              AppState& state,
                              bool& should_exit_out,
                              int& exit_code_out) noexcept;

// Print --help output to stdout (or MessageBoxW if there is no console).
void printHelp() noexcept;

// Print --version output.
void printVersion() noexcept;

} // namespace sao::launcher
