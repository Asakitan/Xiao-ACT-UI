// SAO Auto — launcher/args.cpp
//
// Minimal command-line parser.  Zero third-party deps.

#include "sao/launcher/args.h"
#include "sao/launcher/app.h"

#include <windows.h>
#include <shellapi.h>
#include <cwchar>

namespace sao::launcher {

namespace {

bool wcsEqualsCI(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

bool wcsStartsWithCI(const wchar_t* s, const wchar_t* prefix) {
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

const wchar_t* normalizedLogLevel(const wchar_t* value) {
    static constexpr const wchar_t* kLevels[] = {
        L"trace", L"debug", L"info", L"warn", L"error", L"critical",
    };
    for (const auto* level : kLevels) {
        if (wcsEqualsCI(value, level)) return level;
    }
    return nullptr;
}

} // namespace

bool parseCommandLine(AppState& state, bool& should_exit_out, int& exit_code_out) noexcept {
    should_exit_out = false;
    exit_code_out   = SAO_EXIT_OK;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }
    bool ok = parseCommandLineFromArgv(argc, argv, state, should_exit_out, exit_code_out);
    LocalFree(argv);
    return ok;
}

bool parseCommandLineFromArgv(int argc,
                              wchar_t* const argv[],
                              AppState& state,
                              bool& should_exit_out,
                              int& exit_code_out) noexcept {
    should_exit_out = false;
    exit_code_out   = SAO_EXIT_OK;

    // Skip argv[0] (the exe path).
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (wcsEqualsCI(a, L"--help") || wcsEqualsCI(a, L"-h") || wcsEqualsCI(a, L"/?")) {
            printHelp();
            should_exit_out = true;
            exit_code_out   = SAO_EXIT_OK;
            return true;
        }
        if (wcsEqualsCI(a, L"--version") || wcsEqualsCI(a, L"-v")) {
            printVersion();
            should_exit_out = true;
            exit_code_out   = SAO_EXIT_OK;
            return true;
        }
        if (wcsEqualsCI(a, L"--safe-mode")) {
            state.safe_mode = true;
            continue;
        }
        // Full-stack integration harness.  ``--smoke`` promotes the launcher
        // into a console-friendly mode (init pipeline still runs but the
        // process no longer sits on the message loop).  Combined with
        // ``--exit-after-init`` the launcher prints a literal ``READY``
        // line to stdout and returns SAO_EXIT_OK once every subsystem
        // reports ready.  Both flags are inert in a shipped binary — the
        // tests/integration harness is their only consumer.
        if (wcsEqualsCI(a, L"--smoke")) {
            state.smoke_mode = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--exit-after-init")) {
            state.exit_after_init = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-operator")) {
            state.rt_io_operator = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-preflight-only")) {
            state.rt_io_operator = true;
            state.rt_io_preflight_only = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-input-checks")) {
            state.rt_io_operator = true;
            state.rt_io_input_checks = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-r5-check")) {
            state.rt_io_operator = true;
            state.rt_io_r5_check = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-mf-check")) {
            state.rt_io_operator = true;
            state.rt_io_mf_check = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-exit-after-validation")) {
            state.rt_io_operator = true;
            state.rt_io_exit_after_validation = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--rt-io-status-page")) {
            state.rt_io_force_status_page = true;
            continue;
        }
        if (wcsEqualsCI(a, L"--no-license")) {
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG)
            state.no_license = true;
            continue;
#else
            exit_code_out = SAO_EXIT_BAD_ARGS;
            return false;
#endif
        }
        if (wcsStartsWithCI(a, L"--config=")) {
            lstrcpynW(state.config_path, a + wcslen(L"--config="), MAX_PATH);
            continue;
        }
        if (wcsStartsWithCI(a, L"--log-level=")) {
            const auto* level = normalizedLogLevel(
                a + wcslen(L"--log-level="));
            if (!level) {
                exit_code_out = SAO_EXIT_BAD_ARGS;
                return false;
            }
            lstrcpynW(state.log_level, level, 16);
            continue;
        }

        // Unknown argument.  Fail closed rather than silently ignore — the
        // packaging tools invoke us with a known set, so anything else is
        // either a typo or a foreign message pump.
        exit_code_out = SAO_EXIT_BAD_ARGS;
        return false;
    }

    return true;
}

void printHelp() noexcept {
    // TODO(console-output): Attach a console via AttachConsole(ATTACH_PARENT_PROCESS)
    // and write to stdout when available; fall back to MessageBoxW.  For now
    // the stub uses MessageBoxW so the launcher stays purely GUI.
    static const wchar_t* help =
        L"SaoAuto.exe [options]\r\n\r\n"
        L"  --safe-mode           Skip plugin discovery, load core UI only\r\n"
        L"  --no-license          Bypass license verification (Debug only)\r\n"
        L"  --smoke               Enable console-attached full-stack smoke mode;\r\n"
        L"                        skip GUI message loop, print READY on stdout\r\n"
        L"  --exit-after-init     With --smoke, return after platform init is ready\r\n"
        L"  --rt-io-operator      Run the typed RT I/O operator validation flow\r\n"
        L"  --rt-io-preflight-only  Run read-only preflight, then cleanly exit\r\n"
        L"  --rt-io-input-checks  Add mouse-zero and F24 down/up checks\r\n"
        L"  --rt-io-r5-check      Add the explicit R5 fallback diagnostic\r\n"
        L"  --rt-io-mf-check      Add the explicit MF fallback diagnostic\r\n"
        L"  --rt-io-exit-after-validation  Exit after operator cleanup\r\n"
        L"  --rt-io-status-page   Keep the helper F12 status page enabled even\r\n"
        L"                        under --rt-io-operator\r\n"
        L"  --config=<path>       Override config file location\r\n"
        L"  --log-level=<lvl>     trace|debug|info|warn|error|critical\r\n"
        L"  --version, -v         Print version and exit\r\n"
        L"  --help, -h            Print this help and exit\r\n";
    MessageBoxW(nullptr, help, L"SaoAuto — Help", MB_OK | MB_ICONINFORMATION);
}

void printVersion() noexcept {
#ifndef SAO_LAUNCHER_VERSION
#define SAO_LAUNCHER_VERSION "0.0.0"
#endif
    MessageBoxA(nullptr,
                "SaoAuto " SAO_LAUNCHER_VERSION,
                "SaoAuto — Version",
                MB_OK | MB_ICONINFORMATION);
}

} // namespace sao::launcher
