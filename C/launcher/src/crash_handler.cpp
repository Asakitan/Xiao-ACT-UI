// SAO Auto — launcher/crash_handler.cpp

#include "sao/launcher/crash_handler.h"
#include "sao/launcher/working_dir.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace sao::launcher {
namespace {

constexpr std::size_t kCrashPathCapacity = 32768u;

struct CrashDirectoryState {
    wchar_t value[kCrashPathCapacity];
};

CrashDirectoryState g_crash_directories[2]{};
std::atomic<CrashDirectoryState*> g_active_crash_directory{&g_crash_directories[0]};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_prev_filter{nullptr};
wchar_t g_crash_path[kCrashPathCapacity]{};

struct CrashDirectoryInitializer {
    CrashDirectoryInitializer() noexcept {
        g_crash_directories[0].value[0] = L'.';
        g_crash_directories[0].value[1] = wchar_t(0);
    }
};

CrashDirectoryInitializer g_crash_directory_initializer{};

bool appendText(wchar_t* out, std::size_t capacity, std::size_t& length,
                const wchar_t* text) noexcept {
    if (out == nullptr || text == nullptr) return false;
    while (*text != wchar_t(0)) {
        if (length + 1u >= capacity) return false;
        out[length++] = *text++;
    }
    out[length] = wchar_t(0);
    return true;
}

bool appendUnsigned(wchar_t* out, std::size_t capacity, std::size_t& length,
                    unsigned long value) noexcept {
    wchar_t digits[20]{};
    std::size_t count = 0u;
    do {
        digits[count++] = static_cast<wchar_t>(L'0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < 20u);
    while (count != 0u) {
        if (length + 1u >= capacity) return false;
        out[length++] = digits[--count];
    }
    out[length] = wchar_t(0);
    return true;
}

bool appendFourDigits(wchar_t* out, std::size_t capacity, std::size_t& length,
                      unsigned value) noexcept {
    wchar_t digits[5]{};
    for (std::size_t index = 0u; index < 4u; ++index) {
        digits[3u - index] = static_cast<wchar_t>(L'0' + (value % 10u));
        value /= 10u;
    }
    return appendText(out, capacity, length, digits);
}

bool has_no_reparse_components(const wchar_t* path) noexcept {
    if (path == nullptr || *path == wchar_t(0)) return false;
    try {
        const std::filesystem::path input(path);
        std::filesystem::path current;
        for (const auto& component : input) {
            current /= component;
            if (component == input.root_name() ||
                component == input.root_directory()) {
                continue;
            }
            const DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool is_safe_crash_directory(const wchar_t* dir) noexcept {
    if (dir == nullptr || *dir == wchar_t(0)) return false;
    if (!has_no_reparse_components(dir)) return false;
    const DWORD attributes = GetFileAttributesW(dir);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    HANDLE handle = CreateFileW(
        dir, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO info{};
    const BOOL read = GetFileInformationByHandleEx(
        handle, FileAttributeTagInfo, &info, sizeof(info));
    CloseHandle(handle);
    return read == TRUE &&
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

void prepare_initial_crash_directory() noexcept {
    try {
        std::wstring module_path;
        std::wstring base_dir;
        if (!getCurrentModulePath(module_path) ||
            !computeBaseDir(module_path.c_str(), base_dir)) {
            return;
        }
        const std::wstring crash_dir = base_dir + L"\\crash";
        if (has_no_reparse_components(base_dir.c_str()) &&
            ensureDirectoryExists(crash_dir.c_str()) &&
            is_safe_crash_directory(crash_dir.c_str())) {
            setCrashDumpDirectory(crash_dir.c_str());
        }
    } catch (...) {
    }
}

bool buildCrashPath(wchar_t* path, std::size_t capacity) noexcept {
    if (path == nullptr || capacity == 0u) return false;
    const CrashDirectoryState* directory =
        g_active_crash_directory.load(std::memory_order_acquire);
    std::size_t length = 0u;
    if (!appendText(path, capacity, length, directory->value)) return false;
    if (length != 0u && path[length - 1u] != L'\\' && path[length - 1u] != L'/') {
        if (length + 1u >= capacity) return false;
        path[length++] = L'\\';
        path[length] = wchar_t(0);
    }
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return appendText(path, capacity, length, L"SaoAuto_") &&
           appendUnsigned(path, capacity, length, GetCurrentProcessId()) &&
           appendText(path, capacity, length, L"_") &&
           appendFourDigits(path, capacity, length, time.wYear) &&
           appendFourDigits(path, capacity, length, time.wMonth) &&
           appendFourDigits(path, capacity, length, time.wDay) &&
           appendText(path, capacity, length, L"_") &&
           appendFourDigits(path, capacity, length, time.wHour) &&
           appendFourDigits(path, capacity, length, time.wMinute) &&
           appendFourDigits(path, capacity, length, time.wSecond) &&
           appendText(path, capacity, length, L".dmp");
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    if (buildCrashPath(g_crash_path, kCrashPathCapacity))
        writeMinidump(g_crash_path, ep);
    const LPTOP_LEVEL_EXCEPTION_FILTER previous =
        g_prev_filter.load(std::memory_order_acquire);
    if (previous) return previous(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

bool installCrashHandler() noexcept {
    prepare_initial_crash_directory();
    g_prev_filter.store(SetUnhandledExceptionFilter(unhandledExceptionFilter),
                        std::memory_order_release);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    return true;
}

void uninstallCrashHandler() noexcept {
    SetUnhandledExceptionFilter(g_prev_filter.exchange(nullptr,
                                                       std::memory_order_acq_rel));
}

void setCrashDumpDirectory(const wchar_t* dir) noexcept {
    if (!is_safe_crash_directory(dir)) return;
    const std::size_t length = wcsnlen_s(dir, kCrashPathCapacity);
    if (length == 0u || length >= kCrashPathCapacity) return;
    CrashDirectoryState* active =
        g_active_crash_directory.load(std::memory_order_acquire);
    CrashDirectoryState* next = active == &g_crash_directories[0]
        ? &g_crash_directories[1] : &g_crash_directories[0];
    std::wmemcpy(next->value, dir, length);
    next->value[length] = wchar_t(0);
    g_active_crash_directory.store(next, std::memory_order_release);
}

bool getCrashDumpDirectory(wchar_t* out_dir, size_t capacity) noexcept {
    if (out_dir == nullptr || capacity == 0u) return false;
    const CrashDirectoryState* state =
        g_active_crash_directory.load(std::memory_order_acquire);
    const size_t required = std::wcslen(state->value) + 1u;
    if (required > capacity) {
        out_dir[0] = wchar_t(0);
        return false;
    }
    std::wmemcpy(out_dir, state->value, required);
    return true;
}

bool getCrashDumpDirectory(std::wstring& out_dir) noexcept {
    try {
        const CrashDirectoryState* state =
            g_active_crash_directory.load(std::memory_order_acquire);
        out_dir = state->value;
        return true;
    } catch (...) {
        out_dir.clear();
        return false;
    }
}

bool writeMinidump(const wchar_t* path, EXCEPTION_POINTERS* ep) noexcept {
    if (path == nullptr || *path == wchar_t(0)) return false;
    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                      hFile, type, ep ? &mei : nullptr,
                                      nullptr, nullptr);
    CloseHandle(hFile);
    return ok == TRUE;
}

} // namespace sao::launcher