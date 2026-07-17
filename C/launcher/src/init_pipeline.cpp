// SAO Auto — launcher/init_pipeline.cpp
//
// The subsystem entry points declared in init_pipeline.h are provided by
// each subsystem's DLL.  When a subsystem is toggled off at configure time
// (SAO_BUILD_PLATFORM=OFF, etc.) this TU provides link-time fail-closed stubs
// that report SAO_STATUS_NOT_IMPLEMENTED.
//
// The stubs are compiled in ONLY when the corresponding subsystem meta-
// target is not linked.  We detect that via preprocessor defines set by
// the top-level CMake as it walks the enable options.

#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/app.h"
#include "sao/launcher/args.h"
#include "sao/launcher/crash_handler.h"
#include "sao/launcher/single_instance.h"
#include "sao/launcher/working_dir.h"
#include "sao/launcher/shutdown.h"
#include "sao/launcher/dual_run.h"
#include "sao/launcher/provider_config.h"
#include "sao/launcher/user_menu.h"

#include "launcher_lifecycle.h"

#ifdef SAO_STATUS_OK
#undef SAO_STATUS_OK
#endif
#include "settings_owner_internal.h"
#include "settings_theme_internal.h"

#include <windows.h>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <new>

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/core/logging.h"
#endif

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) && \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/rt_io/proxy.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/theme.h"
#endif

#if defined(SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER) && \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "sao_security/anti_debug/wave8.h"
#endif

// ---------------------------------------------------------------------------
// Wave 5 / Phase 1 — headless init pipeline entry point.
//
// Kept in the same TU as the fail-closed subsystem stubs so unit tests can
// link a single file and inject only the providers under test.
//
// ``SaoLauncherBaseDir`` mirrors Python's ``config.BASE_DIR`` and is
// populated right after ``resolveWorkingDir`` succeeds so any subsystem
// that needs the resolved path can read it without threading state
// through every C ABI boundary.
// ---------------------------------------------------------------------------

extern "C" wchar_t SaoLauncherBaseDir[260] = { 0 };

namespace sao::launcher {

bool buildPlatformConfig(const AppState& state,
                         sao_platform_config& config,
                         char* log_level_storage,
                         std::size_t log_level_capacity) noexcept {
    if (!log_level_storage || log_level_capacity == 0) return false;

    const wchar_t* source = state.log_level[0] ? state.log_level : L"info";
    const int converted = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, log_level_storage,
        static_cast<int>(log_level_capacity), nullptr, nullptr);
    if (converted <= 0) {
        log_level_storage[0] = '\0';
        return false;
    }

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    int32_t core_log_level = SAO_LOG_INFO;
    if (_stricmp(log_level_storage, "trace") == 0) {
        core_log_level = SAO_LOG_TRACE;
    } else if (_stricmp(log_level_storage, "debug") == 0) {
        core_log_level = SAO_LOG_DEBUG;
    } else if (_stricmp(log_level_storage, "info") == 0) {
        core_log_level = SAO_LOG_INFO;
    } else if (_stricmp(log_level_storage, "warn") == 0) {
        core_log_level = SAO_LOG_WARN;
    } else if (_stricmp(log_level_storage, "error") == 0) {
        core_log_level = SAO_LOG_ERROR;
    } else if (_stricmp(log_level_storage, "critical") == 0) {
        core_log_level = SAO_LOG_FATAL;
    } else {
        return false;
    }
    if (sao_core_set_log_level(core_log_level) != SAO_STATUS_OK) {
        return false;
    }
#else
    return false;
#endif

    config = {};
    config.base_dir = state.base_dir;
    config.config_path = state.config_path[0] ? state.config_path : nullptr;
    config.log_level = log_level_storage;
    config.safe_mode = state.safe_mode ? 1 : 0;
    return true;
}

} // namespace sao::launcher

namespace {

constexpr UINT_PTR kUiFrameTimerId = 0x53415549U;
constexpr int32_t kUiFrameIntervalMs = 16;
constexpr int32_t kHomeHotkeyId = 0x5341;
constexpr int32_t kInsertHotkeyId = 0x5342;

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_launcher_composition_test_hooks_t g_composition_test_hooks{};
#endif

// Notify the optional hook that we entered a teardown step.  Silent when
// hooks or the callback is null.
void notifyStep(const sao_launcher_init_hooks_t* hooks, const char* name) {
    if (hooks && hooks->on_teardown_step) {
        hooks->on_teardown_step(name, hooks->user_data);
    }
}

// Run the fail-closed subsystem sequence.  Returns the exit code the
// launcher would emit; ``rc_out`` receives the sao_status_t equivalent
// so tests can distinguish "pipeline never reached this step" from a
// provider failure.
int runPipeline(const sao_launcher_init_hooks_t* hooks,
                sao::launcher::AppState& state,
                sao_dual_run_config& dual_cfg,
                HANDLE& single_instance_mutex,
                HANDLE& dual_run_driver_mutex,
                bool& crash_installed,
                bool& mutex_acquired,
                bool& dual_run_driver_acquired,
                bool& base_dir_resolved,
                bool& license_verified,
                bool& shell_verified,
                bool& security_initialized,
                bool& platform_up,
                bool& plugins_discovered,
                bool& ui_online,
                bool& handed_off_to_python,
                int& handed_off_exit_code) {
    using namespace sao::launcher;

    // ------------------------------------------------------------------
    // Step 0 (Phase 12) — read dual_run.json + decide dispatch.  When the
    // config picks python_only we spawn Python and hand off; the launcher
    // exits with SAO_EXIT_HANDOFF_TO_PYTHON.  Side-by-side spawns Python
    // and keeps going with the CPP pipeline.  cpp_preferred_python_fallback
    // remembers to try the fallback on any later fatal.
    //
    // If the launcher was itself spawned by a parent dual-run driver
    // (SAO_DUAL_RUN_ROLE env var is set) we skip all of this dispatch —
    // the parent already decided; the child just runs the CPP pipeline.
    // ------------------------------------------------------------------
    // Grab the driver mutex once.  Only the "top-level" dual-run driver
    // (not a child that inherited SAO_DUAL_RUN_ROLE) needs to hold it.
    wchar_t inherited_role[64]{};
    const bool is_child_of_dual_run_driver =
        (GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME,
                                  inherited_role, 64) > 0)
        && inherited_role[0] != L'\0';

    if (!is_child_of_dual_run_driver) {
        sao_status_t ms = sao_launcher_dual_run_acquire_driver_mutex(&dual_run_driver_mutex);
        if (ms == SAO_LAUNCHER_ALREADY_RUNNING) {
            // Another dual-run driver owns the mutex; fall back to being
            // just a plain CPP instance for this run (the per-exe
            // single_instance mutex will catch true duplicates).
        } else if (ms == SAO_STATUS_OK) {
            dual_run_driver_acquired = true;
        }
    }

    int32_t should_continue = 1;
    int32_t handoff_exit_code = 0;
    sao_status_t zs = sao_launcher_dual_run_step_zero(&dual_cfg,
                                                       &should_continue,
                                                       &handoff_exit_code);
    if (zs == SAO_LAUNCHER_PYTHON_UNAVAILABLE) {
        // python_only requested but no Python — return a specific error.
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    if (zs == SAO_LAUNCHER_SPAWN_FAILED) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    if (zs != SAO_STATUS_OK) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    if (should_continue == 0) {
        handed_off_to_python = true;
        handed_off_exit_code = handoff_exit_code;
        return handoff_exit_code;
    }

    // Step 1 — crash handler.  Non-fatal on install failure so we still
    // hit the message loop; a runtime crash without a minidump is worse
    // than continuing without one.
    crash_installed = installCrashHandler();

    // Step 2 — single-instance mutex.
    if (!acquireSingleInstance(single_instance_mutex)) {
        return SAO_EXIT_ALREADY_RUNNING;
    }
    mutex_acquired = true;

    // Step 3 — resolve base_dir.  Failure is a fatal init error.
    if (!resolveWorkingDir(state)) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    base_dir_resolved = true;

    // Publish base_dir globally (Wave 5 addition).  A defensive copy so
    // downstream callers don't accidentally hold a pointer into AppState.
    lstrcpynW(SaoLauncherBaseDir, state.base_dir, 260);

    // Prep the crash directory now that we know where to write dumps.
    wchar_t crash_dir[MAX_PATH]{};
    lstrcpynW(crash_dir, state.base_dir, MAX_PATH);
    lstrcatW(crash_dir, L"\\crash");
    ensureDirectoryExists(crash_dir);
    setCrashDumpDirectory(crash_dir);

    // Step 4 — load settings.json (optional, non-fatal).  The launcher
    // itself doesn't consume the values yet; loading the file here
    // proves the plumbing works and gives the platform subsystem a
    // predictable place to pull from.  With no explicit --config all
    // optional providers remain disabled.  An explicit malformed config is
    // fatal rather than partially enabling a subsystem.
    if (loadLauncherProviderConfiguration(
            state.base_dir,
            state.config_path[0] ? state.config_path : nullptr) != SAO_STATUS_OK) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    const auto provider_configuration = launcherProviderConfigurationSnapshot();

    // Step 5 — license verify.  The provider is optional until explicitly
    // enabled; once enabled, provider absence and invalid licenses are fatal.
    if (!state.no_license && provider_configuration.license.enabled) {
        sao_license_result r{};
        sao_status_t s = sao_license_verify(&r);
        state.license_active = s != SAO_STATUS_NOT_IMPLEMENTED;
        if (s != SAO_STATUS_OK || !r.valid) {
            return SAO_EXIT_LICENSE_INVALID;
        } else {
            license_verified = true;
        }
    }

    // Step 6 — shell integrity.  Explicit enablement makes every failure fatal.
    if (provider_configuration.shell.enabled) {
        sao_shell_verify_result r{};
        sao_status_t s = sao_shell_verify_integrity(&r);
        state.shell_active = s != SAO_STATUS_NOT_IMPLEMENTED;
        if (s != SAO_STATUS_OK || r.tampered) {
            return SAO_EXIT_SHELL_TAMPERED;
        } else {
            shell_verified = true;
        }
    }

    // Step 7 — platform bring-up.
    {
        sao_security_config cfg{};
        cfg.enable_anti_debug = 1;
        cfg.enable_anti_dump = 1;
        cfg.enable_anti_screencap = 1;
        cfg.enable_obfuscation_runtime = 1;
        if (sao_security_init(&cfg) != SAO_STATUS_OK) {
            return SAO_EXIT_PLATFORM_INIT_FAIL;
        }
        security_initialized = true;
    }

    // Step 8 — platform bring-up.
    {
        char log_level_narrow[32]{};
        sao_platform_config cfg{};
        if (!buildPlatformConfig(state, cfg, log_level_narrow,
                                 sizeof(log_level_narrow))) {
            return SAO_EXIT_PLATFORM_INIT_FAIL;
        }

        sao_platform_ctx* ctx = nullptr;
        sao_status_t s = sao_platform_bringup(&cfg, &ctx);
        state.platform_ctx = ctx;
        platform_up = ctx != nullptr;
        if (s != SAO_STATUS_OK || !ctx) {
            return SAO_EXIT_PLATFORM_INIT_FAIL;
        }
    }

    // Step 8 — plugin discovery (skipped in safe mode).
    if (!state.safe_mode && provider_configuration.plugins.enabled) {
        sao_plugins_registry* reg = nullptr;
        sao_status_t s = sao_plugins_discover(
            static_cast<sao_platform_ctx*>(state.platform_ctx), &reg);
        if (s != SAO_STATUS_OK || reg == nullptr) {
            return SAO_EXIT_PLUGIN_LOAD_FAIL;
        } else {
            state.plugins_registry = reg;
            plugins_discovered = true;
            if (sao_plugins_activate_autostart(reg) != SAO_STATUS_OK) {
                return SAO_EXIT_PLUGIN_LOAD_FAIL;
            }
        }
    }

    // Step 9 — UI online.
    {
        sao_status_t s = sao_ui_bring_online(
            static_cast<sao_platform_ctx*>(state.platform_ctx));
        if (s != SAO_STATUS_OK) {
            return SAO_EXIT_UI_ONLINE_FAIL;
        }
        ui_online = true;
    }

    // Step 10 — pump the message loop until the hook (or WM_QUIT) tells
    // us to exit.  Tests supply a hook that returns non-zero on the
    // first call so the pipeline exits immediately and we can observe
    // teardown ordering.
    if (hooks && hooks->poll_should_exit) {
        while (true) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return SAO_EXIT_OK;
                int32_t handled = 0;
                if (sao_ui_handle_message(
                        static_cast<sao_platform_ctx*>(state.platform_ctx),
                        msg.message, msg.wParam, msg.lParam,
                        &handled) != SAO_STATUS_OK) {
                    return SAO_EXIT_UI_ONLINE_FAIL;
                }
                if (!handled) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if (sao_ui_tick(
                    static_cast<sao_platform_ctx*>(state.platform_ctx),
                    kUiFrameIntervalMs) != SAO_STATUS_OK) {
                return SAO_EXIT_UI_ONLINE_FAIL;
            }
            if (hooks->poll_should_exit(hooks->user_data) != 0) break;
            Sleep(0);
        }
    } else {
        if (SetTimer(nullptr, kUiFrameTimerId, kUiFrameIntervalMs, nullptr) ==
            0) {
            return SAO_EXIT_UI_ONLINE_FAIL;
        }
        MSG msg{};
        BOOL result = 0;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            int32_t handled = 0;
            if (sao_ui_handle_message(
                    static_cast<sao_platform_ctx*>(state.platform_ctx),
                    msg.message, msg.wParam, msg.lParam,
                    &handled) != SAO_STATUS_OK) {
                KillTimer(nullptr, kUiFrameTimerId);
                return SAO_EXIT_UI_ONLINE_FAIL;
            }
            if (!handled) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        KillTimer(nullptr, kUiFrameTimerId);
        if (result < 0) return SAO_EXIT_UI_ONLINE_FAIL;
    }

    return SAO_EXIT_OK;
}

// Reverse-order teardown.  Never throws; ``on_teardown_step`` is fired
// for each step actually rolled back so tests can assert the ordering.
void teardown(const sao_launcher_init_hooks_t* hooks,
              sao::launcher::AppState& state,
              HANDLE& single_instance_mutex,
              bool crash_installed,
              bool mutex_acquired,
              bool /*base_dir_resolved*/,
              bool /*license_verified*/,
              bool /*shell_verified*/,
              bool security_initialized,
              bool platform_up,
              bool plugins_discovered,
              bool ui_online) noexcept {
    using namespace sao::launcher;

    // Reverse of init order: UI -> plugins -> rt_io/platform -> security ->
    // shell -> license -> single_instance -> crash_handler.
    if (ui_online) {
        (void)sao_ui_take_offline(static_cast<sao_platform_ctx*>(state.platform_ctx));
        notifyStep(hooks, "ui");
    }
    if (plugins_discovered) {
        (void)sao_plugins_shutdown(static_cast<sao_plugins_registry*>(state.plugins_registry));
        state.plugins_registry = nullptr;
        notifyStep(hooks, "plugins");
    }
    if (platform_up) {
        if (sao_platform_teardown(
                static_cast<sao_platform_ctx*>(state.platform_ctx)) ==
            SAO_STATUS_OK) {
            state.platform_ctx = nullptr;
        }
        notifyStep(hooks, "platform");
    }
    if (security_initialized) {
        (void)sao_security_shutdown();
        notifyStep(hooks, "security");
    }
    if (state.shell_active) {
        (void)sao_shell_shutdown();
        state.shell_active = false;
        notifyStep(hooks, "shell");
    }
    if (state.license_active) {
        (void)sao_license_shutdown();
        state.license_active = false;
        notifyStep(hooks, "license");
    }
    if (mutex_acquired && single_instance_mutex) {
        releaseSingleInstance(single_instance_mutex);
        single_instance_mutex = nullptr;
        notifyStep(hooks, "single_instance");
    }
    if (crash_installed) {
        uninstallCrashHandler();
        notifyStep(hooks, "crash_handler");
    }
}

}  // namespace

extern "C" sao_status_t sao_launcher_init_pipeline_run(
    int argc, wchar_t** argv,
    const sao_launcher_init_hooks_t* hooks,
    int* exit_code_out) {
    using namespace sao::launcher;

    if (exit_code_out) *exit_code_out = SAO_EXIT_OK;

    AppState state{};

    // Command line first — a --help / --version can short-circuit.
    if (argc > 0 && argv != nullptr) {
        bool should_exit = false;
        int rc = SAO_EXIT_OK;
        if (!parseCommandLineFromArgv(argc, argv, state, should_exit, rc)) {
            if (exit_code_out) *exit_code_out = SAO_EXIT_BAD_ARGS;
            return SAO_STATUS_INVALID_ARGUMENT;
        }
        if (should_exit) {
            if (exit_code_out) *exit_code_out = rc;
            return SAO_STATUS_OK;
        }
    }

    LauncherLifecycleDecision lifecycle;
    if (prepareLauncherLifecycle(lifecycle) != SAO_STATUS_OK) {
        if (exit_code_out) *exit_code_out = SAO_EXIT_PLATFORM_INIT_FAIL;
        return SAO_STATUS_INTERNAL;
    }

    HANDLE single_instance_mutex = nullptr;
    HANDLE dual_run_driver_mutex = nullptr;
    bool crash_installed = false;
    bool mutex_acquired = false;
    bool dual_run_driver_acquired = false;
    bool base_dir_resolved = false;
    bool license_verified = false;
    bool shell_verified = false;
    bool security_initialized = false;
    bool platform_up = false;
    bool plugins_discovered = false;
    bool ui_online = false;
    bool handed_off_to_python = false;
    int  handed_off_exit_code = 0;

    int rc = runPipeline(hooks, state, lifecycle.dual_config,
                          single_instance_mutex, dual_run_driver_mutex,
                          crash_installed, mutex_acquired, dual_run_driver_acquired,
                          base_dir_resolved, license_verified,
                          shell_verified, security_initialized, platform_up,
                          plugins_discovered, ui_online,
                          handed_off_to_python, handed_off_exit_code);
    const int rollout_result = rc;

    // CPP_PREFERRED_PYTHON_FALLBACK — if any CPP step failed AFTER step-zero
    // let us know it wanted CPP, retry via Python.  step-zero itself didn't
    // spawn Python; we do that lazily here so the fallback is only paid for
    // when the CPP path actually fails.
    wchar_t inherited_role[64]{};
    const bool is_child_of_dual_run_driver =
        GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME,
                                inherited_role, 64) > 0 &&
        inherited_role[0] != L'\0';
    if (!is_child_of_dual_run_driver
        && !handed_off_to_python
        && rc != SAO_EXIT_OK
        && rc != SAO_EXIT_ALREADY_RUNNING
        && rc != SAO_EXIT_BAD_ARGS) {
        if (lifecycle.dual_config.mode ==
            SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK) {
            int32_t fbec = 0;
            const wchar_t* step_name = L"cpp_pipeline";
            if (!platform_up)                step_name = L"platform_bringup";
            else if (!plugins_discovered && !state.safe_mode) step_name = L"plugins_discover";
            else if (!ui_online)             step_name = L"ui_bring_online";
            if (sao_launcher_dual_run_maybe_fallback_to_python(
                    &lifecycle.dual_config, rc,
                    step_name, &fbec)) {
                handed_off_to_python = true;
                handed_off_exit_code = fbec;
                rc = fbec;
            }
        }
    }

    teardown(hooks, state, single_instance_mutex,
             crash_installed, mutex_acquired,
             base_dir_resolved, license_verified,
             shell_verified, security_initialized, platform_up,
             plugins_discovered, ui_online);

    if (dual_run_driver_acquired && dual_run_driver_mutex) {
        sao_launcher_dual_run_release_driver_mutex(dual_run_driver_mutex);
    }

    const char* failure_hint = "init_pipeline";
    if (!base_dir_resolved) failure_hint = "working_dir";
    else if (!security_initialized) failure_hint = "security_init";
    else if (!platform_up) failure_hint = "platform_bringup";
    else if (!plugins_discovered && !state.safe_mode &&
             launcherProviderConfigurationSnapshot().plugins.enabled) {
        failure_hint = "plugins_discover";
    } else if (!ui_online) failure_hint = "ui_bring_online";
    completeLauncherLifecycle(lifecycle, rollout_result, failure_hint);

    if (exit_code_out) *exit_code_out = rc;
    if (handed_off_to_python) {
        return SAO_STATUS_OK;
    }
    return rc == SAO_EXIT_OK ? SAO_STATUS_OK
                              : SAO_STATUS_INTERNAL;
}

extern "C" void sao_launcher_set_composition_test_hooks(
    const sao_launcher_composition_test_hooks_t* hooks) {
#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
    g_composition_test_hooks = hooks ? *hooks : sao_launcher_composition_test_hooks_t{};
#else
    (void)hooks;
#endif
}

extern "C" {

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_platform_bringup(const sao_platform_config* cfg,
                                  sao_platform_ctx** ctx_out) {
    if (!g_composition_test_hooks.platform_bringup) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.platform_bringup(
        cfg, ctx_out, g_composition_test_hooks.user_data);
}
sao_status_t sao_platform_teardown(sao_platform_ctx* ctx) {
    if (!g_composition_test_hooks.platform_teardown) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.platform_teardown(
        ctx, g_composition_test_hooks.user_data);
}
sao_status_t sao_ui_bring_online(sao_platform_ctx* ctx) {
    if (!g_composition_test_hooks.ui_bring_online) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.ui_bring_online(
        ctx, g_composition_test_hooks.user_data);
}
sao_status_t sao_ui_take_offline(sao_platform_ctx* ctx) {
    if (!g_composition_test_hooks.ui_take_offline) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.ui_take_offline(
        ctx, g_composition_test_hooks.user_data);
}
sao_status_t sao_ui_tick(sao_platform_ctx* ctx, uint32_t elapsed_ms) {
    return g_composition_test_hooks.ui_tick
        ? g_composition_test_hooks.ui_tick(
              ctx, elapsed_ms, g_composition_test_hooks.user_data)
        : SAO_STATUS_OK;
}
sao_status_t sao_ui_handle_message(sao_platform_ctx* ctx, uint32_t message,
                                   uintptr_t w_param, intptr_t l_param,
                                   int32_t* out_handled) {
    if (out_handled) *out_handled = 0;
    return g_composition_test_hooks.ui_handle_message
        ? g_composition_test_hooks.ui_handle_message(
              ctx, message, w_param, l_param, out_handled,
              g_composition_test_hooks.user_data)
        : SAO_STATUS_OK;
}
#elif defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
struct sao_platform_ctx {
    sao_rt_io_proxy_handle_t rt_io_proxy;
    sao_ui_overlay_host_handle_t overlay_host;
    sao_ui_entity_shell_handle_t entity_shell;
    bool home_hotkey_registered;
    bool insert_hotkey_registered;
    SaoUiThemeId previous_theme = SAO_UI_THEME_DARK;
    bool restore_theme_on_rollback = false;
    bool settings_save_enabled = false;
    std::unique_ptr<sao::launcher::settings_owner::SettingsOwner> settings_owner;
};

sao_status_t create_settings_owner(
    const wchar_t* base_dir,
    std::unique_ptr<sao::launcher::settings_owner::SettingsOwner>& out) noexcept {
    try {
        const auto settings_path =
            std::filesystem::path(base_dir) / L"settings.json";
        return sao::launcher::settings_owner::SettingsOwner::create(
            settings_path.wstring(), out);
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

SaoUiThemeId runtime_theme_id(
    sao::launcher::settings_theme::PanelTheme theme) noexcept {
    return theme == sao::launcher::settings_theme::PanelTheme::light
        ? SAO_UI_THEME_LIGHT
        : SAO_UI_THEME_DARK;
}

sao_status_t teardown_platform_context(sao_platform_ctx* ctx,
                                       bool save_settings) noexcept;

sao_status_t rollback_platform_bringup(sao_platform_ctx* ctx,
                                        sao_platform_ctx** ctx_out,
                                        sao_status_t failure_status) noexcept {
    const SaoUiThemeId previous_theme = ctx->previous_theme;
    const bool restore_theme = ctx->restore_theme_on_rollback;
    const sao_status_t teardown_status = teardown_platform_context(ctx, false);
    if (teardown_status != SAO_STATUS_OK) {
        *ctx_out = ctx;
    }
    if (restore_theme) {
        (void)sao_ui_theme_set_active_id(previous_theme);
    }
    return failure_status;
}

bool SAO_UI_CALL entity_hit_test(int32_t x, int32_t y, void* user_data) {
    bool hit = false;
    return sao_ui_entity_shell_hit_test(
               static_cast<sao_ui_entity_shell_handle_t>(user_data), x, y,
               &hit) == SAO_STATUS_OK &&
           hit;
}

void SAO_UI_CALL entity_mouse(uint32_t message, int32_t x, int32_t y,
                              int32_t button, int32_t wheel_delta,
                              void* user_data) {
    (void)sao_ui_entity_shell_handle_mouse(
        static_cast<sao_ui_entity_shell_handle_t>(user_data), message, x, y,
        button, wheel_delta);
}

sao_status_t SAO_UI_CALL entity_action(SaoUiEntityAction action,
                                       void* user_data) {
    switch (action) {
    case SAO_UI_ENTITY_ACTION_OPEN_ABOUT:
        break;
    case SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST:
    case SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR:
    case SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE:
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL:
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE:
    case SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR:
    case SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP:
    case SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR:
    case SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER:
    case SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS:
    case SAO_UI_ENTITY_ACTION_PLUGIN_STATUS:
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS: {
        auto* ctx = static_cast<sao_platform_ctx*>(user_data);
        if (ctx == nullptr || !ctx->settings_owner) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        return ctx->settings_owner->save();
    }
    case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
    case SAO_UI_ENTITY_ACTION_SET_ALL_DARK: {
        auto* ctx = static_cast<sao_platform_ctx*>(user_data);
        if (ctx == nullptr || !ctx->settings_owner) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const auto theme = action == SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT
            ? sao::launcher::settings_theme::PanelTheme::light
            : sao::launcher::settings_theme::PanelTheme::dark;
        const sao_status_t status =
            sao::launcher::settings_theme::replace_all_panel_themes(
                *ctx->settings_owner, theme);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        return sao_ui_theme_set_active_id(runtime_theme_id(theme));
    }
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    HWND owner = ctx == nullptr || ctx->overlay_host == nullptr
        ? nullptr
        : static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    if (sao::launcher::openUserDocsIndex(SaoLauncherBaseDir, owner)) {
        return SAO_STATUS_OK;
    }
    MessageBoxW(owner, L"用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。",
                L"SAO Auto", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    return SAO_STATUS_ERR_NOT_FOUND;
}

sao_status_t sao_platform_bringup(const sao_platform_config* cfg,
                                  sao_platform_ctx** ctx_out) {
    if (!cfg || !ctx_out || !cfg->base_dir) return SAO_STATUS_INVALID_ARGUMENT;
    *ctx_out = nullptr;

    (void)SetEnvironmentVariableW(L"SAO_BASE_DIR", cfg->base_dir);
    auto* ctx = new (std::nothrow) sao_platform_ctx{};
    if (!ctx) return SAO_STATUS_INTERNAL;

    sao_status_t status = create_settings_owner(cfg->base_dir, ctx->settings_owner);
    if (status != SAO_STATUS_OK) {
        delete ctx;
        return status;
    }
    sao::launcher::settings_owner::LoadInfo load_info{};
    status = ctx->settings_owner->load(load_info);
    if (status != SAO_STATUS_OK) {
        delete ctx;
        return status;
    }
    sao::launcher::settings_theme::PanelTheme restored_theme{};
    status = sao::launcher::settings_theme::read_process_theme(
        *ctx->settings_owner, restored_theme);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    status = sao_ui_theme_get_active_id(&ctx->previous_theme);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    ctx->restore_theme_on_rollback = true;
    status = sao_ui_theme_set_active_id(runtime_theme_id(restored_theme));
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }

    SaoRtIoProxyConfig rt_io_cfg{};
    rt_io_cfg.session_name_utf8 = "launcher";
    rt_io_cfg.strict_bootstrap = 1;
    status = sao_rt_io_proxy_open(&rt_io_cfg, &ctx->rt_io_proxy);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }

    SaoOverlayHostConfig overlay_cfg{};
    status = sao_ui_overlay_host_create(&overlay_cfg, &ctx->overlay_host);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }

    SaoUiEntityShellConfig entity_cfg{};
    entity_cfg.action_fn = &entity_action;
    entity_cfg.action_user_data = ctx;
    status = sao_ui_entity_shell_create(
        ctx->overlay_host, &entity_cfg, &ctx->entity_shell);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    status = sao_ui_overlay_host_set_hit_test(
        ctx->overlay_host, &entity_hit_test, ctx->entity_shell);
    if (status == SAO_STATUS_OK) {
        status = sao_ui_overlay_host_set_mouse(
            ctx->overlay_host, &entity_mouse, ctx->entity_shell);
    }
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }

    ctx->restore_theme_on_rollback = false;
    ctx->settings_save_enabled = true;
    *ctx_out = ctx;
    return SAO_STATUS_OK;
}

sao_status_t teardown_platform_context(sao_platform_ctx* ctx,
                                       bool save_settings) noexcept {
    if (!ctx) return SAO_STATUS_INVALID_ARGUMENT;
    if (ctx->insert_hotkey_registered) {
        (void)UnregisterHotKey(nullptr, kInsertHotkeyId);
        ctx->insert_hotkey_registered = false;
    }
    if (ctx->home_hotkey_registered) {
        (void)UnregisterHotKey(nullptr, kHomeHotkeyId);
        ctx->home_hotkey_registered = false;
    }
    if (ctx->entity_shell) {
        sao_ui_entity_shell_destroy(ctx->entity_shell);
        ctx->entity_shell = nullptr;
    }
    if (ctx->overlay_host) {
        if (!sao_ui_overlay_host_destroy(ctx->overlay_host)) {
            return SAO_STATUS_INTERNAL;
        }
        ctx->overlay_host = nullptr;
    }
    if (ctx->rt_io_proxy) {
        const sao_status_t proxy_status = sao_rt_io_proxy_close(ctx->rt_io_proxy);
        if (proxy_status != SAO_STATUS_OK) {
            return proxy_status;
        }
        ctx->rt_io_proxy = nullptr;
    }
    if (save_settings) {
        if (!ctx->settings_owner) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const sao_status_t settings_status = ctx->settings_owner->save();
        if (settings_status != SAO_STATUS_OK) {
            return settings_status;
        }
    }
    delete ctx;
    return SAO_STATUS_OK;
}

sao_status_t sao_platform_teardown(sao_platform_ctx* ctx) {
    if (!ctx) return SAO_STATUS_INVALID_ARGUMENT;
    return teardown_platform_context(ctx, ctx->settings_save_enabled);
}

sao_status_t sao_ui_bring_online(sao_platform_ctx* ctx) {
    if (!ctx || !ctx->overlay_host || !ctx->entity_shell) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    sao_status_t status = sao_ui_entity_shell_bring_online(ctx->entity_shell);
    if (status != SAO_STATUS_OK) return status;
    if (!RegisterHotKey(nullptr, kHomeHotkeyId, MOD_NOREPEAT, VK_HOME)) {
        (void)sao_ui_entity_shell_take_offline(ctx->entity_shell);
        return SAO_STATUS_INTERNAL;
    }
    ctx->home_hotkey_registered = true;
    if (!RegisterHotKey(nullptr, kInsertHotkeyId, MOD_NOREPEAT, VK_INSERT)) {
        UnregisterHotKey(nullptr, kHomeHotkeyId);
        ctx->home_hotkey_registered = false;
        (void)sao_ui_entity_shell_take_offline(ctx->entity_shell);
        return SAO_STATUS_INTERNAL;
    }
    ctx->insert_hotkey_registered = true;
    return SAO_STATUS_OK;
}

sao_status_t sao_ui_take_offline(sao_platform_ctx* ctx) {
    if (!ctx || !ctx->overlay_host || !ctx->entity_shell) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    sao_status_t status = SAO_STATUS_OK;
    if (ctx->insert_hotkey_registered) {
        if (!UnregisterHotKey(nullptr, kInsertHotkeyId)) {
            status = SAO_STATUS_INTERNAL;
        } else {
            ctx->insert_hotkey_registered = false;
        }
    }
    if (ctx->home_hotkey_registered) {
        if (!UnregisterHotKey(nullptr, kHomeHotkeyId)) {
            status = SAO_STATUS_INTERNAL;
        } else {
            ctx->home_hotkey_registered = false;
        }
    }
    const sao_status_t offline_status =
        sao_ui_entity_shell_take_offline(ctx->entity_shell);
    return status == SAO_STATUS_OK ? offline_status : status;
}

sao_status_t sao_ui_tick(sao_platform_ctx* ctx, uint32_t elapsed_ms) {
    if (!ctx || !ctx->entity_shell) return SAO_STATUS_INVALID_ARGUMENT;
    return sao_ui_entity_shell_tick(ctx->entity_shell, elapsed_ms);
}

sao_status_t sao_ui_handle_message(sao_platform_ctx* ctx, uint32_t message,
                                   uintptr_t w_param, intptr_t,
                                   int32_t* out_handled) {
    if (!ctx || !ctx->entity_shell || !out_handled) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    *out_handled = 0;
    if (message == WM_TIMER && w_param == kUiFrameTimerId) {
        *out_handled = 1;
        return sao_ui_tick(ctx, kUiFrameIntervalMs);
    }
    if (message != WM_HOTKEY) return SAO_STATUS_OK;
    if (w_param == kHomeHotkeyId) {
        *out_handled = 1;
        return sao_ui_entity_shell_home(ctx->entity_shell);
    }
    if (w_param == kInsertHotkeyId) {
        *out_handled = 1;
        return sao_ui_entity_shell_insert(ctx->entity_shell);
    }
    return SAO_STATUS_OK;
}
#else
sao_status_t sao_platform_bringup(const sao_platform_config*, sao_platform_ctx**) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_teardown(sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_bring_online(sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_take_offline(sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_tick(sao_platform_ctx*, uint32_t) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_handle_message(sao_platform_ctx*, uint32_t, uintptr_t,
                                   intptr_t, int32_t* out_handled) {
    if (out_handled) *out_handled = 0;
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_security_init(const sao_security_config* cfg) {
    if (!g_composition_test_hooks.security_init) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.security_init(
        cfg, g_composition_test_hooks.user_data);
}
sao_status_t sao_security_shutdown(void) {
    if (g_composition_test_hooks.security_shutdown) {
        g_composition_test_hooks.security_shutdown(
            g_composition_test_hooks.user_data);
    }
    return SAO_STATUS_OK;
}
#elif defined(SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER)
sao_status_t sao_security_init(const sao_security_config* cfg) {
    if (!cfg) return SAO_STATUS_INVALID_ARGUMENT;
    if (!cfg->enable_anti_debug) return SAO_STATUS_OK;

    uint32_t score = 0;
    sao_status_t status = sao_security_anti_debug_wave8_scan_all(&score);
    if (status != SAO_STATUS_OK) return status;
    return score == 0 ? SAO_STATUS_OK : SAO_STATUS_PLATFORM_INIT_FAIL;
}
sao_status_t sao_security_shutdown(void) {
    return SAO_STATUS_OK;
}
#else
sao_status_t sao_security_init(const sao_security_config*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_security_shutdown(void) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_shell_verify_integrity(sao_shell_verify_result* out) {
    if (!g_composition_test_hooks.shell_verify) return SAO_STATUS_NOT_IMPLEMENTED;
    return g_composition_test_hooks.shell_verify(
        out, g_composition_test_hooks.user_data);
}
sao_status_t sao_shell_shutdown(void) {
    return g_composition_test_hooks.shell_shutdown
        ? g_composition_test_hooks.shell_shutdown(g_composition_test_hooks.user_data)
        : SAO_STATUS_OK;
}
#elif !defined(SAO_LINKED_SHELL)
sao_status_t sao_shell_verify_integrity(sao_shell_verify_result* out) {
    if (out) {
        out->tampered = 1;
        strncpy_s(out->reason, sizeof(out->reason),
                  "shell integrity provider unavailable", _TRUNCATE);
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_shell_shutdown(void) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_license_verify(sao_license_result* out) {
    if (!g_composition_test_hooks.license_verify) return SAO_STATUS_NOT_IMPLEMENTED;
    return g_composition_test_hooks.license_verify(
        out, g_composition_test_hooks.user_data);
}
sao_status_t sao_license_shutdown(void) {
    return g_composition_test_hooks.license_shutdown
        ? g_composition_test_hooks.license_shutdown(g_composition_test_hooks.user_data)
        : SAO_STATUS_OK;
}
#elif !defined(SAO_LINKED_LICENSE)
sao_status_t sao_license_verify(sao_license_result* out) {
    if (out) {
        out->valid = 0;
        out->expires_utc = 0;
        out->tier[0] = '\0';
        out->hwid_hash[0] = '\0';
        strncpy_s(out->error_msg, sizeof(out->error_msg),
                  "license provider unavailable", _TRUNCATE);
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_license_shutdown(void) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_plugins_discover(sao_platform_ctx* ctx,
                                  sao_plugins_registry** out) {
    if (!g_composition_test_hooks.plugins_discover) {
        if (out) *out = nullptr;
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.plugins_discover(
        ctx, out, g_composition_test_hooks.user_data);
}
sao_status_t sao_plugins_activate_autostart(sao_plugins_registry* registry) {
    return g_composition_test_hooks.plugins_activate_autostart
        ? g_composition_test_hooks.plugins_activate_autostart(
              registry, g_composition_test_hooks.user_data)
        : SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_shutdown(sao_plugins_registry* registry) {
    return g_composition_test_hooks.plugins_shutdown
        ? g_composition_test_hooks.plugins_shutdown(
              registry, g_composition_test_hooks.user_data)
        : SAO_STATUS_NOT_IMPLEMENTED;
}
#elif !defined(SAO_LINKED_PLUGINS)
sao_status_t sao_plugins_discover(sao_platform_ctx*, sao_plugins_registry** out) {
    if (out) *out = nullptr;
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_activate_autostart(sao_plugins_registry*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_shutdown(sao_plugins_registry*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

} // extern "C"
