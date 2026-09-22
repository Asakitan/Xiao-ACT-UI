// SAO Auto — launcher/main.cpp
//
// Entry point for SaoAuto.exe.  We use wWinMain (not main) so the process
// doesn't spawn a console window in release builds.  In debug builds a
// console is attached lazily by args.cpp when --log-level=trace or --help
// / --version is passed.

#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include "sao/launcher/app.h"

// wWinMain signature per the Windows SDK.  We ignore the hInstance /
// hPrevInstance / lpCmdLine / nCmdShow arguments — everything we need
// comes from GetCommandLineW() and GetModuleHandleW(nullptr) later.
extern "C" int WINAPI wWinMain(_In_ HINSTANCE       /*hInstance*/,
                               _In_opt_ HINSTANCE   /*hPrevInstance*/,
                               _In_ LPWSTR          /*lpCmdLine*/,
                               _In_ int             /*nCmdShow*/) {
    struct UiApartment {
        HRESULT status{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
        ~UiApartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    };
    // Construct before App so its compositor retires before this apartment.
    static UiApartment apartment;
    if (FAILED(apartment.status)) {
        OutputDebugStringW(L"SAO UI STA initialization failed.\n");
        return 1;
    }
    // Bump the process working set trim policy so Windows doesn't page us
    // out aggressively while the UI is idle in the tray.  This is a tiny
    // quality-of-life tweak; failure is non-fatal.
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             static_cast<SIZE_T>(-1),
                             static_cast<SIZE_T>(-1));

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE diagnostic_output = nullptr;
    if (output != nullptr && output != INVALID_HANDLE_VALUE) {
        if (DuplicateHandle(GetCurrentProcess(), output, GetCurrentProcess(), &diagnostic_output,
                            0u, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
            diagnostic_output = output;
        }
    }
    const int exit_code = sao::launcher::App::instance().run();
    if (diagnostic_output != nullptr && diagnostic_output != INVALID_HANDLE_VALUE) {
        char line[64]{};
        const int length = std::snprintf(line, sizeof(line), "SAO_PROCESS_RETURN code=%d\r\n",
                                         exit_code);
        if (length > 0) {
            DWORD written = 0u;
            (void)WriteFile(diagnostic_output, line, static_cast<DWORD>(std::strlen(line)),
                            &written, nullptr);
        }
    }
    if (diagnostic_output != output && diagnostic_output != nullptr &&
        diagnostic_output != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(diagnostic_output);
    }
    return exit_code;
}
