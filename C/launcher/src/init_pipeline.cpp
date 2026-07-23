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
#include "sao/launcher/dual_run.h"
#include "sao/launcher/provider_config.h"
#include "sao/launcher/shutdown.h"
#include "sao/launcher/single_instance.h"
#include "sao/launcher/user_menu.h"
#include "sao/launcher/working_dir.h"

#include "launcher_lifecycle.h"

#ifdef SAO_STATUS_OK
#undef SAO_STATUS_OK
#endif
#include "entity_action_routes_internal.h"
#include "entity_builtin_action_internal.h"
#include "settings_owner_internal.h"
#include "settings_theme_internal.h"
#include "tool_launch_internal.h"

#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION) &&                                              \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "plugin_manager_panel_internal.h"
#include "process_selector_panel_internal.h"
#include "workshop_panel_internal.h"
#endif

#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION) &&                                           \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "entity_provider_publication_internal.h"
#include "sao/plugins/loader/entity_provider.h"
#endif

#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <windows.h>

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/core/logging.h"
#endif

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) &&                                         \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/rt_io/proxy.h"
#include "sao/rt_io/window_rect.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dc_mutation.h"
#include "sao/ui/entity_shell.h"
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
#include "sao/ui/fisheye_backdrop.h"
#include "sao/ui/panel_sdk.h"
#endif
#include "sao/ui/overlay_host.h"
#include "sao/ui/streaming_flow.h"
#include "sao/ui/theme.h"
#endif

#if defined(SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER) &&                                         \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "sao_security/anti_debug/probes.h"
#endif

// ---------------------------------------------------------------------------
// Headless init pipeline entry point.
//
// Kept in the same TU as the fail-closed subsystem stubs so unit tests can
// link a single file and inject only the providers under test.
//
// ``SaoLauncherBaseDir`` mirrors Python's ``config.BASE_DIR`` and is
// populated right after ``resolveWorkingDir`` succeeds so any subsystem
// that needs the resolved path can read it without threading state
// through every C ABI boundary.
// ---------------------------------------------------------------------------

extern "C" wchar_t SaoLauncherBaseDir[260] = {0};

namespace sao::launcher {

bool isPaidLicenseTier(const char* tier) noexcept {
    return tier != nullptr && (_stricmp(tier, "paid") == 0 || _stricmp(tier, "pro") == 0 ||
                               _stricmp(tier, "team") == 0);
}

bool buildPlatformConfig(const AppState& state, sao_platform_config& config,
                         char* log_level_storage, std::size_t log_level_capacity) noexcept {
    if (!log_level_storage || log_level_capacity == 0)
        return false;

    const wchar_t* source = state.log_level[0] ? state.log_level : L"info";
    const int converted =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, log_level_storage,
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
    config.streaming_entitled = state.no_license || state.streaming_entitled ? 1 : 0;
    return true;
}

} // namespace sao::launcher

namespace {

constexpr UINT_PTR kUiFrameTimerId = 0x53415549U;
constexpr int32_t kUiFrameIntervalMs = 16;
constexpr int32_t kHomeHotkeyId = 0x5341;
constexpr int32_t kInsertHotkeyId = 0x5342;
constexpr int kMaximumTeardownAttempts = 3;
constexpr int kMaximumStreamingReleaseAttempts = 2;

struct HeadlessCleanupState {
    ~HeadlessCleanupState() {
        if (dual_run_driver_acquired && dual_run_driver_mutex != nullptr) {
            sao_launcher_dual_run_release_driver_mutex(dual_run_driver_mutex);
        }
        if (mutex_acquired && single_instance_mutex != nullptr) {
            sao::launcher::releaseSingleInstance(single_instance_mutex);
        }
        if (crash_installed) {
            sao::launcher::uninstallCrashHandler();
        }
    }

    sao::launcher::AppState state;
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
};

std::mutex g_pending_cleanup_mutex;
std::unique_ptr<HeadlessCleanupState> g_pending_cleanup;

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
int runPipeline(const sao_launcher_init_hooks_t* hooks, sao::launcher::AppState& state,
                sao_dual_run_config& dual_cfg, HANDLE& single_instance_mutex,
                HANDLE& dual_run_driver_mutex, bool& crash_installed, bool& mutex_acquired,
                bool& dual_run_driver_acquired, bool& base_dir_resolved, bool& license_verified,
                bool& shell_verified, bool& security_initialized, bool& platform_up,
                bool& plugins_discovered, bool& ui_online, bool& handed_off_to_python,
                int& handed_off_exit_code) {
    using namespace sao::launcher;

    // ------------------------------------------------------------------
    // Step 0 — read dual_run.json + decide dispatch.  When the
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
        (GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, inherited_role, 64) > 0) &&
        inherited_role[0] != L'\0';

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
    sao_status_t zs =
        sao_launcher_dual_run_step_zero(&dual_cfg, &should_continue, &handoff_exit_code);
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

    // Publish base_dir globally for downstream providers.  A defensive copy so
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
            state.base_dir, state.config_path[0] ? state.config_path : nullptr) != SAO_STATUS_OK) {
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
            state.streaming_entitled = isPaidLicenseTier(r.tier);
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
        if (!buildPlatformConfig(state, cfg, log_level_narrow, sizeof(log_level_narrow))) {
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
        sao_status_t s =
            sao_plugins_discover(static_cast<sao_platform_ctx*>(state.platform_ctx), &reg);
        state.plugins_registry = reg;
        plugins_discovered = reg != nullptr;
        if (s != SAO_STATUS_OK || reg == nullptr) {
            return SAO_EXIT_PLUGIN_LOAD_FAIL;
        } else {
            if (sao_plugins_activate_autostart(reg) != SAO_STATUS_OK) {
                return SAO_EXIT_PLUGIN_LOAD_FAIL;
            }
            if (sao_platform_bind_plugins(static_cast<sao_platform_ctx*>(state.platform_ctx),
                                          reg) != SAO_STATUS_OK) {
                return SAO_EXIT_PLUGIN_LOAD_FAIL;
            }
        }
    }

    // Step 9 — UI online.
    {
        sao_status_t s = sao_ui_bring_online(static_cast<sao_platform_ctx*>(state.platform_ctx));
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
                if (msg.message == WM_QUIT)
                    return SAO_EXIT_OK;
                int32_t handled = 0;
                if (sao_ui_handle_message(static_cast<sao_platform_ctx*>(state.platform_ctx),
                                          msg.message, msg.wParam, msg.lParam,
                                          &handled) != SAO_STATUS_OK) {
                    return SAO_EXIT_UI_ONLINE_FAIL;
                }
                if (!handled) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if (sao_ui_tick(static_cast<sao_platform_ctx*>(state.platform_ctx),
                            kUiFrameIntervalMs) != SAO_STATUS_OK) {
                return SAO_EXIT_UI_ONLINE_FAIL;
            }
            if (hooks->poll_should_exit(hooks->user_data) != 0)
                break;
            Sleep(0);
        }
    } else {
        if (SetTimer(nullptr, kUiFrameTimerId, kUiFrameIntervalMs, nullptr) == 0) {
            return SAO_EXIT_UI_ONLINE_FAIL;
        }
        MSG msg{};
        BOOL result = 0;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            int32_t handled = 0;
            if (sao_ui_handle_message(static_cast<sao_platform_ctx*>(state.platform_ctx),
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
        if (result < 0)
            return SAO_EXIT_UI_ONLINE_FAIL;
    }

    return SAO_EXIT_OK;
}

// Reverse-order teardown.  Never throws; ``on_teardown_step`` is fired
// for each step actually rolled back so tests can assert the ordering.
bool teardown(const sao_launcher_init_hooks_t* hooks, sao::launcher::AppState& state,
              HANDLE& single_instance_mutex, bool& crash_installed, bool& mutex_acquired,
              bool /*base_dir_resolved*/, bool /*license_verified*/, bool /*shell_verified*/,
              bool& security_initialized, bool& platform_up, bool& plugins_discovered,
              bool& ui_online) noexcept {
    using namespace sao::launcher;

    // Reverse of init order: UI -> plugins -> rt_io/platform -> security ->
    // shell -> license -> single_instance -> crash_handler.
    if (ui_online) {
        if (sao_ui_take_offline(static_cast<sao_platform_ctx*>(state.platform_ctx)) !=
            SAO_STATUS_OK) {
            notifyStep(hooks, "ui");
            return false;
        }
        ui_online = false;
        notifyStep(hooks, "ui");
    }
    if (plugins_discovered) {
        if (state.platform_ctx != nullptr) {
            if (sao_platform_bind_plugins(static_cast<sao_platform_ctx*>(state.platform_ctx),
                                          nullptr) != SAO_STATUS_OK) {
                notifyStep(hooks, "plugins");
                return false;
            }
        }
        if (sao_plugins_shutdown(static_cast<sao_plugins_registry*>(state.plugins_registry)) !=
            SAO_STATUS_OK) {
            notifyStep(hooks, "plugins");
            return false;
        }
        state.plugins_registry = nullptr;
        plugins_discovered = false;
        notifyStep(hooks, "plugins");
    }
    if (platform_up) {
        if (sao_platform_teardown(static_cast<sao_platform_ctx*>(state.platform_ctx)) !=
            SAO_STATUS_OK) {
            notifyStep(hooks, "platform");
            return false;
        }
        state.platform_ctx = nullptr;
        platform_up = false;
        notifyStep(hooks, "platform");
    }
    if (security_initialized) {
        if (sao_security_shutdown() != SAO_STATUS_OK) {
            notifyStep(hooks, "security");
            return false;
        }
        security_initialized = false;
        notifyStep(hooks, "security");
    }
    if (state.shell_active) {
        if (sao_shell_shutdown() != SAO_STATUS_OK) {
            notifyStep(hooks, "shell");
            return false;
        }
        state.shell_active = false;
        notifyStep(hooks, "shell");
    }
    if (state.license_active) {
        if (sao_license_shutdown() != SAO_STATUS_OK) {
            notifyStep(hooks, "license");
            return false;
        }
        state.license_active = false;
        notifyStep(hooks, "license");
    }
    if (mutex_acquired && single_instance_mutex) {
        releaseSingleInstance(single_instance_mutex);
        single_instance_mutex = nullptr;
        mutex_acquired = false;
        notifyStep(hooks, "single_instance");
    }
    if (crash_installed) {
        uninstallCrashHandler();
        crash_installed = false;
        notifyStep(hooks, "crash_handler");
    }
    return true;
}

sao_status_t retryPendingCleanup() noexcept {
    std::lock_guard lock(g_pending_cleanup_mutex);
    if (!g_pending_cleanup)
        return SAO_STATUS_OK;
    auto& cleanup = *g_pending_cleanup;
    if (!teardown(nullptr, cleanup.state, cleanup.single_instance_mutex, cleanup.crash_installed,
                  cleanup.mutex_acquired, cleanup.base_dir_resolved, cleanup.license_verified,
                  cleanup.shell_verified, cleanup.security_initialized, cleanup.platform_up,
                  cleanup.plugins_discovered, cleanup.ui_online)) {
        return SAO_STATUS_INTERNAL;
    }
    if (cleanup.dual_run_driver_acquired && cleanup.dual_run_driver_mutex != nullptr) {
        sao_launcher_dual_run_release_driver_mutex(cleanup.dual_run_driver_mutex);
        cleanup.dual_run_driver_mutex = nullptr;
        cleanup.dual_run_driver_acquired = false;
    }
    g_pending_cleanup.reset();
    return SAO_STATUS_OK;
}

using StreamingModeAcquireFn = sao_status_t (*)(void* user_data);
using StreamingModeGetFn = bool (*)(void* user_data);
using StreamingModeSetFn = sao_status_t (*)(bool enabled, void* user_data);
using StreamingModeReleaseFn = sao_status_t (*)(void* user_data);
using EntityAuthorityStepFn = sao_status_t (*)(void* user_data);
using NervgearModeSetFn = sao_status_t (*)(bool enabled, void* user_data);
using NervgearModePersistFn = sao_status_t (*)(bool enabled, void* user_data);
using NervgearDegradedFn = sao_status_t (*)(void* user_data);

sao_status_t releaseStreamingModeWithRetry(StreamingModeReleaseFn release, void* user_data) {
    sao_status_t status = release(user_data);
    for (int attempt = 1; status != SAO_STATUS_OK && attempt < kMaximumStreamingReleaseAttempts;
         ++attempt) {
        status = release(user_data);
    }
    return status;
}

sao_status_t applyStreamingModeTransaction(bool enabled, StreamingModeAcquireFn acquire,
                                           StreamingModeGetFn get_flow,
                                           StreamingModeGetFn get_capture,
                                           StreamingModeSetFn set_capture,
                                           StreamingModeSetFn set_flow,
                                           StreamingModeReleaseFn release, void* user_data) {
    sao_status_t status = acquire(user_data);
    if (status != SAO_STATUS_OK) {
        return status;
    }

    const bool previous_flow = get_flow(user_data);
    const bool previous_capture = get_capture(user_data);
    status = set_capture(enabled, user_data);
    if (status == SAO_STATUS_OK) {
        status = set_flow(enabled, user_data);
    }

    sao_status_t compensation_status = SAO_STATUS_OK;
    if (status != SAO_STATUS_OK) {
        compensation_status = set_capture(previous_capture, user_data);
        const sao_status_t flow_status = set_flow(previous_flow, user_data);
        if (compensation_status == SAO_STATUS_OK) {
            compensation_status = flow_status;
        }
    }

    const sao_status_t release_status = releaseStreamingModeWithRetry(release, user_data);
    if (release_status != SAO_STATUS_OK && status == SAO_STATUS_OK) {
        compensation_status = set_capture(previous_capture, user_data);
        const sao_status_t flow_status = set_flow(previous_flow, user_data);
        if (compensation_status == SAO_STATUS_OK) {
            compensation_status = flow_status;
        }
    }
    if (compensation_status != SAO_STATUS_OK) {
        return compensation_status;
    }
    if (release_status != SAO_STATUS_OK) {
        return release_status;
    }
    return status;
}

sao_status_t publishEntityAuthorityBeforeOnline(EntityAuthorityStepFn publish,
                                                EntityAuthorityStepFn bring_online,
                                                void* user_data) {
    if (publish == nullptr || bring_online == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const sao_status_t publication_status = publish(user_data);
    return publication_status == SAO_STATUS_OK ? bring_online(user_data) : publication_status;
}

sao_status_t applyNervgearModeTransaction(bool& mode, NervgearModeSetFn set_shell_mode,
                                          NervgearModePersistFn persist_mode,
                                          NervgearDegradedFn publish_degraded, void* user_data) {
    if (set_shell_mode == nullptr || persist_mode == nullptr || publish_degraded == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const bool previous = mode;
    const bool target = !previous;
    sao_status_t status = set_shell_mode(target, user_data);
    if (status != SAO_STATUS_OK) {
        return status;
    }
    status = persist_mode(target, user_data);
    if (status == SAO_STATUS_OK) {
        mode = target;
        return SAO_STATUS_OK;
    }
    const sao_status_t rollback_status = set_shell_mode(previous, user_data);
    if (rollback_status == SAO_STATUS_OK) {
        return status;
    }

    mode = target;
    const sao_status_t degraded_status = publish_degraded(user_data);
    return degraded_status == SAO_STATUS_OK ? SAO_STATUS_ERR_UNKNOWN : degraded_status;
}

} // namespace

extern "C" sao_status_t sao_launcher_init_pipeline_retry_pending_cleanup(void) {
    return retryPendingCleanup();
}

extern "C" sao_status_t sao_launcher_init_pipeline_run(int argc, wchar_t** argv,
                                                       const sao_launcher_init_hooks_t* hooks,
                                                       int* exit_code_out) {
    using namespace sao::launcher;

    if (exit_code_out)
        *exit_code_out = SAO_EXIT_OK;

    if (retryPendingCleanup() != SAO_STATUS_OK) {
        if (exit_code_out)
            *exit_code_out = SAO_EXIT_PLATFORM_INIT_FAIL;
        return SAO_STATUS_INTERNAL;
    }

    std::unique_ptr<HeadlessCleanupState> cleanup(new (std::nothrow) HeadlessCleanupState{});
    if (!cleanup) {
        if (exit_code_out)
            *exit_code_out = SAO_EXIT_PLATFORM_INIT_FAIL;
        return SAO_STATUS_INTERNAL;
    }
    auto& state = cleanup->state;

    // Command line first — a --help / --version can short-circuit.
    if (argc > 0 && argv != nullptr) {
        bool should_exit = false;
        int rc = SAO_EXIT_OK;
        if (!parseCommandLineFromArgv(argc, argv, state, should_exit, rc)) {
            if (exit_code_out)
                *exit_code_out = SAO_EXIT_BAD_ARGS;
            return SAO_STATUS_INVALID_ARGUMENT;
        }
        if (should_exit) {
            if (exit_code_out)
                *exit_code_out = rc;
            return SAO_STATUS_OK;
        }
    }

    LauncherLifecycleDecision lifecycle;
    if (prepareLauncherLifecycle(lifecycle) != SAO_STATUS_OK) {
        if (exit_code_out)
            *exit_code_out = SAO_EXIT_PLATFORM_INIT_FAIL;
        return SAO_STATUS_INTERNAL;
    }

    bool handed_off_to_python = false;
    int handed_off_exit_code = 0;

    int rc = runPipeline(hooks, state, lifecycle.dual_config, cleanup->single_instance_mutex,
                         cleanup->dual_run_driver_mutex, cleanup->crash_installed,
                         cleanup->mutex_acquired, cleanup->dual_run_driver_acquired,
                         cleanup->base_dir_resolved, cleanup->license_verified,
                         cleanup->shell_verified, cleanup->security_initialized,
                         cleanup->platform_up, cleanup->plugins_discovered, cleanup->ui_online,
                         handed_off_to_python, handed_off_exit_code);
    const int rollout_result = rc;

    // CPP_PREFERRED_PYTHON_FALLBACK — if any CPP step failed AFTER step-zero
    // let us know it wanted CPP, retry via Python.  step-zero itself didn't
    // spawn Python; we do that lazily here so the fallback is only paid for
    // when the CPP path actually fails.
    wchar_t inherited_role[64]{};
    const bool is_child_of_dual_run_driver =
        GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, inherited_role, 64) > 0 &&
        inherited_role[0] != L'\0';
    if (!is_child_of_dual_run_driver && !handed_off_to_python && rc != SAO_EXIT_OK &&
        rc != SAO_EXIT_ALREADY_RUNNING && rc != SAO_EXIT_BAD_ARGS) {
        if (lifecycle.dual_config.mode == SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK) {
            int32_t fbec = 0;
            const wchar_t* step_name = L"cpp_pipeline";
            if (!cleanup->platform_up)
                step_name = L"platform_bringup";
            else if (!cleanup->plugins_discovered && !state.safe_mode)
                step_name = L"plugins_discover";
            else if (!cleanup->ui_online)
                step_name = L"ui_bring_online";
            if (sao_launcher_dual_run_maybe_fallback_to_python(&lifecycle.dual_config, rc,
                                                               step_name, &fbec)) {
                handed_off_to_python = true;
                handed_off_exit_code = fbec;
                rc = fbec;
            }
        }
    }

    bool teardown_complete = false;
    for (int attempt = 0; attempt < kMaximumTeardownAttempts; ++attempt) {
        if (teardown(hooks, state, cleanup->single_instance_mutex, cleanup->crash_installed,
                     cleanup->mutex_acquired, cleanup->base_dir_resolved, cleanup->license_verified,
                     cleanup->shell_verified, cleanup->security_initialized, cleanup->platform_up,
                     cleanup->plugins_discovered, cleanup->ui_online)) {
            teardown_complete = true;
            break;
        }
    }

    if (cleanup->dual_run_driver_acquired && cleanup->dual_run_driver_mutex) {
        sao_launcher_dual_run_release_driver_mutex(cleanup->dual_run_driver_mutex);
        cleanup->dual_run_driver_mutex = nullptr;
        cleanup->dual_run_driver_acquired = false;
    }

    const char* failure_hint = "init_pipeline";
    if (!cleanup->base_dir_resolved)
        failure_hint = "working_dir";
    else if (!cleanup->security_initialized)
        failure_hint = "security_init";
    else if (!cleanup->platform_up)
        failure_hint = "platform_bringup";
    else if (!cleanup->plugins_discovered && !state.safe_mode &&
             launcherProviderConfigurationSnapshot().plugins.enabled) {
        failure_hint = "plugins_discover";
    } else if (!cleanup->ui_online)
        failure_hint = "ui_bring_online";
    completeLauncherLifecycle(lifecycle, rollout_result, failure_hint);

    if (!teardown_complete && (rc == SAO_EXIT_OK || handed_off_to_python)) {
        rc = SAO_EXIT_PLATFORM_INIT_FAIL;
        handed_off_to_python = false;
    }
    if (exit_code_out)
        *exit_code_out = rc;
    if (!teardown_complete) {
        std::lock_guard lock(g_pending_cleanup_mutex);
        g_pending_cleanup = std::move(cleanup);
        return SAO_STATUS_INTERNAL;
    }
    if (handed_off_to_python) {
        return SAO_STATUS_OK;
    }
    return rc == SAO_EXIT_OK ? SAO_STATUS_OK : SAO_STATUS_INTERNAL;
}

extern "C" void
sao_launcher_set_composition_test_hooks(const sao_launcher_composition_test_hooks_t* hooks) {
#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
    g_composition_test_hooks = hooks ? *hooks : sao_launcher_composition_test_hooks_t{};
#else
    (void)hooks;
#endif
}

extern "C" {

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_launcher_init_pipeline_test_apply_streaming_mode_transaction(
    bool enabled, sao_status_t (*acquire)(void*), bool (*get_flow)(void*),
    bool (*get_capture)(void*), sao_status_t (*set_capture)(bool, void*),
    sao_status_t (*set_flow)(bool, void*), sao_status_t (*release)(void*), void* user_data) {
    if (acquire == nullptr || get_flow == nullptr || get_capture == nullptr ||
        set_capture == nullptr || set_flow == nullptr || release == nullptr) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    return applyStreamingModeTransaction(enabled, acquire, get_flow, get_capture, set_capture,
                                         set_flow, release, user_data);
}

sao_status_t sao_launcher_init_pipeline_test_publish_entity_authority_before_online(
    sao_status_t (*publish)(void*), sao_status_t (*bring_online)(void*), void* user_data) {
    return publishEntityAuthorityBeforeOnline(publish, bring_online, user_data);
}

sao_status_t sao_launcher_init_pipeline_test_apply_nervgear_mode_transaction(
    bool* mode, sao_status_t (*set_shell_mode)(bool, void*),
    sao_status_t (*persist_mode)(bool, void*), sao_status_t (*publish_degraded)(void*),
    void* user_data) {
    if (mode == nullptr) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    return applyNervgearModeTransaction(*mode, set_shell_mode, persist_mode, publish_degraded,
                                        user_data);
}

sao_status_t sao_platform_bringup(const sao_platform_config* cfg, sao_platform_ctx** ctx_out) {
    if (!g_composition_test_hooks.platform_bringup) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.platform_bringup(cfg, ctx_out,
                                                     g_composition_test_hooks.user_data);
}
sao_status_t sao_platform_teardown(sao_platform_ctx* ctx) {
    if (!g_composition_test_hooks.platform_teardown) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.platform_teardown(ctx, g_composition_test_hooks.user_data);
}
sao_status_t sao_platform_bind_plugins(sao_platform_ctx*, sao_plugins_registry*) {
    return SAO_STATUS_OK;
}
sao_status_t sao_ui_bring_online(sao_platform_ctx* ctx) {
    if (!g_composition_test_hooks.ui_bring_online) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.ui_bring_online(ctx, g_composition_test_hooks.user_data);
}
sao_status_t sao_ui_take_offline(sao_platform_ctx* ctx) {
    if (!g_composition_test_hooks.ui_take_offline) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.ui_take_offline(ctx, g_composition_test_hooks.user_data);
}
sao_status_t sao_ui_tick(sao_platform_ctx* ctx, uint32_t elapsed_ms) {
    return g_composition_test_hooks.ui_tick
               ? g_composition_test_hooks.ui_tick(ctx, elapsed_ms,
                                                  g_composition_test_hooks.user_data)
               : SAO_STATUS_OK;
}
sao_status_t sao_ui_handle_message(sao_platform_ctx* ctx, uint32_t message, uintptr_t w_param,
                                   intptr_t l_param, int32_t* out_handled) {
    if (out_handled)
        *out_handled = 0;
    return g_composition_test_hooks.ui_handle_message
               ? g_composition_test_hooks.ui_handle_message(ctx, message, w_param, l_param,
                                                            out_handled,
                                                            g_composition_test_hooks.user_data)
               : SAO_STATUS_OK;
}
#elif defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
struct sao_platform_ctx {
    sao_rt_io_proxy_handle_t rt_io_proxy;
    sao_rt_io_window_rect_controller_t window_rect_controller;
    SaoRtIoWindowToken window_rect_token;
    bool window_rect_registered;
    sao_ui_dc_mutation_coordinator_handle_t dc_mutation_coordinator;
    sao_ui_overlay_host_handle_t overlay_host;
    sao_ui_compositor_handle_t compositor;
    bool sdk_compositor_bound;
    sao_ui_entity_shell_handle_t entity_shell;
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    sao_ui_fisheye_backdrop_handle_t fisheye_backdrop;
    std::unique_ptr<sao::launcher::plugin_manager_panel::Owner> plugin_manager_panel;
    std::unique_ptr<sao::launcher::process_selector_panel::Owner> process_selector_panel;
    std::unique_ptr<sao::launcher::workshop_panel::Owner> workshop_panel;
    bool workshop_panel_visible;
#endif
    bool home_hotkey_registered;
    bool insert_hotkey_registered;
    SaoUiThemeId previous_theme = SAO_UI_THEME_DARK;
    bool restore_theme_on_rollback = false;
    bool settings_save_enabled = false;
    bool nervgear_mode{true};
    bool streaming_flow_started = false;
    sao_plugins_registry* plugins_registry = nullptr;
    sao::launcher::entity_builtin_action::State builtin_action_state;
    sao::launcher::entity_action_routes::EntityActionRouteStore entity_action_routes;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    sao::launcher::entity_provider_publication::EntityProviderPublicationState
        entity_provider_publication;
#endif
    std::unique_ptr<sao::launcher::settings_owner::SettingsOwner> settings_owner;
    std::unique_ptr<sao::launcher::tool_launch::AiEditorProcessOwner> ai_editor;
};

sao_status_t create_ai_editor_owner(
    const wchar_t* base_dir,
    std::unique_ptr<sao::launcher::tool_launch::AiEditorProcessOwner>& out) noexcept {
    try {
        out = std::make_unique<sao::launcher::tool_launch::AiEditorProcessOwner>(base_dir);
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t
create_settings_owner(const wchar_t* base_dir,
                      std::unique_ptr<sao::launcher::settings_owner::SettingsOwner>& out) noexcept {
    try {
        const auto settings_path = std::filesystem::path(base_dir) / L"settings.json";
        return sao::launcher::settings_owner::SettingsOwner::create(settings_path.wstring(), out);
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
struct SharedPanelVisibilityProbe {
    bool plugin_manager = false;
    bool process_selector = false;
};

sao_status_t reload_plugins(void* user_data);

void SAO_UI_CALL collect_shared_panel_visibility(sao_ui_panel_handle_t panel,
                                                 const SaoPanelDescriptor* descriptor,
                                                 void* user_data) {
    auto* visibility = static_cast<SharedPanelVisibilityProbe*>(user_data);
    if (visibility == nullptr || descriptor == nullptr || descriptor->panel_id_utf8 == nullptr)
        return;
    const bool plugin_manager =
        std::strcmp(descriptor->panel_id_utf8,
                    sao::launcher::plugin_manager_panel::kPanelId.data()) == 0;
    const bool process_selector = std::strcmp(descriptor->panel_id_utf8,
                                              sao::launcher::process_selector_panel::kPanelId) == 0;
    if (!plugin_manager && !process_selector)
        return;
    SaoPanelState state{};
    if (sao_ui_panel_get_state(panel, &state) != SAO_STATUS_OK)
        return;
    visibility->plugin_manager = visibility->plugin_manager || (plugin_manager && state.visible);
    visibility->process_selector =
        visibility->process_selector || (process_selector && state.visible);
}

sao_status_t update_shared_fisheye_visibility(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr || ctx->fisheye_backdrop == nullptr)
        return SAO_STATUS_OK;

    SharedPanelVisibilityProbe panels;
    sao_status_t status = sao_ui_panel_registry_iterate(&collect_shared_panel_visibility, &panels);
    if (status != SAO_STATUS_OK)
        return status;

    SaoUiEntityShellSnapshot entity{};
    if (ctx->entity_shell != nullptr) {
        status = sao_ui_entity_shell_get_snapshot(ctx->entity_shell, &entity);
        if (status != SAO_STATUS_OK)
            return status;
    }

    const sao::launcher::entity_builtin_action::SharedFisheyeVisibility visibility{
        ctx->workshop_panel_visible,
        panels.plugin_manager,
        panels.process_selector,
        entity.overlay_visible && entity.menu_visible,
    };
    if (!sao::launcher::entity_builtin_action::should_show_shared_fisheye(visibility))
        return sao_ui_fisheye_backdrop_hide(ctx->fisheye_backdrop);

    SaoOverlayHostClientRect host_bounds{};
    status = sao_ui_overlay_host_get_client_rect(ctx->overlay_host, &host_bounds);
    if (status != SAO_STATUS_OK)
        return status;
    if (host_bounds.width <= 0 || host_bounds.height <= 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const SaoUiFisheyeBackdropRect local_bounds{
        0,
        0,
        host_bounds.width,
        host_bounds.height,
    };
    return sao_ui_fisheye_backdrop_show(
        ctx->fisheye_backdrop, &local_bounds,
        sao::launcher::entity_builtin_action::kSharedFisheyeBackdropZOrder);
}

sao_status_t tick_shared_fisheye(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr || ctx->fisheye_backdrop == nullptr)
        return SAO_STATUS_OK;
    const sao_status_t visibility_status = update_shared_fisheye_visibility(ctx);
    if (visibility_status != SAO_STATUS_OK)
        return visibility_status;
    return sao_ui_fisheye_backdrop_tick(ctx->fisheye_backdrop);
}

sao_status_t create_shared_ui_owners(const wchar_t* base_dir, sao_platform_ctx* ctx) noexcept {
    if (base_dir == nullptr || ctx == nullptr || ctx->compositor == nullptr ||
        ctx->rt_io_proxy == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sao_status_t status = sao_ui_fisheye_backdrop_create(ctx->compositor, &ctx->fisheye_backdrop);
    if (status != SAO_STATUS_OK)
        return status;
    try {
        ctx->plugin_manager_panel = std::make_unique<sao::launcher::plugin_manager_panel::Owner>(
            ctx->compositor, [ctx] { return reload_plugins(ctx); });
        ctx->process_selector_panel =
            std::make_unique<sao::launcher::process_selector_panel::Owner>(ctx->compositor,
                                                                           ctx->rt_io_proxy);
        ctx->workshop_panel = std::make_unique<sao::launcher::workshop_panel::Owner>(
            ctx->compositor, std::filesystem::path(base_dir));
        ctx->workshop_panel->set_visibility_changed_callback([ctx](bool visible) {
            ctx->workshop_panel_visible = visible;
            (void)update_shared_fisheye_visibility(ctx);
        });
        return SAO_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    } catch (...) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
}

sao_status_t retire_shared_ui_owners(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (ctx->workshop_panel) {
        const sao_status_t status = ctx->workshop_panel->try_take_offline();
        if (status != SAO_STATUS_OK)
            return status;
        ctx->workshop_panel->set_visibility_changed_callback({});
        ctx->workshop_panel.reset();
        ctx->workshop_panel_visible = false;
        ctx->builtin_action_state.authority.workshop = false;
    }
    if (ctx->process_selector_panel) {
        const sao_status_t status = ctx->process_selector_panel->take_offline();
        if (status != SAO_STATUS_OK)
            return status;
        ctx->process_selector_panel.reset();
        ctx->builtin_action_state.authority.process_selector = false;
    }
    if (ctx->plugin_manager_panel) {
        const sao_status_t status = ctx->plugin_manager_panel->take_offline();
        if (status != SAO_STATUS_OK)
            return status;
        ctx->plugin_manager_panel.reset();
        ctx->builtin_action_state.authority.plugin_manager = false;
        ctx->builtin_action_state.authority.plugin_status = false;
    }
    if (ctx->fisheye_backdrop != nullptr) {
        const sao_status_t status = sao_ui_fisheye_backdrop_try_destroy(ctx->fisheye_backdrop);
        if (status != SAO_STATUS_OK)
            return status;
        ctx->fisheye_backdrop = nullptr;
        ctx->builtin_action_state.authority.fisheye_procedural = false;
        ctx->builtin_action_state.authority.fisheye_live = false;
    }
    return SAO_STATUS_OK;
}
#else
sao_status_t tick_shared_fisheye(sao_platform_ctx*) noexcept {
    return SAO_STATUS_OK;
}
#endif

SaoUiThemeId runtime_theme_id(sao::launcher::settings_theme::PanelTheme theme) noexcept {
    return theme == sao::launcher::settings_theme::PanelTheme::light ? SAO_UI_THEME_LIGHT
                                                                     : SAO_UI_THEME_DARK;
}

sao_status_t map_sdk_runtime_status(sao_sdk_status_t status) noexcept {
    switch (status) {
    case SAO_SDK_OK:
        return SAO_STATUS_OK;
    case SAO_SDK_ERR_INVALID_ARGUMENT:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    case SAO_SDK_ERR_NOT_INITIALIZED:
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    case SAO_SDK_ERR_HANDLE_INVALID:
        return SAO_STATUS_ERR_HANDLE_INVALID;
    case SAO_SDK_ERR_BUFFER_TOO_SMALL:
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    case SAO_SDK_ERR_NOT_IMPLEMENTED:
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    case SAO_SDK_ERR_ABI_MISMATCH:
        return SAO_STATUS_ERR_ABI_MISMATCH;
    case SAO_SDK_ERR_UNSUPPORTED:
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    case SAO_SDK_ERR_BUSY:
        return SAO_STATUS_ERR_CANCELLED;
    case SAO_SDK_ERR_ACCESS_DENIED:
        return SAO_STATUS_ERR_ACCESS_DENIED;
    case SAO_SDK_ERR_NOT_FOUND:
        return SAO_STATUS_ERR_NOT_FOUND;
    case SAO_SDK_ERR_ALREADY_EXISTS:
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    case SAO_SDK_ERR_INTERNAL:
    default:
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t teardown_platform_context(sao_platform_ctx* ctx, bool save_settings) noexcept;

sao_status_t rollback_platform_bringup(sao_platform_ctx* ctx, sao_platform_ctx** ctx_out,
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

sao_status_t SAO_UI_CALL hide_window_rect(void* user_data, void* hwnd,
                                          const SaoUiDcMutationRect* fake_rect, uint32_t settle_ms,
                                          uint32_t timeout_ms) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || hwnd == nullptr || fake_rect == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (ctx->window_rect_controller == nullptr || !ctx->window_rect_registered) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    const uint64_t hwnd_value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
    const SaoRtIoWindowToken& token = ctx->window_rect_token;
    if (token.hwnd == 0 || token.pid == 0 || token.tid == 0 || token.generation == 0 ||
        token.hwnd != hwnd_value) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (fake_rect->right <= fake_rect->left || fake_rect->bottom <= fake_rect->top) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const SaoRtIoRect rect{fake_rect->left, fake_rect->top, fake_rect->right, fake_rect->bottom};
    SaoRtIoCallResult result{};
    return sao_rt_io_hide_window_rect(ctx->window_rect_controller, &token, &rect, settle_ms,
                                      timeout_ms, &result);
}

sao_status_t SAO_UI_CALL hide_exstyle(void* user_data, void* hwnd, uint32_t mask,
                                      uint32_t timeout_ms) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || hwnd == nullptr || mask == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (ctx->window_rect_controller == nullptr || !ctx->window_rect_registered) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    const uint64_t hwnd_value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
    const SaoRtIoWindowToken& token = ctx->window_rect_token;
    if (token.hwnd == 0 || token.pid == 0 || token.tid == 0 || token.generation == 0 ||
        token.hwnd != hwnd_value) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    SaoRtIoCallResult result{};
    return sao_rt_io_clear_window_exstyle(ctx->window_rect_controller, &token, mask, timeout_ms,
                                          &result);
}

#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
void sync_entity_publication_authority(sao_platform_ctx* ctx) noexcept {
    const auto& source = ctx->builtin_action_state.authority;
    auto& target = ctx->entity_provider_publication.builtin_authority;
    target.publication_available = source.publication_available;
    target.topmost = source.topmost;
    target.nervgear = source.nervgear;
    target.streaming = source.streaming;
    target.save_settings = source.save_settings;
    target.ai_editor = source.ai_editor;
    target.workshop = source.workshop;
    target.process_selector = source.process_selector;
    target.plugin_manager = source.plugin_manager;
    target.reload_plugins = source.reload_plugins;
    target.plugin_status = source.plugin_status;
    target.fisheye_procedural = source.fisheye_procedural;
    target.fisheye_live = source.fisheye_live;
    target.theme = source.theme;
    target.about = source.about;
    target.controls =
        source.controls && !ctx->builtin_action_state.controls_degraded
            ? sao::launcher::entity_provider_publication::ControlPublicationStatus::ready
            : sao::launcher::entity_provider_publication::ControlPublicationStatus::
                  degraded_internal;
}

sao_status_t sync_plugin_runtime_authority(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto& action = ctx->builtin_action_state.authority;
    auto& publication = ctx->entity_provider_publication.builtin_authority;
    if (ctx->plugins_registry == nullptr) {
        action.plugin_runtime = false;
        action.reload_plugins = false;
        action.plugin_manager = false;
        action.plugin_status = false;
        sync_entity_publication_authority(ctx);
        publication.plugin_runtime = sao::launcher::entity_provider_publication::
            PluginRuntimePublicationStatus::not_applicable;
        publication.python_runtime = sao::launcher::entity_provider_publication::
            PythonRuntimePublicationStatus::not_applicable;
        return SAO_STATUS_OK;
    }

    sao_plugins_status_snapshot_t plugins_status{};
    plugins_status.struct_size = sizeof(plugins_status);
    const sao_status_t status = sao_plugins_status_snapshot(ctx->plugins_registry, &plugins_status);
    const bool plugin_runtime_ready =
        status == SAO_STATUS_OK &&
        plugins_status.operational_status == SAO_PLUGINS_OPERATIONAL_READY;
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    const bool plugin_panel_ready = ctx->plugin_manager_panel != nullptr;
#else
    constexpr bool plugin_panel_ready = false;
#endif
    action.plugin_runtime = plugin_runtime_ready;
    action.reload_plugins = plugin_runtime_ready;
    action.plugin_manager = plugin_runtime_ready && plugin_panel_ready;
    action.plugin_status = plugin_runtime_ready && plugin_panel_ready;
    sync_entity_publication_authority(ctx);
    publication.plugin_runtime =
        plugin_runtime_ready
            ? sao::launcher::entity_provider_publication::PluginRuntimePublicationStatus::ready
            : sao::launcher::entity_provider_publication::PluginRuntimePublicationStatus::
                  degraded_internal;
    if (status != SAO_STATUS_OK)
        return status;

    using PythonStatus = sao::launcher::entity_provider_publication::PythonRuntimePublicationStatus;
    switch (plugins_status.python_runtime_status) {
    case SAO_PLUGINS_PYTHON_RUNTIME_READY:
        publication.python_runtime = PythonStatus::ready;
        break;
    case SAO_PLUGINS_PYTHON_RUNTIME_UNCONFIGURED:
        publication.python_runtime = PythonStatus::degraded_unconfigured;
        break;
    case SAO_PLUGINS_PYTHON_RUNTIME_UNAVAILABLE:
        publication.python_runtime = PythonStatus::degraded_unavailable;
        break;
    default:
        publication.python_runtime = PythonStatus::degraded_host_unavailable;
        break;
    }
    return SAO_STATUS_OK;
}
#endif

sao_status_t persist_topmost_mode(bool enabled, void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    return ctx == nullptr || !ctx->settings_owner
               ? SAO_STATUS_ERR_NOT_INITIALIZED
               : ctx->settings_owner->set_value_and_save("topmost", enabled);
}

sao_status_t apply_streaming_mode(bool enabled, void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || ctx->overlay_host == nullptr || !ctx->streaming_flow_started) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    return applyStreamingModeTransaction(
        enabled, [](void*) { return sao_streaming_flow_mode_lock_acquire(2.0); },
        [](void*) { return sao_streaming_flow_get_mode(); },
        [](void* context) {
            return sao_ui_overlay_host_capture_excluded(
                static_cast<sao_platform_ctx*>(context)->overlay_host);
        },
        [](bool exclude, void* context) {
            return sao_ui_overlay_host_set_capture_mode(
                static_cast<sao_platform_ctx*>(context)->overlay_host, exclude);
        },
        [](bool exclude, void*) -> sao_status_t {
            return sao_streaming_flow_set_mode(exclude) == 0 ? SAO_STATUS_ERR_NOT_INITIALIZED
                                                             : SAO_STATUS_OK;
        },
        [](void*) { return sao_streaming_flow_mode_lock_release(); }, ctx);
}

sao_status_t persist_streaming_mode(bool enabled, void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    return ctx == nullptr || !ctx->settings_owner
               ? SAO_STATUS_ERR_NOT_INITIALIZED
               : ctx->settings_owner->set_value_and_save("streaming_mode", enabled);
}

#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
sao_status_t open_workshop(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || !ctx->workshop_panel)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t status = ctx->workshop_panel->open();
    if (status != SAO_STATUS_OK)
        return status;
    return update_shared_fisheye_visibility(ctx);
}

sao_status_t open_process_selector(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || !ctx->process_selector_panel)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t status = ctx->process_selector_panel->open();
    if (status != SAO_STATUS_OK)
        return status;
    return update_shared_fisheye_visibility(ctx);
}

sao_status_t open_plugin_manager(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || !ctx->plugin_manager_panel)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t status = ctx->plugin_manager_panel->open();
    if (status != SAO_STATUS_OK)
        return status;
    return update_shared_fisheye_visibility(ctx);
}

sao_status_t open_plugin_status(void* user_data) {
    return open_plugin_manager(user_data);
}

sao_status_t set_fisheye_procedural(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    return ctx == nullptr || ctx->fisheye_backdrop == nullptr
               ? SAO_STATUS_ERR_NOT_INITIALIZED
               : sao_ui_fisheye_backdrop_set_mode(ctx->fisheye_backdrop,
                                                  SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL);
}

sao_status_t set_fisheye_live(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    return ctx == nullptr || ctx->fisheye_backdrop == nullptr
               ? SAO_STATUS_ERR_NOT_INITIALIZED
               : sao_ui_fisheye_backdrop_set_mode(ctx->fisheye_backdrop,
                                                  SAO_UI_FISHEYE_BACKDROP_MODE_LIVE);
}
#endif

sao_status_t reload_plugins(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || ctx->plugins_registry == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    const sao_status_t status = sao_plugins_reload_all(ctx->plugins_registry);
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    const sao_status_t authority_status = sync_plugin_runtime_authority(ctx);
    if (authority_status != SAO_STATUS_OK) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
#endif
    return status;
}

sao_status_t refresh_entity(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || ctx->entity_shell == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    ctx->builtin_action_state.authority.publication_available = true;
    sync_entity_publication_authority(ctx);
    ctx->entity_provider_publication.topmost = ctx->builtin_action_state.topmost;
    ctx->entity_provider_publication.streaming_mode = ctx->builtin_action_state.streaming_mode;
    const sao_status_t status = sao::launcher::entity_provider_publication::refresh(
        ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
        ctx->nervgear_mode, &sao::plugins::loader::sao_plugins_entity_provider_snapshot_v2,
        &sao::plugins::loader::sao_plugins_entity_provider_snapshot,
        &sao_ui_entity_shell_set_roots);
    if (status != SAO_STATUS_OK) {
        ctx->builtin_action_state.authority.publication_available = false;
        sync_entity_publication_authority(ctx);
    }
    return status;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

sao_status_t publish_nervgear_degraded(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || ctx->entity_shell == nullptr) {
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    ctx->builtin_action_state.controls_degraded = true;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    sync_entity_publication_authority(ctx);
    const sao_status_t status = refresh_entity(ctx);
    if (status != SAO_STATUS_OK) {
        ctx->builtin_action_state.authority.publication_available = false;
        sync_entity_publication_authority(ctx);
    }
    return status;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

sao_status_t SAO_UI_CALL entity_action(SaoUiEntityAction action, void* user_data) {
    const auto action_token = static_cast<std::int32_t>(action);
    if (sao::launcher::entity_action_routes::is_dynamic_token(action_token)) {
        auto* ctx = static_cast<sao_platform_ctx*>(user_data);
        if (ctx == nullptr) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        sao::launcher::entity_action_routes::EntityActionRoute route;
        const sao_status_t route_status = ctx->entity_action_routes.resolve(action_token, route);
        if (route_status == SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
            sao::launcher::entity_provider_publication::OwnedEntityActionResult result;
            return sao::launcher::entity_provider_publication::invoke_v2(
                route, ctx->entity_shell,
                &sao::plugins::loader::sao_plugins_entity_provider_invoke_v2,
                &sao_ui_entity_shell_get_snapshot, &sao_ui_entity_shell_home, &result);
#else
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
        }
        return route_status == SAO_STATUS_ERR_NOT_FOUND ? SAO_STATUS_ERR_INVALID_ARGUMENT
                                                        : route_status;
    }
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr) {
        const sao::launcher::entity_builtin_action::State unavailable_state{};
        return sao::launcher::entity_builtin_action::authorization_status(action,
                                                                          unavailable_state);
    }
    const sao_status_t authority_status =
        sao::launcher::entity_builtin_action::authorization_status(action,
                                                                   ctx->builtin_action_state);
    if (authority_status != SAO_STATUS_OK) {
        ctx->builtin_action_state.last_status = authority_status;
        return authority_status;
    }
    switch (action) {
    case SAO_UI_ENTITY_ACTION_OPEN_ABOUT:
        break;
    case SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST:
    case SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE:
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL:
    case SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE:
    case SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP:
    case SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR:
    case SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER:
    case SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS:
    case SAO_UI_ENTITY_ACTION_PLUGIN_STATUS: {
        sao::launcher::entity_builtin_action::Operations operations;
        // apply_topmost_mode intentionally stays nullptr: the headless
        // launcher has no owned z-order manager (see the authority.topmost
        // = false comment in sao_platform_bringup). authorization_status()
        // gates SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST closed via
        // authority.topmost, so dispatch() never reaches toggle_topmost().
        // If a future change promotes a z-order manager onto sao_platform_ctx,
        // wire the apply function here alongside authority.topmost = true.
        operations.persist_topmost_mode = &persist_topmost_mode;
        operations.apply_streaming_mode = &apply_streaming_mode;
        operations.persist_streaming_mode = &persist_streaming_mode;
        operations.reload_plugins = &reload_plugins;
        operations.refresh_entity = &refresh_entity;
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
        operations.open_workshop = &open_workshop;
        operations.open_process_selector = &open_process_selector;
        operations.open_plugin_manager = &open_plugin_manager;
        operations.open_plugin_status = &open_plugin_status;
        operations.set_fisheye_procedural = &set_fisheye_procedural;
        operations.set_fisheye_live = &set_fisheye_live;
#endif
        operations.user_data = ctx;
        const sao_status_t status = sao::launcher::entity_builtin_action::dispatch(
            action, ctx->builtin_action_state, operations);
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
        if (ctx->builtin_action_state.controls_degraded) {
            ctx->entity_provider_publication.builtin_authority.controls = sao::launcher::
                entity_provider_publication::ControlPublicationStatus::degraded_internal;
            (void)refresh_entity(ctx);
        }
#endif
        return status;
    }
    case SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR: {
        if (ctx == nullptr || ctx->entity_shell == nullptr || !ctx->settings_owner) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        return applyNervgearModeTransaction(
            ctx->nervgear_mode,
            [](bool enabled, void* context) {
                return sao_ui_entity_shell_set_nervgear_mode(
                    static_cast<sao_platform_ctx*>(context)->entity_shell, enabled);
            },
            [](bool enabled, void* context) {
                return static_cast<sao_platform_ctx*>(context)->settings_owner->set_value_and_save(
                    "nervgear_mode", enabled);
            },
            &publish_nervgear_degraded, ctx);
    }
    case SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR: {
        if (ctx == nullptr || !ctx->ai_editor) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        // The callback reports request acceptance. The detached owner records
        // completion state and emits asynchronous failures through core logs.
        return sao::launcher::tool_launch::open_ai_editor(ctx->ai_editor.get());
    }
    case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS: {
        if (ctx == nullptr || !ctx->settings_owner) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        return ctx->settings_owner->save();
    }
    case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
    case SAO_UI_ENTITY_ACTION_SET_ALL_DARK: {
        if (ctx == nullptr || !ctx->settings_owner) {
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        const auto theme = action == SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT
                               ? sao::launcher::settings_theme::PanelTheme::light
                               : sao::launcher::settings_theme::PanelTheme::dark;
        const sao_status_t status =
            sao::launcher::settings_theme::replace_all_panel_themes(*ctx->settings_owner, theme);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        return sao_ui_theme_set_active_id(runtime_theme_id(theme));
    }
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    HWND owner = ctx == nullptr || ctx->overlay_host == nullptr
                     ? nullptr
                     : static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    if (sao::launcher::openUserDocsIndex(SaoLauncherBaseDir, owner)) {
        return SAO_STATUS_OK;
    }
    MessageBoxW(owner, L"用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。", L"SAO Auto",
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
    return SAO_STATUS_ERR_NOT_FOUND;
}

sao_status_t sao_platform_bringup(const sao_platform_config* cfg, sao_platform_ctx** ctx_out) {
    if (!cfg || !ctx_out || !cfg->base_dir)
        return SAO_STATUS_INVALID_ARGUMENT;
    *ctx_out = nullptr;

    (void)SetEnvironmentVariableW(L"SAO_BASE_DIR", cfg->base_dir);
    auto* ctx = new (std::nothrow) sao_platform_ctx{};
    if (!ctx)
        return SAO_STATUS_INTERNAL;

    sao_status_t status = create_ai_editor_owner(cfg->base_dir, ctx->ai_editor);
    if (status != SAO_STATUS_OK) {
        delete ctx;
        return status;
    }
    status = create_settings_owner(cfg->base_dir, ctx->settings_owner);
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
    status = ctx->settings_owner->get_truthy("nervgear_mode", true, ctx->nervgear_mode);
    if (status != SAO_STATUS_OK) {
        delete ctx;
        return status;
    }
    bool persisted_streaming_mode = false;
    status = ctx->settings_owner->get_truthy("streaming_mode", true, persisted_streaming_mode);
    if (status != SAO_STATUS_OK) {
        delete ctx;
        return status;
    }
    ctx->builtin_action_state.topmost = false;
    ctx->builtin_action_state.streaming_entitled = cfg->streaming_entitled != 0;
    ctx->builtin_action_state.streaming_mode =
        ctx->builtin_action_state.streaming_entitled && persisted_streaming_mode;
    auto& action_authority = ctx->builtin_action_state.authority;
    action_authority.publication_available = true;
    action_authority.controls = true;
    action_authority.nervgear = true;
    action_authority.streaming = ctx->builtin_action_state.streaming_entitled;
    action_authority.save_settings = true;
    action_authority.ai_editor = sao::launcher::tool_launch::ai_editor_capability_available();
    action_authority.workshop = false;
    action_authority.process_selector = false;
    action_authority.plugin_manager = false;
    action_authority.reload_plugins = false;
    action_authority.plugin_status = false;
    action_authority.fisheye_procedural = false;
    action_authority.fisheye_live = false;
    action_authority.theme = true;
    action_authority.about = true;
    // Topmost toggling is a compositor-thread z-order operation performed by
    // sao_ui_z_order_manager. The headless launcher pipeline does not own a
    // z-order manager (only the shipping compositor does), so there is no
    // apply function to wire in the entity_action callback below. Leaving
    // authority.topmost = false explicitly keeps the toggle out of the
    // published menu until a future integration promotes the manager into
    // the platform ctx and provides an apply hook. See toggle_topmost() in
    // entity_builtin_action_internal.cpp — a nullptr apply_topmost_mode
    // would otherwise short-circuit to SAO_STATUS_ERR_NOT_INITIALIZED.
    action_authority.topmost = false;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    ctx->entity_provider_publication.topmost = ctx->builtin_action_state.topmost;
    ctx->entity_provider_publication.streaming_mode = ctx->builtin_action_state.streaming_mode;
#endif
    sao::launcher::settings_theme::PanelTheme restored_theme{};
    status =
        sao::launcher::settings_theme::read_process_theme(*ctx->settings_owner, restored_theme);
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

    status =
        sao_rt_io_window_rect_controller_create(ctx->rt_io_proxy, &ctx->window_rect_controller);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }

    SaoUiDcMutationProviderV2 dc_mutation_provider{};
    dc_mutation_provider.struct_size = sizeof(dc_mutation_provider);
    dc_mutation_provider.hide_window_rect = &hide_window_rect;
    dc_mutation_provider.hide_exstyle = &hide_exstyle;
    dc_mutation_provider.user_data = ctx;
    status = sao_ui_dc_mutation_coordinator_create_ex_v2(&dc_mutation_provider,
                                                         &ctx->dc_mutation_coordinator);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }

    SaoOverlayHostConfig overlay_cfg{};
    overlay_cfg.dc_mutation_coordinator = ctx->dc_mutation_coordinator;
    status = sao_ui_overlay_host_create(&overlay_cfg, &ctx->overlay_host);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    const auto render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    if (render_hwnd == nullptr) {
        return rollback_platform_bringup(ctx, ctx_out, SAO_STATUS_ERR_HANDLE_INVALID);
    }
    status = sao_ui_compositor_create(ctx->overlay_host, nullptr, &ctx->compositor);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    status = map_sdk_runtime_status(sao_sdk_platform_bind_ui_compositor(ctx->compositor));
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    ctx->sdk_compositor_bound = true;
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    status = create_shared_ui_owners(cfg->base_dir, ctx);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    action_authority.workshop = ctx->workshop_panel != nullptr;
    action_authority.process_selector = ctx->process_selector_panel != nullptr;
    action_authority.fisheye_procedural = ctx->fisheye_backdrop != nullptr;
    action_authority.fisheye_live = ctx->fisheye_backdrop != nullptr;
#endif
    status = sao_rt_io_window_rect_register(
        ctx->window_rect_controller,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(render_hwnd)), &ctx->window_rect_token);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    ctx->window_rect_registered = true;

    status = sao_streaming_flow_startup(2.0);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    ctx->streaming_flow_started = true;

    status = apply_streaming_mode(ctx->builtin_action_state.streaming_mode, ctx);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    SaoUiEntityShellConfig entity_cfg{};
    entity_cfg.action_fn = &entity_action;
    entity_cfg.action_user_data = ctx;
    status =
        sao_ui_entity_shell_create_on_compositor(ctx->compositor, &entity_cfg, &ctx->entity_shell);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
    status = sao_ui_entity_shell_set_nervgear_mode(ctx->entity_shell, ctx->nervgear_mode);
    if (status != SAO_STATUS_OK) {
        return rollback_platform_bringup(ctx, ctx_out, status);
    }
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    auto& authority = ctx->entity_provider_publication.builtin_authority;
    sync_entity_publication_authority(ctx);
    authority.topmost_status = sao::launcher::entity_provider_publication::
        TopmostPublicationStatus::degraded_authority_unavailable;
#endif
    ctx->restore_theme_on_rollback = false;
    ctx->settings_save_enabled = true;
    *ctx_out = ctx;
    return SAO_STATUS_OK;
}

sao_status_t teardown_platform_context(sao_platform_ctx* ctx, bool save_settings) noexcept {
    if (!ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (ctx->ai_editor) {
        const sao_status_t ai_editor_status = ctx->ai_editor->take_offline();
        if (ai_editor_status != SAO_STATUS_OK) {
            return ai_editor_status;
        }
        ctx->ai_editor.reset();
    }
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    const sao_status_t shared_ui_status = retire_shared_ui_owners(ctx);
    if (shared_ui_status != SAO_STATUS_OK)
        return shared_ui_status;
#endif
    if (ctx->insert_hotkey_registered) {
        (void)UnregisterHotKey(nullptr, kInsertHotkeyId);
        ctx->insert_hotkey_registered = false;
    }
    if (ctx->home_hotkey_registered) {
        (void)UnregisterHotKey(nullptr, kHomeHotkeyId);
        ctx->home_hotkey_registered = false;
    }
    if (ctx->streaming_flow_started) {
        const sao_status_t streaming_status = sao_streaming_flow_teardown(2.0, 2.0);
        if (streaming_status != SAO_STATUS_OK) {
            return streaming_status;
        }
        ctx->streaming_flow_started = false;
    }
    if (ctx->entity_shell) {
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
        const sao_status_t clear_status = sao::launcher::entity_provider_publication::clear(
            ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
            ctx->nervgear_mode, &sao_ui_entity_shell_set_roots);
        if (clear_status != SAO_STATUS_OK) {
            return clear_status;
        }
#endif
        const sao_status_t entity_status = sao_ui_entity_shell_try_destroy(ctx->entity_shell);
        if (entity_status != SAO_STATUS_OK)
            return entity_status;
        ctx->entity_shell = nullptr;
    }
    if (ctx->sdk_compositor_bound) {
        const sao_sdk_status_t unbind_status = sao_sdk_platform_unbind_ui_compositor();
        if (unbind_status != SAO_SDK_OK)
            return map_sdk_runtime_status(unbind_status);
        ctx->sdk_compositor_bound = false;
    }
    if (ctx->compositor) {
        const sao_status_t compositor_status = sao_ui_compositor_try_destroy(ctx->compositor);
        if (compositor_status != SAO_STATUS_OK)
            return compositor_status;
        ctx->compositor = nullptr;
    }
    if (ctx->overlay_host) {
        if (!sao_ui_overlay_host_destroy(ctx->overlay_host)) {
            return SAO_STATUS_INTERNAL;
        }
        ctx->overlay_host = nullptr;
    }
    if (ctx->dc_mutation_coordinator) {
        sao_ui_dc_mutation_coordinator_destroy(ctx->dc_mutation_coordinator);
        ctx->dc_mutation_coordinator = nullptr;
    }
    if (ctx->window_rect_registered) {
        const sao_status_t revoke_status =
            sao_rt_io_window_rect_revoke(ctx->window_rect_controller, &ctx->window_rect_token);
        if (revoke_status != SAO_STATUS_OK) {
            return revoke_status;
        }
        ctx->window_rect_registered = false;
        ctx->window_rect_token = {};
    }
    if (ctx->window_rect_controller) {
        sao_rt_io_window_rect_controller_destroy(ctx->window_rect_controller);
        ctx->window_rect_controller = nullptr;
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
    if (!ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    return teardown_platform_context(ctx, ctx->settings_save_enabled);
}

sao_status_t sao_platform_bind_plugins(sao_platform_ctx* ctx, sao_plugins_registry* registry) {
    if (ctx == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    ctx->plugins_registry = registry;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    return sync_plugin_runtime_authority(ctx);
#else
    return SAO_STATUS_OK;
#endif
}

sao_status_t sao_ui_bring_online(sao_platform_ctx* ctx) {
    if (!ctx || !ctx->overlay_host || !ctx->compositor || !ctx->entity_shell) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    sao_status_t status = SAO_STATUS_OK;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    status = publishEntityAuthorityBeforeOnline(
        [](void* context) { return refresh_entity(context); },
        [](void* context) {
            return sao_ui_entity_shell_bring_online(
                static_cast<sao_platform_ctx*>(context)->entity_shell);
        },
        ctx);
    if (status != SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
        (void)sao_core_logf(SAO_LOG_WARN, "launcher.entity_provider",
                            "initial catalog publication failed: status=%d", status);
#endif
        return status;
    }
#else
    status = sao_ui_entity_shell_bring_online(ctx->entity_shell);
    if (status != SAO_STATUS_OK)
        return status;
#endif
    status = tick_shared_fisheye(ctx);
    if (status == SAO_STATUS_OK)
        status = sao_ui_compositor_tick(ctx->compositor);
    if (status != SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
        (void)sao::launcher::entity_provider_publication::clear(
            ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
            ctx->nervgear_mode, &sao_ui_entity_shell_set_roots);
#endif
        (void)sao_ui_entity_shell_take_offline(ctx->entity_shell);
        return status;
    }
    if (!RegisterHotKey(nullptr, kHomeHotkeyId, MOD_NOREPEAT, VK_HOME)) {
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
        (void)sao::launcher::entity_provider_publication::clear(
            ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
            ctx->nervgear_mode, &sao_ui_entity_shell_set_roots);
#endif
        (void)sao_ui_entity_shell_take_offline(ctx->entity_shell);
        return SAO_STATUS_INTERNAL;
    }
    ctx->home_hotkey_registered = true;
    if (!RegisterHotKey(nullptr, kInsertHotkeyId, MOD_NOREPEAT, VK_INSERT)) {
        UnregisterHotKey(nullptr, kHomeHotkeyId);
        ctx->home_hotkey_registered = false;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
        (void)sao::launcher::entity_provider_publication::clear(
            ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
            ctx->nervgear_mode, &sao_ui_entity_shell_set_roots);
#endif
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
    // AiEditorProcessOwner::take_offline() is owned by teardown_platform_context
    // (invoked by sao_platform_teardown after this function returns) so we do
    // NOT drive it a second time here.  Calling take_offline twice was
    // idempotent but produced churn on the ai_editor state machine on every
    // normal shutdown.
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    const sao_status_t shared_ui_status = retire_shared_ui_owners(ctx);
    if (shared_ui_status != SAO_STATUS_OK)
        return shared_ui_status;
#endif
    sao_status_t status = SAO_STATUS_OK;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    // entity_provider_publication::clear() calls close_invocation_gate()
    // itself as its first step; we do not close it separately here to avoid
    // duplicating the transition-in-progress guard against the same store.
    ctx->builtin_action_state.authority.publication_available = false;
    sync_entity_publication_authority(ctx);
    status = sao::launcher::entity_provider_publication::clear(
        ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
        ctx->nervgear_mode, &sao_ui_entity_shell_set_roots);
#endif
    if (ctx->insert_hotkey_registered) {
        if (!UnregisterHotKey(nullptr, kInsertHotkeyId)) {
            if (status == SAO_STATUS_OK)
                status = SAO_STATUS_INTERNAL;
        } else {
            ctx->insert_hotkey_registered = false;
        }
    }
    if (ctx->home_hotkey_registered) {
        if (!UnregisterHotKey(nullptr, kHomeHotkeyId)) {
            if (status == SAO_STATUS_OK)
                status = SAO_STATUS_INTERNAL;
        } else {
            ctx->home_hotkey_registered = false;
        }
    }
    const sao_status_t offline_status = sao_ui_entity_shell_take_offline(ctx->entity_shell);
    const sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
    if (status != SAO_STATUS_OK)
        return status;
    return offline_status == SAO_STATUS_OK ? compositor_status : offline_status;
}

sao_status_t sao_ui_tick(sao_platform_ctx* ctx, uint32_t elapsed_ms) {
    if (!ctx || !ctx->entity_shell || !ctx->compositor)
        return SAO_STATUS_INVALID_ARGUMENT;
    sao_status_t status = sao_ui_entity_shell_tick(ctx->entity_shell, elapsed_ms);
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    const sao_status_t provider_status =
        status == SAO_STATUS_OK && ctx->builtin_action_state.authority.publication_available
            ? sao::launcher::entity_provider_publication::poll(
                  ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
                  elapsed_ms, ctx->nervgear_mode,
                  &sao::plugins::loader::sao_plugins_entity_provider_snapshot_v2,
                  &sao::plugins::loader::sao_plugins_entity_provider_snapshot,
                  &sao_ui_entity_shell_set_roots)
            : (status == SAO_STATUS_OK ? refresh_entity(ctx) : status);
    if (provider_status != SAO_STATUS_OK) {
        ctx->builtin_action_state.authority.publication_available = false;
        sync_entity_publication_authority(ctx);
    }
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    if (provider_status != SAO_STATUS_OK) {
        (void)sao_core_logf(SAO_LOG_WARN, "launcher.entity_provider",
                            "catalog refresh deferred: status=%d", provider_status);
    }
#endif
    if (status == SAO_STATUS_OK && provider_status != SAO_STATUS_OK)
        status = provider_status;
#endif
    if (ctx->ai_editor) {
        const sao_status_t ai_editor_status = ctx->ai_editor->service_ui();
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
        if (ai_editor_status != SAO_STATUS_OK &&
            ai_editor_status != SAO_STATUS_ERR_CANCELLED) {
            (void)sao_core_logf(
                SAO_LOG_WARN, "launcher.ai_editor",
                "owner-thread UI service deferred: status=%d",
                ai_editor_status);
        }
#endif
        if (status == SAO_STATUS_OK &&
            ai_editor_status != SAO_STATUS_ERR_CANCELLED) {
            status = ai_editor_status;
        }
    }
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    if (ctx->workshop_panel) {
        const sao_status_t workshop_status = ctx->workshop_panel->service_ui();
        if (status == SAO_STATUS_OK && workshop_status != SAO_STATUS_OK)
            status = workshop_status;
    }
#endif
    const sao_status_t fisheye_status = tick_shared_fisheye(ctx);
    if (status == SAO_STATUS_OK && fisheye_status != SAO_STATUS_OK)
        status = fisheye_status;
    const sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
    return status == SAO_STATUS_OK ? compositor_status : status;
}

sao_status_t sao_ui_handle_message(sao_platform_ctx* ctx, uint32_t message, uintptr_t w_param,
                                   intptr_t, int32_t* out_handled) {
    if (!ctx || !ctx->entity_shell || !out_handled) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    *out_handled = 0;
    if (message == WM_TIMER && w_param == kUiFrameTimerId) {
        *out_handled = 1;
        return sao_ui_tick(ctx, kUiFrameIntervalMs);
    }
    if (message != WM_HOTKEY)
        return SAO_STATUS_OK;
    if (w_param == kHomeHotkeyId) {
        *out_handled = 1;
        const sao_status_t status = sao_ui_entity_shell_home(ctx->entity_shell);
        const sao_status_t fisheye_status =
            status == SAO_STATUS_OK ? tick_shared_fisheye(ctx) : status;
        const sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
        return status != SAO_STATUS_OK
                   ? status
                   : (fisheye_status == SAO_STATUS_OK ? compositor_status : fisheye_status);
    }
    if (w_param == kInsertHotkeyId) {
        *out_handled = 1;
        const sao_status_t status = sao_ui_entity_shell_insert(ctx->entity_shell);
        const sao_status_t fisheye_status =
            status == SAO_STATUS_OK ? tick_shared_fisheye(ctx) : status;
        const sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
        return status != SAO_STATUS_OK
                   ? status
                   : (fisheye_status == SAO_STATUS_OK ? compositor_status : fisheye_status);
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
sao_status_t sao_platform_bind_plugins(sao_platform_ctx*, sao_plugins_registry*) {
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
sao_status_t sao_ui_handle_message(sao_platform_ctx*, uint32_t, uintptr_t, intptr_t,
                                   int32_t* out_handled) {
    if (out_handled)
        *out_handled = 0;
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_security_init(const sao_security_config* cfg) {
    if (!g_composition_test_hooks.security_init) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.security_init(cfg, g_composition_test_hooks.user_data);
}
sao_status_t sao_security_shutdown(void) {
    if (g_composition_test_hooks.security_shutdown) {
        g_composition_test_hooks.security_shutdown(g_composition_test_hooks.user_data);
    }
    return SAO_STATUS_OK;
}
#elif defined(SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER)
sao_status_t sao_security_init(const sao_security_config* cfg) {
    if (!cfg)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (!cfg->enable_anti_debug)
        return SAO_STATUS_OK;

    uint32_t score = 0;
    sao_status_t status = sao_security_anti_debug_scan_all(&score);
    if (status != SAO_STATUS_OK)
        return status;
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
    if (!g_composition_test_hooks.shell_verify)
        return SAO_STATUS_NOT_IMPLEMENTED;
    return g_composition_test_hooks.shell_verify(out, g_composition_test_hooks.user_data);
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
        strncpy_s(out->reason, sizeof(out->reason), "shell integrity provider unavailable",
                  _TRUNCATE);
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_shell_shutdown(void) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_license_verify(sao_license_result* out) {
    if (!g_composition_test_hooks.license_verify)
        return SAO_STATUS_NOT_IMPLEMENTED;
    return g_composition_test_hooks.license_verify(out, g_composition_test_hooks.user_data);
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
        strncpy_s(out->error_msg, sizeof(out->error_msg), "license provider unavailable",
                  _TRUNCATE);
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_license_shutdown(void) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_status_t sao_plugins_discover(sao_platform_ctx* ctx, sao_plugins_registry** out) {
    if (!g_composition_test_hooks.plugins_discover) {
        if (out)
            *out = nullptr;
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.plugins_discover(ctx, out, g_composition_test_hooks.user_data);
}
sao_status_t sao_plugins_activate_autostart(sao_plugins_registry* registry) {
    return g_composition_test_hooks.plugins_activate_autostart
               ? g_composition_test_hooks.plugins_activate_autostart(
                     registry, g_composition_test_hooks.user_data)
               : SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_reload_all(sao_plugins_registry*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_status_snapshot(sao_plugins_registry*,
                                         sao_plugins_status_snapshot_t* out_status) {
    if (out_status != nullptr)
        *out_status = {};
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_shutdown(sao_plugins_registry* registry) {
    return g_composition_test_hooks.plugins_shutdown
               ? g_composition_test_hooks.plugins_shutdown(registry,
                                                           g_composition_test_hooks.user_data)
               : SAO_STATUS_NOT_IMPLEMENTED;
}
#elif !defined(SAO_LINKED_PLUGINS)
sao_status_t sao_plugins_discover(sao_platform_ctx*, sao_plugins_registry** out) {
    if (out)
        *out = nullptr;
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_activate_autostart(sao_plugins_registry*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_reload_all(sao_plugins_registry*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_status_snapshot(sao_plugins_registry*,
                                         sao_plugins_status_snapshot_t* out_status) {
    if (out_status != nullptr)
        *out_status = {};
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_plugins_shutdown(sao_plugins_registry*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#endif

} // extern "C"
