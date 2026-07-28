// SAO Auto — launcher/app.h
//
// The App class is the launcher's top-level state machine.  It owns exactly
// one process lifetime and drives the init pipeline described in README.md.
//
// The launcher only ever holds one App instance (constructed inside
// wWinMain).  Everything else is a member owned by App or a subsystem
// handle returned through the C ABI.
//
// Do NOT expose C++ types from any subsystem here.  All fields must be
// either raw Win32 types or opaque handles.  If you need to add cross-
// subsystem state, add it to init_pipeline.h instead.

#pragma once

#include "sao/launcher/user_menu.h"

#include <cstdint>
#include <windows.h>

namespace sao::launcher {

// Exit codes.  Kept in sync with README.md's table.
//
// SAO_EXIT_HANDOFF_TO_PYTHON is kept at 100 to leave a clear gap between the
// process-init failure codes (0..8) and the dispatch-only handoff signal.
// dual_run.h historically declared a ``#define SAO_EXIT_HANDOFF_TO_PYTHON 100``
// so C-only TUs that never included this header still saw the value.  The
// ``#undef`` below removes that macro when both headers are visible in the
// same TU so the enum entry (the ABI source of truth) declares cleanly; the
// numeric value is preserved.
#ifdef SAO_EXIT_HANDOFF_TO_PYTHON
#undef SAO_EXIT_HANDOFF_TO_PYTHON
#endif
enum SaoLauncherExitCode : int {
    SAO_EXIT_OK = 0,
    SAO_EXIT_ALREADY_RUNNING = 1,
    SAO_EXIT_LICENSE_INVALID = 2,
    SAO_EXIT_SHELL_TAMPERED = 3,
    SAO_EXIT_PLATFORM_INIT_FAIL = 4,
    SAO_EXIT_PLUGIN_LOAD_FAIL = 5,
    SAO_EXIT_UI_ONLINE_FAIL = 6,
    SAO_EXIT_CRASH = 7,
    SAO_EXIT_BAD_ARGS = 8,
    SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL = 9,
    SAO_EXIT_HANDOFF_TO_PYTHON = 100,
};

// Compact snapshot of the parsed command line + init state.  Kept flat so we
// can hand it to C ABI functions without needing to translate C++ layout.
struct AppState {
    // Command line.
    bool safe_mode = false;
    bool no_license = false;
    // Full-stack smoke harness — `--smoke` flips the launcher into a
    // console-attached, non-GUI mode used by the tests/integration/
    // ``test_full_stack_integration`` cases.  Combined with ``--exit-after-init``
    // it drives the pipeline through platform bring-up and prints a
    // literal ``READY`` line on stdout so the parent test process can
    // observe successful init without opening a window.  Neither flag
    // survives into a shipped exe — they are purely a test entry point.
    bool smoke_mode = false;
    bool exit_after_init = false;
    bool rt_io_operator = false;
    bool rt_io_preflight_only = false;
    bool rt_io_input_checks = false;
    bool rt_io_r5_check = false;
    bool rt_io_mf_check = false;
    bool rt_io_exit_after_validation = false;
    // Overrides the default operator-mode stealth posture that hides the
    // helper's F12 status page.  Does not itself enable rt_io_operator.
    bool rt_io_force_status_page = false;
    wchar_t config_path[MAX_PATH] = {0};
    wchar_t log_level[16] = {0};

    // Resolved paths.
    wchar_t base_dir[MAX_PATH] = {0};
    wchar_t exe_path[MAX_PATH] = {0};

    // Init pipeline handles.  Void* on purpose — they belong to whichever
    // subsystem produced them.
    void* platform_ctx = nullptr;     // sao_platform_ctx*
    void* plugins_registry = nullptr; // sao_plugins_registry*
    bool shell_active = false;
    bool license_active = false;
    bool streaming_entitled = false;
};

bool shouldEmitRtIoReady(const AppState& state,
                         bool validation_ready) noexcept;

// The launcher singleton.  Only one instance lives per process.
class App {
  public:
    static App& instance() noexcept;

    // Full lifecycle: parse args, run the init pipeline, pump the main
    // message loop, then run shutdown().  Returns SaoLauncherExitCode.
    int run();

    // Individual pipeline steps.  Exposed for tests + the safe-mode path
    // that skips some of them.
    int parseCommandLine();
    int installCrashHandler();
    int acquireSingleInstance();
    int resolveWorkingDir();
    int verifyLicense();
    int verifyShellIntegrity();
    int initSecurity();
    int bringUpPlatform();
    int discoverPlugins();
    int bringUpUi();
    int runRtIoOperator();

    // Main loop.  Blocks until WM_QUIT is posted.  Runs after every step
    // above succeeded.
    int runMessageLoop();

    // Reverse-order teardown.  Called by run() on both success and every
    // fatal-error branch.  Safe to call multiple times.
    void shutdown() noexcept;

    // State accessor for tests.
    const AppState& state() const noexcept {
        return state_;
    }

  private:
    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    AppState state_{};
    UserMenu user_menu_{};
    HANDLE single_instance_mutex_ = nullptr;
    HANDLE dual_run_driver_mutex_ = nullptr;
    bool dual_run_driver_acquired_ = false;
    bool security_initialized_ = false;
    bool smoke_ready_printed_ = false;
    bool shutdown_called_ = false;
};

} // namespace sao::launcher
