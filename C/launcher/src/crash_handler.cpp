// SAO Auto — launcher/crash_handler.cpp
//
// Minidump on unhandled SEH.  Uses dbghelp!MiniDumpWriteDump directly so we
// don't drag in any additional deps.

#include "sao/launcher/crash_handler.h"

#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace sao::launcher {

namespace {

wchar_t             g_crash_dir[MAX_PATH]     = L".";
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter    = nullptr;

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    // Build "SaoAuto_<pid>_<UTC yyyymmdd_hhmmss>.dmp"
    SYSTEMTIME t{};
    GetSystemTime(&t);
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"%s\\SaoAuto_%lu_%04u%02u%02u_%02u%02u%02u.dmp",
                 g_crash_dir,
                 GetCurrentProcessId(),
                 t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
    writeMinidump(path, ep);

    // Chain to any previous filter (e.g., Windows Error Reporting) so we
    // still show the standard crash dialog + collect telemetry.
    if (g_prev_filter) {
        return g_prev_filter(ep);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

bool installCrashHandler() noexcept {
    g_prev_filter = SetUnhandledExceptionFilter(unhandledExceptionFilter);
    // Disable the Windows error dialog so our minidump handler runs
    // deterministically rather than the OS asking the user first.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    return true;
}

void uninstallCrashHandler() noexcept {
    SetUnhandledExceptionFilter(g_prev_filter);
    g_prev_filter = nullptr;
}

void setCrashDumpDirectory(const wchar_t* dir) noexcept {
    if (!dir || !*dir) return;
    lstrcpynW(g_crash_dir, dir, MAX_PATH);
}

bool writeMinidump(const wchar_t* path, EXCEPTION_POINTERS* ep) noexcept {
    HANDLE hFile = CreateFileW(path,
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    // MiniDumpWithDataSegs + Handles + ThreadInfo covers 99% of what we need
    // for post-mortem.  Full-memory dumps are avoided (they can be many GB).
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules |
        MiniDumpWithProcessThreadData);

    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(),
                                GetCurrentProcessId(),
                                hFile,
                                type,
                                ep ? &mei : nullptr,
                                nullptr,
                                nullptr);
    CloseHandle(hFile);
    return ok == TRUE;
}

} // namespace sao::launcher
