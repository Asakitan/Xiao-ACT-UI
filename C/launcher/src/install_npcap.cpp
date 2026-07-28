// install_npcap.cpp — silent Npcap installer helper (Phase 14).
//
// Port of python/utils/install_npcap.py. Detects presence via registry
// (HKLM\SYSTEM\CurrentControlSet\Services\npcap), otherwise runs installer
// with /S (silent) flag. Requires UAC elevation on first install.

#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace sao::launcher::install {

bool npcap_present() {
#if defined(_WIN32)
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\npcap", 0,
                      KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegCloseKey(hkey);
        return true;
    }
    return false;
#else
    return false;
#endif
}

int run_silent_installer(const char* installer_path_utf8) {
#if defined(_WIN32)
    if (installer_path_utf8 == nullptr) return -1;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, installer_path_utf8, -1, nullptr, 0);
    if (wlen <= 0) return -1;
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, installer_path_utf8, -1, w.data(), wlen);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // elevate
    sei.lpFile = w.c_str();
    sei.lpParameters = L"/S";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(sei.hProcess, &exit_code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(exit_code);
#else
    (void)installer_path_utf8;
    return -1;
#endif
}

} // namespace sao::launcher::install
