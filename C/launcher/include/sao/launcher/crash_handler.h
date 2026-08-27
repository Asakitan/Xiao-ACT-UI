// SAO Auto — launcher/crash_handler.h
//
// Installs a top-level unhandled-exception filter that writes a minidump
// to <base_dir>/crash/SaoAuto_<pid>_<timestamp>.dmp before letting Windows
// close the process.
//
// Called FIRST in the init pipeline so any crash inside the pipeline itself
// still produces a diagnosable dump.

#pragma once

#include <cstddef>
#include <string>
#include <windows.h>

namespace sao::launcher {

// Install the unhandled-exception filter.  Safe to call once per process.
// Returns true on success.
bool installCrashHandler() noexcept;

// Uninstall (restores the previous filter, if any).  Not normally called
// during shutdown — we want the filter live until the very end.
void uninstallCrashHandler() noexcept;

// Path where the next crash dump will land.  base_dir is joined with
// "crash/" and the file is named by PID + timestamp.  Called from
// resolveWorkingDir() after BASE_DIR is known.
void setCrashDumpDirectory(const wchar_t* dir) noexcept;

// Copy the current crash directory, including the terminator. Returns false
// when the output buffer is null or too small.
bool getCrashDumpDirectory(wchar_t* out_dir, size_t capacity) noexcept;
bool getCrashDumpDirectory(std::wstring& out_dir) noexcept;

// Testable form: write a minidump to `path` from the given exception info.
// Returns true on success.
bool writeMinidump(const wchar_t* path, EXCEPTION_POINTERS* ep) noexcept;

} // namespace sao::launcher
