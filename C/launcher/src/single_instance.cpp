// SAO Auto — launcher/single_instance.cpp

#include "sao/launcher/single_instance.h"

#include <windows.h>
#include <cwchar>
#include <cstdint>

namespace sao::launcher {

namespace {

// Deterministic hash of a wide string.  We don't need cryptographic strength
// here — just enough to avoid two co-installed copies (e.g., one in
// C:\SaoAuto and one in D:\SaoAuto) fighting for the same mutex.
uint64_t fnv1a64(const wchar_t* s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= static_cast<uint64_t>(*s);
        h *= 1099511628211ULL;
    }
    return h;
}

void buildMutexName(wchar_t* out, size_t out_cap) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    _snwprintf_s(out, out_cap, _TRUNCATE, L"Global\\SaoAuto.Instance.%016llx", fnv1a64(exe));
}

} // namespace

bool acquireSingleInstance(HANDLE& mutex_out) noexcept {
    wchar_t name[128]{};
    buildMutexName(name, 128);

    HANDLE m = CreateMutexW(nullptr, FALSE, name);
    if (!m) {
        mutex_out = nullptr;
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        mutex_out = nullptr;
        return false;
    }
    mutex_out = m;
    return true;
}

void releaseSingleInstance(HANDLE mutex) noexcept {
    if (mutex) {
        CloseHandle(mutex);
    }
}

void forwardCommandLineToRunningInstance(const wchar_t* cmdline) noexcept {
    // Look for the running instance's message window.  The platform/ui
    // subsystem publishes a hidden window with class name "SaoAutoMsgHwnd"
    // for exactly this purpose.  When we can't find it (e.g., platform/ui
    // not built), we just silently fail — the second instance will exit
    // and the user will have to focus the first instance manually.
    HWND target = FindWindowW(L"SaoAutoMsgHwnd", nullptr);
    if (!target) return;

    COPYDATASTRUCT cds{};
    cds.dwData = 0x5A051L;     // arbitrary tag, ui subsystem must recognise it
    cds.cbData = static_cast<DWORD>((wcslen(cmdline) + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(cmdline);
    SendMessageTimeoutW(target,
                        WM_COPYDATA,
                        0,
                        reinterpret_cast<LPARAM>(&cds),
                        SMTO_ABORTIFHUNG,
                        1000,
                        nullptr);
}

} // namespace sao::launcher
