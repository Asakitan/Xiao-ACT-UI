#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(linker, "/NODEFAULTLIB")
#pragma comment(linker, "/ENTRY:_start")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

void __stdcall _start(void) {
    WCHAR path[MAX_PATH], dir[MAX_PATH], target[MAX_PATH + 32];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int i;
    WCHAR *s;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    for (i = 0; path[i]; i++) dir[i] = path[i];
    dir[i] = 0;
    s = dir;
    WCHAR *last = NULL;
    for (; *s; s++) if (*s == L'\\') last = s;
    if (last) *last = 0;

    i = 0;
    for (s = dir; *s; s++) target[i++] = *s;
    WCHAR suffix[] = L"\\runtime\\XiaoACTUI.exe";
    for (s = suffix; *s; s++) target[i++] = *s;
    target[i] = 0;

    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
        ExitProcess(2);
    }

    __stosb((unsigned char*)&si, 0, sizeof(si));
    __stosb((unsigned char*)&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;

    if (!CreateProcessW(target, GetCommandLineW(), NULL, NULL, FALSE,
                        0, NULL, dir, &si, &pi)) {
        ExitProcess(3);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    ExitProcess(0);
}
