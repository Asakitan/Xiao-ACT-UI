// SAO Auto — launcher/main.cpp
//
// Entry point for SaoAuto.exe.  We use wWinMain (not main) so the process
// doesn't spawn a console window in release builds.  In debug builds a
// console is attached lazily by args.cpp when --log-level=trace or --help
// / --version is passed.

#include <windows.h>
#include <objbase.h>
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

    return sao::launcher::App::instance().run();
}
