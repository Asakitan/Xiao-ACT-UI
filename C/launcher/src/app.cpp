// SAO Auto — launcher/app.cpp
//
// App orchestrates the init pipeline described in README.md.  Every step is
// a small function that returns SaoLauncherExitCode; run() chains them in
// order and aborts on the first failure.

#include "sao/launcher/app.h"
#include "sao/launcher/args.h"
#include "sao/launcher/crash_handler.h"
#include "sao/launcher/dual_run.h"
#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#include "sao/launcher/shutdown.h"
#include "sao/launcher/single_instance.h"
#include "sao/launcher/working_dir.h"

#include "launcher_lifecycle.h"

#include <cstdio>
#include <cstring>

namespace sao::launcher {

namespace {

constexpr UINT kUiFrameIntervalMs = 16;

// Console-readable smoke/operator output.  Prefer an inherited stdout pipe
// so subprocess harnesses keep deterministic capture; allocate a console only
// when the process has no usable output handle.
void consolePrintLine(const char* line) noexcept {
    if (line == nullptr)
        return;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            (void)AllocConsole();
        }
        output = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (output != nullptr && output != INVALID_HANDLE_VALUE) {
        DWORD written = 0u;
        const DWORD size = static_cast<DWORD>(std::strlen(line));
        if (WriteFile(output, line, size, &written, nullptr) &&
            written == size) {
            static constexpr char newline[] = "\r\n";
            (void)WriteFile(output, newline, sizeof(newline) - 1u,
                            &written, nullptr);
            return;
        }
    }
    std::fputs(line, stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

void smokePrint(const AppState& state, const char* tag) noexcept {
    if (state.smoke_mode)
        consolePrintLine(tag);
}

void rtIoOperatorPrint(const char* line, void*) noexcept {
    consolePrintLine(line);
}

} // namespace

App& App::instance() noexcept {
    static App instance;
    return instance;
}

bool shouldEmitRtIoReady(const AppState& state,
                         bool validation_ready) noexcept {
    return state.rt_io_operator && !state.rt_io_preflight_only &&
        validation_ready;
}

int App::run() {
    // 1. Parse the command line first — --help / --version must never trigger
    //    any subsystem init, so we return early if the parser tells us to.
    int rc = parseCommandLine();
    if (rc != SAO_EXIT_OK) {
        return rc;
    }

    LauncherLifecycleDecision lifecycle;
    if (prepareLauncherLifecycle(lifecycle, state_.rt_io_operator) !=
        SAO_STATUS_OK) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    const auto finish = [&](int exit_code, const char* hint) {
        completeLauncherLifecycle(lifecycle, exit_code, hint);
        return exit_code;
    };
    const auto fail = [&](int exit_code, const wchar_t* step, const char* hint) {
        int32_t fallback_exit = 0;
        if (sao_launcher_dual_run_maybe_fallback_to_python(&lifecycle.dual_config, exit_code, step,
                                                           &fallback_exit)) {
            shutdown();
            completeLauncherLifecycle(lifecycle, exit_code, hint);
            return fallback_exit;
        }
        shutdown();
        return finish(exit_code, hint);
    };

    // 1b. Dual-run step zero.  Read %APPDATA%\SaoAuto\dual_run.json
    // and, if the user has selected python_only or a Python-preferred variant,
    // spawn Python and hand off.  See dual_run.h for the full contract.
    // Children spawned by a parent dual-run driver have SAO_DUAL_RUN_ROLE set
    // and short-circuit dispatch to "just run CPP".
    {
        auto& dual_cfg = lifecycle.dual_config;

        wchar_t inherited_role[64]{};
        const bool is_child_of_dual_run_driver =
            (GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, inherited_role, 64) > 0) &&
            inherited_role[0] != L'\0';

        if (!is_child_of_dual_run_driver) {
            sao_status_t ms = sao_launcher_dual_run_acquire_driver_mutex(&dual_run_driver_mutex_);
            if (ms == SAO_STATUS_OK)
                dual_run_driver_acquired_ = true;
        }

        int32_t should_continue = 1;
        int32_t handoff_exit = 0;
        sao_status_t zs =
            sao_launcher_dual_run_step_zero(&dual_cfg, &should_continue, &handoff_exit);
        if (zs == SAO_LAUNCHER_PYTHON_UNAVAILABLE) {
            return fail(SAO_EXIT_PLATFORM_INIT_FAIL, L"dual_run_step_zero", "dual_run_step_zero");
        }
        if (zs != SAO_STATUS_OK) {
            return fail(SAO_EXIT_PLATFORM_INIT_FAIL, L"dual_run_step_zero", "dual_run_step_zero");
        }
        if (should_continue == 0) {
            // Smoke harness: the parent test observes the exit code (which
            // is SAO_EXIT_HANDOFF_TO_PYTHON = 100 for the python_only
            // path) plus, in smoke mode, a stdout tag it can grep for.
            smokePrint(state_, "HANDOFF_TO_PYTHON");
            shutdown();
            return finish(handoff_exit, "python_handoff");
        }
    }

    // 2. Crash handler.  Everything after this point produces a minidump on
    //    unhandled SEH.
    rc = installCrashHandler();
    if (rc != SAO_EXIT_OK) {
        // Non-fatal in principle, but if we can't install it we log and
        // continue — the shutdown path still fires normally.
    }

    // 3. Single-instance guard.
    rc = acquireSingleInstance();
    if (rc != SAO_EXIT_OK) {
        shutdown();
        return finish(rc, "single_instance");
    }

    // 4. Resolve BASE_DIR + all derived paths (crash/, logs/, plugins/, ...).
    rc = resolveWorkingDir();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"working_dir", "working_dir");
    }

    if (loadLauncherProviderConfiguration(state_.base_dir,
                                          state_.config_path[0] ? state_.config_path : nullptr) !=
        SAO_STATUS_OK) {
        return fail(SAO_EXIT_PLATFORM_INIT_FAIL, L"provider_config", "provider_config");
    }
    const auto provider_configuration = launcherProviderConfigurationSnapshot();

    // 5. License verification. The bypass exists only in the actual Debug
    // configuration, even if AppState is populated outside the CLI parser.
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG)
    const bool no_license_bypass = state_.no_license;
#else
    const bool no_license_bypass = false;
#endif
    if (!no_license_bypass && provider_configuration.license.enabled) {
        rc = verifyLicense();
        if (rc != SAO_EXIT_OK) {
            return fail(rc, L"license_verify", "license_verify");
        }
    }

    // 6. Shell integrity.  Confirms the crypter stub hasn't been tampered.
    if (provider_configuration.shell.enabled) {
        rc = verifyShellIntegrity();
        if (rc != SAO_EXIT_OK) {
            return fail(rc, L"shell_verify", "shell_verify");
        }
    }

    // 7. Security must be online before the platform can create its helper
    // session or any UI/overlay surface.
    rc = initSecurity();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"security_init", "security_init");
    }

    // 8. Platform bring-up.  From here on, subsystems are alive.
    rc = bringUpPlatform();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"platform_bringup", "platform_bringup");
    }

    // 9. Plugin discovery.  In safe mode we skip this entirely.
    if (!state_.safe_mode && provider_configuration.plugins.enabled) {
        rc = discoverPlugins();
        if (rc != SAO_EXIT_OK) {
            return fail(rc, L"plugins_discover", "plugins_discover");
        }
    }

    // Platform-ready smoke checkpoint — the pipeline just reached "every
    // subsystem is up".  ``--smoke --exit-after-init`` uses this as the
    // observable checkpoint: emit READY on stdout and stop before we
    // bring the UI online (no GUI, no message loop, no window handle
    // leaks under a subprocess).  Regular launches keep going.
    if (state_.smoke_mode && state_.exit_after_init) {
        smokePrint(state_, "READY");
        smoke_ready_printed_ = true;
        if (!state_.rt_io_operator) {
            shutdown();
            return finish(SAO_EXIT_OK, nullptr);
        }
    }

    // 10. UI online.  Displays panels + hotkeys.
    rc = bringUpUi();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"ui_bring_online", "ui_bring_online");
    }

    // UI-online smoke checkpoint for callers that want to observe the
    // UI bring-up step too.  When only
    // ``--smoke`` is set (no ``--exit-after-init``) we still print READY
    // before falling through to the message loop so
    // interactive smoke inspection works.
    if (state_.smoke_mode && !smoke_ready_printed_) {
        smokePrint(state_, "READY");
        smoke_ready_printed_ = true;
    }

    if (state_.rt_io_operator) {
        rc = runRtIoOperator();
        if (rc != SAO_EXIT_OK) {
            return fail(rc, L"rt_io_operator", "rt_io_operator");
        }
        if (state_.rt_io_preflight_only ||
            state_.rt_io_exit_after_validation ||
            (state_.smoke_mode && state_.exit_after_init)) {
            shutdown();
            return finish(SAO_EXIT_OK, nullptr);
        }
    }

    // 10. Blocking main loop.  Returns when WM_QUIT is posted.
    rc = runMessageLoop();

    shutdown();
    return finish(rc, rc == SAO_EXIT_OK ? nullptr : "message_loop");
}

int App::parseCommandLine() {
    bool should_exit = false;
    int exit_code = SAO_EXIT_OK;
    if (!::sao::launcher::parseCommandLine(state_, should_exit, exit_code)) {
        return SAO_EXIT_BAD_ARGS;
    }
    if (should_exit) {
        return exit_code;
    }
    return SAO_EXIT_OK;
}

int App::installCrashHandler() {
    // Failing crash handler install is intentionally non-fatal — we keep
    // going and the user just doesn't get a minidump on the next crash.
    (void)::sao::launcher::installCrashHandler();
    return SAO_EXIT_OK;
}

int App::acquireSingleInstance() {
    if (!::sao::launcher::acquireSingleInstance(single_instance_mutex_)) {
        // Forward our command line to the running instance before we die.
        forwardCommandLineToRunningInstance(GetCommandLineW());
        return SAO_EXIT_ALREADY_RUNNING;
    }
    return SAO_EXIT_OK;
}

int App::resolveWorkingDir() {
    if (!::sao::launcher::resolveWorkingDir(state_)) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    // Point the crash handler at <base_dir>/crash/ now that we know it.
    wchar_t crash_dir[MAX_PATH]{};
    if (swprintf_s(crash_dir, MAX_PATH, L"%ls\\crash", state_.base_dir) < 0) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    ensureDirectoryExists(crash_dir);
    setCrashDumpDirectory(crash_dir);
    lstrcpynW(SaoLauncherBaseDir, state_.base_dir, MAX_PATH);
    return SAO_EXIT_OK;
}

int App::verifyLicense() {
    sao_license_result r{};
    sao_status_t s = sao_license_verify(&r);
    state_.license_active = s != SAO_STATUS_NOT_IMPLEMENTED;
    if (s != SAO_STATUS_OK || !r.valid) {
        return SAO_EXIT_LICENSE_INVALID;
    }
    state_.streaming_entitled = isPaidLicenseTier(r.tier);
    return SAO_EXIT_OK;
}

int App::verifyShellIntegrity() {
    sao_shell_verify_result r{};
    sao_status_t s = sao_shell_verify_integrity(&r);
    state_.shell_active = s != SAO_STATUS_NOT_IMPLEMENTED;
    if (s != SAO_STATUS_OK || r.tampered) {
        return SAO_EXIT_SHELL_TAMPERED;
    }
    return SAO_EXIT_OK;
}

int App::initSecurity() {
    sao_security_config cfg{};
    cfg.enable_anti_debug = 1;
    cfg.enable_anti_dump = 1;
    cfg.enable_anti_screencap = 1;
    cfg.enable_obfuscation_runtime = 1;
    cfg.enable_user_evasion = 1;
    cfg.strict_user_evasion = 0;
    cfg.anti_debug_poll_interval_seconds = 5;

    if (sao_security_init(&cfg) != SAO_STATUS_OK) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    security_initialized_ = true;
    return SAO_EXIT_OK;
}

int App::bringUpPlatform() {
    char log_level[32]{};
    sao_platform_config cfg{};
    if (!buildPlatformConfig(state_, cfg, log_level, sizeof(log_level))) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }

    sao_platform_ctx* ctx = nullptr;
    const sao_status_t status = sao_platform_bringup(&cfg, &ctx);
    state_.platform_ctx = ctx;
    if (status != SAO_STATUS_OK || !ctx) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    return SAO_EXIT_OK;
}

int App::discoverPlugins() {
    sao_plugins_registry* reg = nullptr;
    const sao_status_t status =
        sao_plugins_discover(static_cast<sao_platform_ctx*>(state_.platform_ctx), &reg);
    state_.plugins_registry = reg;
    if (status != SAO_STATUS_OK || reg == nullptr) {
        return SAO_EXIT_PLUGIN_LOAD_FAIL;
    }

    if (sao_plugins_activate_autostart(reg) != SAO_STATUS_OK) {
        return SAO_EXIT_PLUGIN_LOAD_FAIL;
    }
    if (sao_platform_bind_plugins(static_cast<sao_platform_ctx*>(state_.platform_ctx), reg) !=
        SAO_STATUS_OK) {
        return SAO_EXIT_PLUGIN_LOAD_FAIL;
    }

    return SAO_EXIT_OK;
}

int App::bringUpUi() {
    if (sao_ui_bring_online(static_cast<sao_platform_ctx*>(state_.platform_ctx)) != SAO_STATUS_OK) {
        return SAO_EXIT_UI_ONLINE_FAIL;
    }
    if (!user_menu_.create(state_.base_dir)) {
        return SAO_EXIT_UI_ONLINE_FAIL;
    }
    return SAO_EXIT_OK;
}

int App::runRtIoOperator() {
    sao_launcher_rt_io_operator_options_t options{};
    options.struct_size = sizeof(options);
    options.preflight_only = state_.rt_io_preflight_only ? 1u : 0u;
    options.input_checks = state_.rt_io_input_checks ? 1u : 0u;
    options.r5_check = state_.rt_io_r5_check ? 1u : 0u;
    options.mf_check = state_.rt_io_mf_check ? 1u : 0u;
    options.exit_after_validation =
        state_.rt_io_exit_after_validation ? 1u : 0u;
    options.timeout_ms = 120000u;

    int32_t ready = 0;
    const sao_status_t status = sao_launcher_rt_io_operator_run(
        static_cast<sao_platform_ctx*>(state_.platform_ctx), &options,
        &rtIoOperatorPrint, nullptr, &ready);
    if (status != SAO_STATUS_OK)
        return SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL;
    if (state_.rt_io_preflight_only)
        return ready == 0 ? SAO_EXIT_OK
                          : SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL;
    if (!shouldEmitRtIoReady(state_, ready != 0))
        return SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL;
    consolePrintLine("RT_IO_READY");
    return SAO_EXIT_OK;
}

int App::runMessageLoop() {
    const UINT_PTR timer_id = SetTimer(nullptr, 0, kUiFrameIntervalMs, nullptr);
    if (timer_id == 0) {
        return SAO_EXIT_UI_ONLINE_FAIL;
    }
    MSG msg{};
    BOOL result = 0;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        int32_t handled = 0;
        sao_status_t status = SAO_STATUS_OK;
        if (msg.message == WM_TIMER && msg.wParam == timer_id) {
            handled = 1;
            status = sao_ui_tick(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                 kUiFrameIntervalMs);
        } else {
            status = sao_ui_handle_message(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                           msg.message, msg.wParam, msg.lParam, &handled);
        }
        if (status != SAO_STATUS_OK) {
            KillTimer(nullptr, timer_id);
            return SAO_EXIT_UI_ONLINE_FAIL;
        }
        if (!handled) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    KillTimer(nullptr, timer_id);
    if (result < 0)
        return SAO_EXIT_UI_ONLINE_FAIL;
    return static_cast<int>(msg.wParam);
}

void App::shutdown() noexcept {
    if (shutdown_called_) {
        return;
    }
    user_menu_.destroy();
    if (!runFullShutdown(state_, security_initialized_)) {
        return;
    }
    security_initialized_ = false;
    if (single_instance_mutex_) {
        releaseSingleInstance(single_instance_mutex_);
        single_instance_mutex_ = nullptr;
    }
    if (dual_run_driver_acquired_ && dual_run_driver_mutex_) {
        sao_launcher_dual_run_release_driver_mutex(dual_run_driver_mutex_);
        dual_run_driver_mutex_ = nullptr;
        dual_run_driver_acquired_ = false;
    }
    shutdown_called_ = true;
}

} // namespace sao::launcher
