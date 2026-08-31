// SAO Auto — launcher/args.cpp
//
// Minimal command-line parser.  Zero third-party deps.

#include "sao/launcher/args.h"
#include "sao/launcher/app.h"

#include <windows.h>
#include <shellapi.h>
#include <cwchar>

// RT I/O command-line contract. The long spellings remain accepted for
// compatibility with internal tools and documents; neutral aliases are the
// low-exposure spelling shown to users.
//
// | enum | long name | alias | options field | consumer (file:line) | default | exit-code impact |
// | RtIoFlag::operator_mode | --rt-io-operator | --s1 | AppState::rt_io_operator -> sao_platform_config::rt_io_operator | app.cpp:387-397,567-588; init_pipeline.cpp:257-259 | false | validation failure => SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL (9); success continues the normal loop |
// | RtIoFlag::preflight_only | --rt-io-preflight-only | --s2 | AppState::rt_io_preflight_only -> sao_launcher_rt_io_operator_options_t::preflight_only | app.cpp:392-397,569,583-586; init_pipeline.cpp:1707-1850 | false | successful preflight exits 0; failed preflight => 9 |
// | RtIoFlag::input_checks | --rt-io-input-checks | --s3 | AppState::rt_io_input_checks -> sao_launcher_rt_io_operator_options_t::input_checks | app.cpp:392-397,570; init_pipeline.cpp:3457-3468 | false | operator validation failure => 9; success follows normal operator flow |
// | RtIoFlag::r5_check | --rt-io-r5-check | --s4 | AppState::rt_io_r5_check -> sao_launcher_rt_io_operator_options_t::r5_check | app.cpp:392-397,571; init_pipeline.cpp:3457-3468 | false | operator validation failure => 9; success follows normal operator flow |
// | RtIoFlag::mf_check | --rt-io-mf-check | --s5 | AppState::rt_io_mf_check -> sao_launcher_rt_io_operator_options_t::mf_check | app.cpp:392-397,572; init_pipeline.cpp:3457-3468 | false | operator validation failure => 9; success follows normal operator flow |
// | RtIoFlag::exit_after_validation | --rt-io-exit-after-validation | --s6 | AppState::rt_io_exit_after_validation -> sao_launcher_rt_io_operator_options_t::exit_after_validation | app.cpp:392-397,574; init_pipeline.cpp:1707-1850 | false | successful validation shuts down and exits 0; failure => 9 |
// | RtIoFlag::status_page | --rt-io-status-page | --s7 | AppState::rt_io_force_status_page -> sao_platform_config::rt_io_force_status_page | init_pipeline.cpp:257-259,3976-3978 | false (normal page policy unchanged) | no new exit path; with operator it only keeps the status page enabled |
//
// Detailed behavior retained here rather than in help text:
// - operator_mode selects the strict operator path and its RT_IO_READY gate.
// - preflight_only runs only the read-only preflight, cleans up, and implies
//   operator_mode for compatibility with the old parser.
// - input_checks adds mouse-zero and F24 down/up checks to live validation.
// - r5_check and mf_check add their respective diagnostics to live validation.
// - exit_after_validation implies operator_mode and exits after cleanup.
// - status_page is a page-policy override only. It deliberately does not
//   enable operator_mode; operator mode hides the page by default and this
//   flag opts back in.

namespace sao::launcher {

namespace {

enum class RtIoFlag {
    operator_mode,
    preflight_only,
    input_checks,
    r5_check,
    mf_check,
    exit_after_validation,
    status_page,
};

struct RtIoFlagSpec {
    RtIoFlag flag;
    const wchar_t* long_name;
    const wchar_t* alias;
};

constexpr RtIoFlagSpec kRtIoFlagSpecs[] = {
    {RtIoFlag::operator_mode, L"--rt-io-operator", L"--s1"},
    {RtIoFlag::preflight_only, L"--rt-io-preflight-only", L"--s2"},
    {RtIoFlag::input_checks, L"--rt-io-input-checks", L"--s3"},
    {RtIoFlag::r5_check, L"--rt-io-r5-check", L"--s4"},
    {RtIoFlag::mf_check, L"--rt-io-mf-check", L"--s5"},
    {RtIoFlag::exit_after_validation, L"--rt-io-exit-after-validation", L"--s6"},
    {RtIoFlag::status_page, L"--rt-io-status-page", L"--s7"},
};

bool wcsEqualsCI(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

const RtIoFlagSpec* findRtIoFlag(const wchar_t* argument) {
    for (const auto& spec : kRtIoFlagSpecs) {
        if (wcsEqualsCI(argument, spec.long_name) ||
            wcsEqualsCI(argument, spec.alias)) {
            return &spec;
        }
    }
    return nullptr;
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
    return parseCommandLineText(GetCommandLineW(), state, should_exit_out, exit_code_out);
}

bool parseCommandLineText(const wchar_t* command_line,
                          AppState& state,
                          bool& should_exit_out,
                          int& exit_code_out) noexcept {
    should_exit_out = false;
    exit_code_out   = SAO_EXIT_OK;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command_line, &argc);
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
#if defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
            state.smoke_mode = true;
            continue;
#else
            exit_code_out = SAO_EXIT_BAD_ARGS;
            return false;
#endif
        }
        if (wcsEqualsCI(a, L"--exit-after-init")) {
#if defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
            state.exit_after_init = true;
            continue;
#else
            exit_code_out = SAO_EXIT_BAD_ARGS;
            return false;
#endif
        }
        if (const RtIoFlagSpec* rt_io_flag = findRtIoFlag(a)) {
            switch (rt_io_flag->flag) {
            case RtIoFlag::operator_mode:
                state.rt_io_operator = true;
                break;
            case RtIoFlag::preflight_only:
                state.rt_io_operator = true;
                state.rt_io_preflight_only = true;
                break;
            case RtIoFlag::input_checks:
                state.rt_io_operator = true;
                state.rt_io_input_checks = true;
                break;
            case RtIoFlag::r5_check:
                state.rt_io_operator = true;
                state.rt_io_r5_check = true;
                break;
            case RtIoFlag::mf_check:
                state.rt_io_operator = true;
                state.rt_io_mf_check = true;
                break;
            case RtIoFlag::exit_after_validation:
                state.rt_io_operator = true;
                state.rt_io_exit_after_validation = true;
                break;
            case RtIoFlag::status_page:
                // Page policy only; this flag deliberately does not enable
                // the operator path.
                state.rt_io_force_status_page = true;
                break;
            }
            continue;
        }
        if (wcsEqualsCI(a, L"--no-license")) {
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
            state.no_license = true;
            continue;
#else
            exit_code_out = SAO_EXIT_BAD_ARGS;
            return false;
#endif
        }
        if (wcsStartsWithCI(a, L"--file=")) {
            const wchar_t* path = a + wcslen(L"--file=");
            if (path[0] == L'\0') {
                exit_code_out = SAO_EXIT_BAD_ARGS;
                return false;
            }
            state.open_path.assign(path);
            continue;
        }
        if (wcsStartsWithCI(a, L"--config=")) {
            state.config_path.assign(a + wcslen(L"--config="));
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
        L"  --safe-mode           Skip plugins and privileged RT I/O; load core UI only\r\n"
        L"  --no-license          Bypass license verification (Debug only)\r\n"
    #if defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
        L"  --smoke               Enable console-attached full-stack smoke mode;\r\n"
        L"                        skip GUI message loop, print READY on stdout\r\n"
        L"  --exit-after-init     With --smoke, return after platform init is ready\r\n"
    #endif
        L"  --s1                  Runtime option S1\r\n"
        L"  --s2                  Runtime option S2\r\n"
        L"  --s3                  Runtime option S3\r\n"
        L"  --s4                  Runtime option S4\r\n"
        L"  --s5                  Runtime option S5\r\n"
        L"  --s6                  Runtime option S6\r\n"
        L"  --s7                  Runtime option S7\r\n"
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
