// SAO Auto — launcher/app.cpp
//
// App orchestrates the init pipeline described in README.md.  Every step is
// a small function that returns SaoLauncherExitCode; run() chains them in
// order and aborts on the first failure.

#include "sao/launcher/app.h"
#include "sao/launcher/args.h"
#include "sao/launcher/boot_residency.h"
#if defined(SAO_LAUNCHER_HAS_AUTO_UPDATE)
#include "sao/launcher/auto_update.h"
#endif
#include "sao/launcher/crash_handler.h"
#include "sao/launcher/dual_run.h"
#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#include "sao/launcher/shutdown.h"
#include "sao/launcher/single_instance.h"
#include "sao/launcher/user_guide_webview.h"
#include "sao/launcher/user_menu.h"
#include "sao/launcher/working_dir.h"

#ifdef SAO_STATUS_OK
#undef SAO_STATUS_OK
#endif
#include "sao/ui/linkstart_intro.h"

#include "launcher_lifecycle.h"

#if __has_include("sao_security/anti_screencap/capture_mode.h")
#include "sao_security/anti_screencap/capture_mode.h"
#define SAO_LAUNCHER_HAS_ANTI_SCREENCAP_API 1
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <thread>

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
        if (WriteFile(output, line, size, &written, nullptr) && written == size) {
            static constexpr char newline[] = "\r\n";
            (void)WriteFile(output, newline, sizeof(newline) - 1u, &written, nullptr);
            return;
        }
    }
    std::fputs(line, stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

void smokePrint(const AppState& state, const char* tag) noexcept {
#if defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
    if (state.smoke_mode)
        consolePrintLine(tag);
#else
    (void)state;
    (void)tag;
#endif
}

bool acceptanceSmokeMode(const AppState& state) noexcept {
#if defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
    return state.smoke_mode;
#else
    (void)state;
    return false;
#endif
}

bool acceptanceExitAfterInit(const AppState& state) noexcept {
#if defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
    return state.smoke_mode && state.exit_after_init;
#else
    (void)state;
    return false;
#endif
}

void rtIoOperatorPrint(const char* line, void*) noexcept {
    consolePrintLine(line);
}

// Startup diagnostics: enabled by --log-level=trace|debug.  Emits the
// resolved command-line configuration plus every pipeline exit/fail mark
// so a hung or partially-booting launcher is diagnosable from the parent
// console without a debugger.
bool traceDiagnosticsEnabled(const AppState& state) noexcept {
    return _wcsicmp(state.log_level, L"trace") == 0 || _wcsicmp(state.log_level, L"debug") == 0;
}

void tracePrint(const AppState& state, const char* line) noexcept {
    if (traceDiagnosticsEnabled(state))
        consolePrintLine(line);
}

void tracePrintfImpl(const AppState& state, const char* format, ...) noexcept {
    if (!traceDiagnosticsEnabled(state))
        return;
    char buffer[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    consolePrintLine(buffer);
}

void logStartupConfiguration(const AppState& state) noexcept {
    if (!traceDiagnosticsEnabled(state))
        return;
#ifndef SAO_LAUNCHER_VERSION
#define SAO_LAUNCHER_VERSION "0.0.0"
#endif
    tracePrintfImpl(
        state,
        "STARTUP SaoAuto " SAO_LAUNCHER_VERSION " safe_mode=%d smoke=%d rt_io_operator=%d "
        "preflight=%d input_checks=%d r5=%d mf=%d "
        "exit_after_validation=%d status_page=%d "
        "log_level=%ls config=%ls",
        state.safe_mode ? 1 : 0, state.smoke_mode ? 1 : 0, state.rt_io_operator ? 1 : 0,
        state.rt_io_preflight_only ? 1 : 0, state.rt_io_input_checks ? 1 : 0,
        state.rt_io_r5_check ? 1 : 0, state.rt_io_mf_check ? 1 : 0,
        state.rt_io_exit_after_validation ? 1 : 0, state.rt_io_force_status_page ? 1 : 0,
        state.log_level[0] ? state.log_level : L"info",
        state.config_path.empty() ? L"<default>" : state.config_path.c_str());
}

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)

// Driver/engine bootstrap stages rendered on the Link Start rail while the
// launcher is still initialising.  Ordering is the historical bring-up order:
// driver chain, engine surfaces, capture shield, tagWND scrub, plugin
// runtimes, then plugin engines.
constexpr uint32_t kBootstrapStageCount = 6u;
constexpr const char* kBootstrapCaptions[kBootstrapStageCount] = {
    "DRIVER CHAIN",  "ENGINE SURFACES", "CAPTURE SHIELD",
    "WINDOW SCRUB",  "ENGINE RUNTIMES", "PLUGIN ENGINES",
};

// Runs the driver chain off the owner thread so the intro keeps animating while
// the helper/driver bootstrap blocks.  The stage list is fixed before start();
// the owner thread only reads the telemetry atomics.
class IntroBootstrapWorker {
  public:
    using Step = sao_status_t (*)(void* user_data);

    void add(const char* caption, Step step, void* user_data) noexcept {
        if (stage_count_ < kCapacity)
            stages_[stage_count_++] = Stage{caption, step, user_data};
    }

    void start() noexcept {
        thread_ = std::thread([this] { execute(); });
    }

    void join() noexcept {
        if (thread_.joinable())
            thread_.join();
    }

    bool finished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }

    sao_status_t result() const noexcept {
        return result_.load(std::memory_order_relaxed);
    }

  private:
    struct Stage {
        const char* caption;
        Step step;
        void* user_data;
    };
    static constexpr uint32_t kCapacity = 6u;

    void execute() noexcept {
        sao_status_t status = SAO_STATUS_OK;
        for (uint32_t index = 0; index < stage_count_; ++index) {
            status = stages_[index].step != nullptr ? stages_[index].step(stages_[index].user_data)
                                                    : SAO_STATUS_OK;
            if (status != SAO_STATUS_OK)
                break;
        }
        result_.store(status, std::memory_order_relaxed);
        finished_.store(true, std::memory_order_release);
    }

    Stage stages_[kCapacity]{};
    uint32_t stage_count_ = 0u;
    std::atomic<sao_status_t> result_{SAO_STATUS_OK};
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

struct BootstrapStageContext {
    const sao_platform_config* cfg = nullptr;
    sao_platform_ctx* platform = nullptr;
};

sao_status_t bootstrap_step_drivers(void* user_data) {
    auto* context = static_cast<BootstrapStageContext*>(user_data);
    return sao_platform_bringup_drivers(context->cfg, context->platform);
}

sao_status_t bootstrap_step_capture_shield(void* user_data) {
    auto* context = static_cast<BootstrapStageContext*>(user_data);
    return sao_platform_bringup_capture_shield(context->cfg, context->platform);
}

sao_status_t bootstrap_step_wnd_scrub(void* user_data) {
    auto* context = static_cast<BootstrapStageContext*>(user_data);
    return sao_platform_bringup_wnd_scrub(context->cfg, context->platform);
}

// Publishes the stage currently being bootstrapped to the intro rail.
void publish_intro_bootstrap(sao_platform_ctx* ctx, uint32_t stage_index, bool failed) {
    SaoUiLinkStartBootstrap state{};
    state.struct_size = sizeof(state);
    state.stage_index = stage_index;
    state.stage_count = kBootstrapStageCount;
    state.stage_progress = 0.0F;
    state.flags = failed ? SAO_UI_LINKSTART_BOOTSTRAP_FLAG_FAILED : 0u;
    state.caption_utf8 = stage_index < kBootstrapStageCount ? kBootstrapCaptions[stage_index]
                                                            : kBootstrapCaptions[0];
    (void)sao_ui_intro_publish_bootstrap(ctx, &state);
}

// Drains the owner-thread window queue and advances the intro one frame.
// Returns false once WM_QUIT was seen; the quit request is pushed back so the
// later runMessageLoop() still observes it.
bool pump_intro_frame(sao_platform_ctx* ctx) {
    bool alive = true;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            alive = false;
            continue;
        }
        int32_t handled = 0;
        if (sao_ui_handle_message(ctx, msg.message, msg.wParam, msg.lParam, &handled) !=
            SAO_STATUS_OK) {
            handled = 0;
        }
        if (!handled) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (alive)
        (void)sao_ui_intro_pump(ctx);
    return alive;
}

#endif // SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER

} // namespace

App& App::instance() noexcept {
    static App instance;
    return instance;
}

bool shouldEmitRtIoReady(const AppState& state, bool validation_ready) noexcept {
    return state.rt_io_operator && !state.rt_io_preflight_only && validation_ready;
}

int App::run() {
    // 1. Parse the command line first — --help / --version must never trigger
    //    any subsystem init, so we return early if the parser tells us to.
    int rc = parseCommandLine();
    if (rc != SAO_EXIT_OK) {
        return rc;
    }
    smokePrint(state_, "STAGE_ARGS");
    logStartupConfiguration(state_);

    rc = installCrashHandler();
    if (rc != SAO_EXIT_OK) {
        return rc;
    }

    LauncherLifecycleDecision lifecycle;
    if (prepareLauncherLifecycle(lifecycle, state_.rt_io_operator) != SAO_STATUS_OK) {
        ::sao::launcher::uninstallCrashHandler();
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    smokePrint(state_, "STAGE_LIFECYCLE");
    const auto finish = [&](int exit_code, const char* hint) {
        tracePrintfImpl(state_, "EXIT %s code=%d", hint, exit_code);
        completeLauncherLifecycle(lifecycle, exit_code, hint);
        return exit_code;
    };
    const auto fail = [&](int exit_code, const wchar_t* step, const char* hint) {
        tracePrintfImpl(state_, "FAIL step=%ls hint=%s code=%d", step, hint, exit_code);
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
        int32_t fallback_exit = 0;
        if (sao_launcher_dual_run_maybe_fallback_to_python_v2(&lifecycle.dual_config_v2, exit_code,
                                                              step, &fallback_exit)) {
            if (!shutdown())
                return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown_pending");
            completeLauncherLifecycle(lifecycle, fallback_exit, "python_handoff");
            return fallback_exit;
        }
#endif
        if (!shutdown())
            return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown_pending");
        return finish(exit_code, hint);
    };

#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
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

        bool plain_cpp_due_to_existing_driver = false;
        if (!is_child_of_dual_run_driver) {
            sao_status_t ms = sao_launcher_dual_run_acquire_driver_mutex(&dual_run_driver_mutex_);
            if (ms == SAO_STATUS_OK)
                dual_run_driver_acquired_ = true;
            else if (ms == SAO_LAUNCHER_ALREADY_RUNNING) {
                dual_cfg.mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
                lifecycle.dual_config_v2.legacy.mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
                lifecycle.selected_mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
                plain_cpp_due_to_existing_driver = true;
            } else {
                return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "dual_run_driver_mutex");
            }
        }

        int32_t should_continue = 1;
        int32_t handoff_exit = 0;
        sao_status_t zs = plain_cpp_due_to_existing_driver
                              ? SAO_STATUS_OK
                              : sao_launcher_dual_run_step_zero_v2(&lifecycle.dual_config_v2,
                                                                   &should_continue, &handoff_exit);
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
            if (!shutdown())
                return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown_pending");
            return finish(handoff_exit, "python_handoff");
        }
    }
#endif
    smokePrint(state_, "STAGE_DUAL_RUN");

    // 2. Single-instance guard.
    rc = acquireSingleInstance();
    if (rc != SAO_EXIT_OK) {
        if (!shutdown())
            return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown_pending");
        return finish(rc, "single_instance");
    }
    smokePrint(state_, "STAGE_SINGLE_INSTANCE");

    // 4. Resolve BASE_DIR + all derived paths (crash/, logs/, plugins/, ...).
    rc = resolveWorkingDir();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"working_dir", "working_dir");
    }

    if (loadLauncherProviderConfiguration(
            state_.base_dir.c_str(),
            state_.config_path.empty() ? nullptr : state_.config_path.c_str()) != SAO_STATUS_OK) {
        return fail(SAO_EXIT_PLATFORM_INIT_FAIL, L"provider_config", "provider_config");
    }
    const auto provider_configuration = launcherProviderConfigurationSnapshot();
    smokePrint(state_, "STAGE_PROVIDER_CONFIG");

    // 5. License verification. The bypass exists only in the actual Debug
    // configuration, even if AppState is populated outside the CLI parser.
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
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
    smokePrint(state_, "STAGE_SECURITY");

    // 8. Platform bring-up.  From here on, subsystems are alive.  The
    //    production composition provider brings the surfaces up first so the
    //    Link Start intro can cover the driver/engine bootstrap that follows.
#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    rc = bringUpSurface();
#else
    rc = bringUpPlatform();
#endif
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"platform_bringup", "platform_bringup");
    }
    int32_t user_guide_presented = 1;
    first_run_ =
        sao_platform_user_guide_presented(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                          &user_guide_presented) == SAO_STATUS_OK &&
        user_guide_presented == 0;
    user_guide_open_pending_ = false;
    user_guide_open_accepted_ = false;
    user_guide_skip_intro_ = false;
    user_guide_next_retry_ = 0;
    smokePrint(state_, "STAGE_PLATFORM");

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    // 8.5 + 9 — the driver chain, the engine surfaces, the plugin runtimes and
    // the plugin engines bootstrap underneath the intro animation.
    rc = runBootstrapUnderIntro();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"bootstrap_under_intro", "bootstrap_under_intro");
    }
#else
    if (ensureConfiguredPluginRuntimes(state_, provider_configuration.plugins) != SAO_STATUS_OK) {
        return fail(SAO_EXIT_PLUGIN_LOAD_FAIL, L"runtime_installer", "runtime_installer");
    }

    // 9. Plugin discovery.  In safe mode we skip this entirely.
    if (!state_.safe_mode && provider_configuration.plugins.enabled) {
        rc = discoverPlugins();
        if (rc != SAO_EXIT_OK) {
            return fail(rc, L"plugins_discover", "plugins_discover");
        }
    }
#endif
    smokePrint(state_, "STAGE_PLUGINS");

    // Platform-ready smoke checkpoint — the pipeline just reached "every
    // subsystem is up".  ``--smoke --exit-after-init`` uses this as the
    // observable checkpoint: emit READY on stdout and stop before we
    // bring the UI online (no GUI, no message loop, no window handle
    // leaks under a subprocess).  Regular launches keep going.  In strict
    // rt-io operator mode the only readiness signal is the gated
    // RT_IO_READY emitted after the strict chain succeeds; the generic
    // platform checkpoint must not claim readiness first.
    if (acceptanceExitAfterInit(state_)) {
        if (!state_.rt_io_operator) {
            smokePrint(state_, "READY");
            smoke_ready_printed_ = true;
            smokePrint(state_, "STAGE_SHUTDOWN_BEGIN");
            const bool shutdown_complete = shutdown();
            smokePrint(state_, "STAGE_SHUTDOWN_END");
            if (!shutdown_complete) {
                return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown");
            }
            return finish(SAO_EXIT_OK, nullptr);
        }
    }

    // 10. UI online.  Displays panels + hotkeys.
    rc = bringUpUi();
    if (rc != SAO_EXIT_OK) {
        return fail(rc, L"ui_bring_online", "ui_bring_online");
    }

    // Boot-residency restart UX (W6.9).  After the UI is online, offer the
    // one-shot restart prompt when the helper armed a boot-start promotion
    // this session and the machine has not yet performed the real restart.
    // Skipped in smoke/acceptance/operator runs and safe mode (the probe
    // suppresses safe mode internally).
    if (!state_.smoke_mode && !state_.exit_after_init && !state_.rt_io_operator) {
        const int32_t prompt_result = sao::launcher::boot_residency_prompt_if_required(nullptr);
        if (prompt_result == sao::launcher::BOOT_RESIDENCY_ERROR) {
            // A failed restart scheduling is non-fatal: the latch survives
            // and the prompt reappears on the next launch.
            consolePrintLine("BOOT_RESIDENCY_RESTART_SCHEDULE_FAILED");
        } else if (prompt_result == sao::launcher::BOOT_RESIDENCY_RESTART_ACCEPTED) {
            consolePrintLine("BOOT_RESIDENCY_RESTART_SCHEDULED");
        }
    }

#if defined(SAO_LAUNCHER_HAS_AUTO_UPDATE)
    if (!state_.smoke_mode && !state_.exit_after_init && !state_.rt_io_operator &&
        provider_configuration.update.enabled) {
        MSG queue_probe{};
        (void)PeekMessageW(&queue_probe, nullptr, 0, 0, PM_NOREMOVE);
        startAutoUpdate(provider_configuration.update, state_.base_dir,
                        std::wstring(state_.exe_path), GetCurrentThreadId(),
                        &auto_update_cancel_event_, &auto_update_worker_);
    }
#endif
    // UI-online smoke checkpoint for callers that want to observe the
    // UI bring-up step too.  When only
    // ``--smoke`` is set (no ``--exit-after-init``) we still print READY
    // before falling through to the message loop so
    // interactive smoke inspection works.  Strict operator mode reports
    // readiness exclusively through the gated RT_IO_READY.
    if (acceptanceSmokeMode(state_) && !smoke_ready_printed_ && !state_.rt_io_operator) {
        smokePrint(state_, "READY");
        smoke_ready_printed_ = true;
    }

    if (state_.rt_io_operator) {
        rc = runRtIoOperator();
        if (rc != SAO_EXIT_OK) {
            return fail(rc, L"rt_io_operator", "rt_io_operator");
        }
        if (state_.rt_io_preflight_only || state_.rt_io_exit_after_validation ||
            acceptanceExitAfterInit(state_)) {
            if (!shutdown()) {
                return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown");
            }
            return finish(SAO_EXIT_OK, nullptr);
        }
    }

    // 10. Blocking main loop.  Returns when WM_QUIT is posted.
    rc = runMessageLoop();

    const bool shutdown_complete = shutdown();
    if (rc == SAO_EXIT_OK && !shutdown_complete) {
        return finish(SAO_EXIT_PLATFORM_INIT_FAIL, "shutdown");
    }
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
    const auto acquire_result = ::sao::launcher::acquireSingleInstance(single_instance_mutex_);
    if (acquire_result == SingleInstanceAcquireResult::already_running) {
        // Boot-residency UX: the restart prompt offers "run this program
        // again to cancel the 30-second restart".  A second launch reaches
        // exactly this path, so cancel any pending reboot before the
        // command line is forwarded to the running instance.
        abort_machine_restart();
        // Forward our command line to the running instance before we die.
        forwardCommandLineToRunningInstance(GetCommandLineW());
        return SAO_EXIT_ALREADY_RUNNING;
    }
    if (acquire_result == SingleInstanceAcquireResult::failed)
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    return SAO_EXIT_OK;
}

int App::resolveWorkingDir() {
    if (!::sao::launcher::resolveWorkingDir(state_)) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    // Point the crash handler at <base_dir>/crash/ now that we know it.
    const std::wstring crash_dir = state_.base_dir + L"\\crash";
    if (!ensureDirectoryExists(crash_dir.c_str()))
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    setCrashDumpDirectory(crash_dir.c_str());
    std::wstring active_crash_dir;
    if (!getCrashDumpDirectory(active_crash_dir) ||
        _wcsicmp(active_crash_dir.c_str(), crash_dir.c_str()) != 0) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    sao_launcher_set_base_dir(state_.base_dir.c_str());
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
#if defined(SAO_LAUNCHER_HAS_ANTI_SCREENCAP_API)
    // init_pipeline's composition provider treats enable_anti_screencap as
    // the launcher-level switch; consume it here so the flag is not ignored.
    // Registering every current-process top-level window up front closes the
    // startup gap before the overlay message pump begins its lazy sweep.
    if (cfg.enable_anti_screencap) {
        if (sao_security_anti_screencap_register_process_windows() < 0) {
            (void)sao_security_shutdown();
            return SAO_EXIT_PLATFORM_INIT_FAIL;
        }
    }
#endif
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

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
int App::bringUpSurface() {
    char log_level[32]{};
    sao_platform_config cfg{};
    if (!buildPlatformConfig(state_, cfg, log_level, sizeof(log_level))) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }

    sao_platform_ctx* ctx = nullptr;
    const sao_status_t status = sao_platform_bringup_surface(&cfg, &ctx);
    state_.platform_ctx = ctx;
    if (status != SAO_STATUS_OK || ctx == nullptr) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    return SAO_EXIT_OK;
}

// Bootstraps the driver and engine stages with the Link Start intro on screen.
// The intro parks on the CONNECTED frame until every stage below is done, so
// the helper/driver chain, the plugin runtimes and the plugin engines all run
// strictly underneath the animation.
int App::runBootstrapUnderIntro() {
    char log_level[32]{};
    sao_platform_config cfg{};
    if (!buildPlatformConfig(state_, cfg, log_level, sizeof(log_level))) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    const PluginsProviderConfiguration plugins_configuration =
        launcherProviderConfigurationSnapshot().plugins;
    sao_platform_ctx* platform = static_cast<sao_platform_ctx*>(state_.platform_ctx);
    if (platform == nullptr) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }

    // Smoke/operator/safe runs keep the historical order and never present the
    // overlay surface; only an interactive launch is covered by the intro.
    const bool headless = state_.smoke_mode || state_.rt_io_operator || state_.safe_mode;
    const bool cover_with_intro = !headless && sao_ui_intro_show(platform, 1) == SAO_STATUS_OK;
    bool quit_requested = false;
    const auto release_intro = [&](bool failed) {
        if (cover_with_intro && platform != nullptr)
            (void)sao_ui_intro_release_bootstrap(platform, failed ? 1 : 0);
    };
    const auto advance_intro = [&]() {
        if (!quit_requested)
            quit_requested = !pump_intro_frame(platform);
    };

    // Stage 1 — driver chain, off the owner thread while the intro animates.
    BootstrapStageContext context{};
    context.cfg = &cfg;
    context.platform = platform;
    sao_status_t stage_status = SAO_STATUS_OK;
    if (cover_with_intro) {
        IntroBootstrapWorker worker;
        worker.add(kBootstrapCaptions[0], &bootstrap_step_drivers, &context);
        publish_intro_bootstrap(platform, 0u, false);
        worker.start();
        while (!worker.finished()) {
            if (quit_requested)
                Sleep(1u);
            else
                advance_intro();
        }
        worker.join();
        stage_status = worker.result();
    } else {
        stage_status = sao_platform_bringup_drivers(&cfg, platform);
    }
    if (stage_status != SAO_STATUS_OK) {
        // The driver stage leaves rollback to the owner thread: dismiss the
        // intro here and let shutdown() tear the context down.
        release_intro(true);
        state_.platform_ctx = platform;
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    state_.platform_ctx = platform;

    // Stage 2 — engine surfaces (panels, window-rect registration, capture
    // shield, entity shell).  Owner thread only.
    if (cover_with_intro) {
        publish_intro_bootstrap(platform, 1u, false);
        advance_intro();
    }
    sao_platform_ctx* surface_rolled_back_to = nullptr;
    stage_status = sao_platform_bringup_engines(&cfg, platform, &surface_rolled_back_to);
    if (stage_status != SAO_STATUS_OK) {
        // A clean rollback destroyed the context; only a failed teardown hands
        // it back for shutdown() to retry.
        platform = surface_rolled_back_to;
        release_intro(true);
        state_.platform_ctx = platform;
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    state_.platform_ctx = platform;

    // Stage 3 — capture shield: the anti-screencap chain over every window the
    // process owns, plus the tagWND rcWindow/ExStyle scrub.  Both need the
    // overlay HWNDs, so they follow the engine stage.
    if (cover_with_intro) {
        publish_intro_bootstrap(platform, 2u, false);
        advance_intro();
    }
    stage_status = sao_platform_bringup_capture_shield(&cfg, platform);
    if (stage_status != SAO_STATUS_OK) {
        release_intro(true);
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }

    if (cover_with_intro) {
        publish_intro_bootstrap(platform, 3u, false);
        advance_intro();
    }
    stage_status = sao_platform_bringup_wnd_scrub(&cfg, platform);
    if (stage_status != SAO_STATUS_OK) {
        release_intro(true);
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }

    // Stage 4 — configured plugin runtimes.
    if (cover_with_intro) {
        publish_intro_bootstrap(platform, 4u, false);
        advance_intro();
    }
    if (ensureConfiguredPluginRuntimes(state_, plugins_configuration) != SAO_STATUS_OK) {
        release_intro(true);
        return SAO_EXIT_PLUGIN_LOAD_FAIL;
    }

    // Stage 5 — plugin engines.  In safe mode plugin discovery is skipped.
    if (!state_.safe_mode && plugins_configuration.enabled) {
        if (cover_with_intro) {
            publish_intro_bootstrap(platform, 5u, false);
            advance_intro();
        }
        const int plugin_exit = discoverPlugins();
        if (plugin_exit != SAO_EXIT_OK) {
            release_intro(true);
            return plugin_exit;
        }
    }

    // Late windows (guide host, AI editor, plugin panels) exist by now: repeat
    // the registration sweep so the shield covers them too.
    (void)sao_platform_bringup_capture_sweep(platform);

    // Bootstrap done: release the hold so the CONNECTED → fade tail plays out
    // and the intro completes on the normal end edge.
    release_intro(false);
    return SAO_EXIT_OK;
}
#endif // SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER

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
    state_.ui_online = true;
    if (!user_menu_.create(state_.base_dir.c_str())) {
        return SAO_EXIT_UI_ONLINE_FAIL;
    }
    if (sao_platform_bind_user_menu(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                    &user_menu_) != SAO_STATUS_OK) {
        (void)sao_platform_unbind_user_menu(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                            &user_menu_);
        user_menu_.destroy();
        return SAO_EXIT_UI_ONLINE_FAIL;
    }
    user_menu_.processCommandLine(GetCommandLineW(), false);
    return SAO_EXIT_OK;
}

int App::runRtIoOperator() {
    sao_launcher_rt_io_operator_options_t options{};
    options.struct_size = sizeof(options);
    options.preflight_only = state_.rt_io_preflight_only ? 1u : 0u;
    options.input_checks = state_.rt_io_input_checks ? 1u : 0u;
    options.r5_check = state_.rt_io_r5_check ? 1u : 0u;
    options.mf_check = state_.rt_io_mf_check ? 1u : 0u;
    options.exit_after_validation = state_.rt_io_exit_after_validation ? 1u : 0u;
    options.timeout_ms = 120000u;

    int32_t ready = 0;
    const sao_status_t status =
        sao_launcher_rt_io_operator_run(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                        &options, &rtIoOperatorPrint, nullptr, &ready);
    if (status != SAO_STATUS_OK)
        return SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL;
    if (state_.rt_io_preflight_only)
        return ready == 0 ? SAO_EXIT_OK : SAO_EXIT_RT_IO_OPERATOR_VALIDATION_FAIL;
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
            tickUserGuideWebView();
            status = sao_ui_tick(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                 kUiFrameIntervalMs);
            if (status == SAO_STATUS_OK)
                serviceFirstRunGuide();
            const int32_t restart_result = boot_residency_take_prompt_result();
            if (restart_result == BOOT_RESIDENCY_RESTART_ACCEPTED)
                consolePrintLine("BOOT_RESIDENCY_RESTART_SCHEDULED");
            else if (restart_result == BOOT_RESIDENCY_ERROR)
                consolePrintLine("BOOT_RESIDENCY_RESTART_SCHEDULE_FAILED");
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

void App::serviceFirstRunGuide() noexcept {
    if (!first_run_ || state_.platform_ctx == nullptr)
        return;
    if (!user_guide_open_pending_ && !user_guide_open_accepted_) {
        int32_t finished = 0;
        int32_t reason_value = SAO_UI_LINKSTART_COMPLETION_NONE;
        if (sao_ui_linkstart_poll_finished_ex(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                                              &finished, &reason_value) != SAO_STATUS_OK ||
            finished == 0) {
            return;
        }
        const auto reason = static_cast<SaoUiLinkStartCompletionReason>(reason_value);
        switch (reason) {
        case SAO_UI_LINKSTART_COMPLETION_NATURAL:
        case SAO_UI_LINKSTART_COMPLETION_SKIPPED:
            user_guide_skip_intro_ = true;
            user_guide_open_pending_ = true;
            break;
        case SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED:
        case SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST:
            user_guide_skip_intro_ = false;
            user_guide_open_pending_ = true;
            break;
        case SAO_UI_LINKSTART_COMPLETION_NONE:
        case SAO_UI_LINKSTART_COMPLETION_OFFLINE:
        case SAO_UI_LINKSTART_COMPLETION_TEARDOWN:
        case SAO_UI_LINKSTART_COMPLETION_BOOTSTRAP_FAILED:
            return;
        default:
            return;
        }
    }

    const ULONGLONG now = GetTickCount64();
    if (now < user_guide_next_retry_)
        return;
    user_guide_next_retry_ = now + 1000u;
    if (!user_guide_open_accepted_) {
        if (!openUserDocsIndex(state_.base_dir.c_str(), nullptr, user_guide_skip_intro_))
            return;
        user_guide_open_accepted_ = true;
        user_guide_open_pending_ = false;
    }
    if (!userGuideWebViewReady())
        return;
    if (sao_platform_mark_user_guide_presented(
            static_cast<sao_platform_ctx*>(state_.platform_ctx)) == SAO_STATUS_OK) {
        first_run_ = false;
        user_guide_open_accepted_ = false;
        user_guide_next_retry_ = 0;
    }
}

bool App::stopAutoUpdate() noexcept {
    if (auto_update_cancel_event_ != nullptr) {
        (void)SetEvent(auto_update_cancel_event_);
    }
    if (auto_update_worker_ != nullptr) {
        (void)CancelSynchronousIo(auto_update_worker_);
        const DWORD wait_result = WaitForSingleObject(auto_update_worker_, 5000u);
        if (wait_result == WAIT_OBJECT_0) {
            (void)CloseHandle(auto_update_worker_);
            auto_update_worker_ = nullptr;
            if (auto_update_cancel_event_ != nullptr) {
                (void)CloseHandle(auto_update_cancel_event_);
                auto_update_cancel_event_ = nullptr;
            }
            return true;
        }
        return false;
    } else if (auto_update_cancel_event_ != nullptr) {
        (void)CloseHandle(auto_update_cancel_event_);
        auto_update_cancel_event_ = nullptr;
    }
    return true;
}

bool App::shutdown() noexcept {
    if (shutdown_called_) {
        return stopAutoUpdate();
    }
    const ULONGLONG deadline = GetTickCount64() + 5000u;
    bool quit_pending = false;
    int quit_code = 0;
    for (;;) {
        if (stopAutoUpdate() && boot_residency_close_prompt() && shutdownUserGuideWebView()) {
            if (state_.platform_ctx != nullptr) {
                (void)sao_platform_unbind_user_menu(
                    static_cast<sao_platform_ctx*>(state_.platform_ctx), &user_menu_);
            }
            user_menu_.destroy();
            if (runFullShutdown(state_, security_initialized_)) {
                security_initialized_ = false;
                releaseOwnedSingleInstanceMutex(single_instance_mutex_);
                if (dual_run_driver_acquired_ && dual_run_driver_mutex_) {
                    sao_launcher_dual_run_release_driver_mutex(dual_run_driver_mutex_);
                    dual_run_driver_mutex_ = nullptr;
                    dual_run_driver_acquired_ = false;
                }
                shutdown_called_ = true;
                if (quit_pending) PostQuitMessage(quit_code);
                return true;
            }
        }
        if (GetTickCount64() >= deadline)
            break;
        MSG pending{};
        for (unsigned count = 0; count < 64 && PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (pending.message == WM_QUIT) {
                quit_pending = true;
                quit_code = static_cast<int>(pending.wParam);
                continue;
            }
            TranslateMessage(&pending);
            DispatchMessageW(&pending);
        }
        if (state_.platform_ctx != nullptr) {
            (void)sao_ui_tick(static_cast<sao_platform_ctx*>(state_.platform_ctx),
                              kUiFrameIntervalMs);
        }
        (void)MsgWaitForMultipleObjectsEx(0, nullptr, 2u, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (quit_pending) PostQuitMessage(quit_code);
    return false;
}

} // namespace sao::launcher
