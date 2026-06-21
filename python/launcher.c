#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show) {
    WCHAR exePath[MAX_PATH];
    WCHAR dirPath[MAX_PATH];
    WCHAR targetExe[MAX_PATH];

    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    /* Get directory of this launcher */
    wcscpy_s(dirPath, MAX_PATH, exePath);
    WCHAR *lastSlash = wcsrchr(dirPath, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    /* Build path to runtime/XiaoACTUI.exe */
    wsprintfW(targetExe, L"%s\\runtime\\XiaoACTUI.exe", dirPath);

    /* Check if target exists */
    if (GetFileAttributesW(targetExe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"runtime\\XiaoACTUI.exe not found", L"Error", MB_ICONERROR);
        return 1;
    }

    /* Launch with same working directory as launcher */
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;

    if (!CreateProcessW(targetExe, cmdLine, NULL, NULL, FALSE,
                        0, NULL, dirPath, &si, &pi)) {
        MessageBoxW(NULL, L"Failed to start runtime\\XiaoACTUI.exe", L"Error", MB_ICONERROR);
        return 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
