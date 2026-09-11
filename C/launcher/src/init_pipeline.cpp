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
#include "settings_config_panel.h"
#include "settings_owner_internal.h"
#include "settings_profiles.h"
#include "settings_theme_internal.h"
#include "tool_launch_internal.h"

#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION) &&                                              \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "license_panel_internal.h"
#include "plugin_manager_panel_internal.h"
#include "process_selector_panel_internal.h"
#include "workshop_panel_internal.h"
#endif

#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION) &&                                           \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "entity_provider_publication_internal.h"
#include "sao/plugins/loader/entity_provider.h"
#endif

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <windows.h>

#include <bcrypt.h>

#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/core/logging.h"
#endif

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) &&                                         \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#undef SAO_STATUS_OK
#include "sao/rt_io/engine_registry.h"
#include "sao/rt_io/helper_bootstrap.h"
#include "sao/rt_io/proxy.h"
#include "sao/rt_io/window_rect.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dc_mutation.h"
#include "sao/ui/dialog.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/input_router.h"
#include "sao/ui/linkstart_intro.h"
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
#include "sao/ui/fisheye_backdrop.h"
#include "sao/ui/panel_sdk.h"
#endif
#include "hotkey_config_panel.h"
#include "hotkey_manager.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/sound.h"
#include "sao/ui/streaming_flow.h"
#include "sao/ui/theme.h"

// Anti-screencap chains: capture-mode state machine, per-method availability
// registry, per-window registration, threat reaction and the tagWND-adjacent
// helpers.  Guarded so a platform-off build keeps the fail-closed stubs.
#if __has_include("sao_security/anti_screencap/capture_mode.h")
#include "sao_security/anti_screencap/capture_method_registry.h"
#include "sao_security/anti_screencap/capture_mode.h"
#include "sao_security/anti_screencap/dwm_thumbnail.h"
#include "sao_security/anti_screencap/kernel_sprite_protect.h"
#include "sao_security/anti_screencap/overlay_host_capture.h"
#include "sao_security/anti_screencap/syscall_affinity.h"
#define SAO_LAUNCHER_HAS_ANTI_SCREENCAP_CHAIN 1
#endif
#endif

#if defined(SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER) &&                                         \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "sao_security/anti_debug/probes.h"
#endif
#if defined(SAO_LAUNCHER_ANTI_DUMP_PROVIDER) && !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "sao_security/anti_dump/erase_headers.h"
#include "sao_security/anti_dump/snapshot_detect.h"
#endif
#if __has_include("sao_security/anti_screencap/capture_mode.h")
#include "sao_security/anti_screencap/capture_mode.h"
#define SAO_LAUNCHER_HAS_ANTI_SCREENCAP_API 1
#endif
#if defined(SAO_LAUNCHER_USER_EVASION_PROVIDER) && !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
#include "sao_security/user_evasion/api.h"
#include "sao_security/user_evasion/halo_gate.h"
#include "sao_security/user_evasion/indirect_syscall.h"
#include "sao_security/user_evasion/unhook.h"
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

namespace {
std::wstring g_dynamic_base_dir;
}

extern "C" void sao_launcher_set_base_dir(const wchar_t* base_dir) {
    g_dynamic_base_dir = base_dir == nullptr ? std::wstring{} : std::wstring(base_dir);
    SaoLauncherBaseDir[0] = wchar_t(0);
    if (g_dynamic_base_dir.size() + 1u <= std::size(SaoLauncherBaseDir))
        std::wmemcpy(SaoLauncherBaseDir, g_dynamic_base_dir.c_str(),
                     g_dynamic_base_dir.size() + 1u);
}

extern "C" const wchar_t* sao_launcher_base_dir(void) {
    return g_dynamic_base_dir.c_str();
}

extern "C" uint32_t sao_launcher_base_dir_length(void) {
    return g_dynamic_base_dir.size() >= UINT32_MAX
               ? UINT32_MAX
               : static_cast<uint32_t>(g_dynamic_base_dir.size());
}

extern "C" sao_status_t sao_launcher_copy_base_dir(wchar_t* out_dir, uint32_t* inout_char_count) {
    if (inout_char_count == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    const uint32_t required = sao_launcher_base_dir_length() + 1u;
    if (out_dir == nullptr || *inout_char_count < required) {
        *inout_char_count = required;
        return out_dir == nullptr ? SAO_STATUS_OK : SAO_STATUS_INVALID_ARGUMENT;
    }
    std::wmemcpy(out_dir, g_dynamic_base_dir.c_str(), required);
    *inout_char_count = required;
    return SAO_STATUS_OK;
}

// File-scope glue for the runtime installer hook. Lives in a real named
// namespace so both the anonymous-namespace pipeline caller and the
// extern-"C" platform composition provider (where sao_platform_ctx is a
// complete type) can reference the hook types without dancing around
// anonymous-namespace internal linkage rules.
//
// Contract:
//   * ProgressFn is invoked at least once per runtime being installed.
//   * EnsureAllFn returns SAO_STATUS_OK when every listed runtime is
//     already present or was fetched/verified successfully; any other
//     value flags the Panel authority mirror as unavailable.
//   * g_record_outcome is non-null only when a platform composition
//     provider with a complete sao_platform_ctx type installs a concrete
//     recorder at TU load time; stays null under the fail-closed and
//     test build variants so the runtime installer step still runs but
//     its Panel authority mirror is skipped.
namespace sao::launcher::runtime_installer_glue {

using ProgressFn = void (*)(const char* kind_opaque_id_utf8, uint64_t bytes_done,
                            uint64_t bytes_total, void* user_data);
using EnsureAllFn = sao_status_t (*)(const wchar_t* base_dir, ProgressFn progress_cb,
                                     void* progress_user_data);
using RecordOutcomeFn = void (*)(void* platform_ctx, bool ok) noexcept;

inline RecordOutcomeFn g_record_outcome = nullptr;

} // namespace sao::launcher::runtime_installer_glue

namespace sao::launcher {

static bool actualDebugNoLicenseRequested(const AppState& state) noexcept {
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
    return state.no_license;
#else
    (void)state;
    return false;
#endif
}

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
    config.base_dir = state.base_dir.c_str();
    config.config_path = state.config_path.empty() ? nullptr : state.config_path.c_str();
    config.log_level = log_level_storage;
    config.safe_mode = state.safe_mode ? 1 : 0;
    const bool no_license_bypass = actualDebugNoLicenseRequested(state);
    config.streaming_entitled =
        !state.safe_mode && (no_license_bypass || state.streaming_entitled) ? 1 : 0;
    config.rt_io_operator = state.rt_io_operator ? 1 : 0;
    config.rt_io_dev_license_bypass = no_license_bypass ? 1 : 0;
    config.rt_io_force_status_page = state.rt_io_force_status_page ? 1 : 0;
    return true;
}

} // namespace sao::launcher

namespace {

constexpr UINT_PTR kUiFrameTimerId = 0x53415549U;
constexpr int32_t kUiFrameIntervalMs = 16;
constexpr int kMaximumTeardownAttempts = 3;
constexpr int kMaximumStreamingReleaseAttempts = 2;
constexpr uint64_t kRtIoMandatoryLiveStepMask = ((uint64_t{1} << 9u) - 1u) | (uint64_t{1} << 13u) |
                                                (uint64_t{1} << 14u) | (uint64_t{1} << 15u) |
                                                (uint64_t{1} << 16u);
constexpr uint32_t kRtIoObservationUnknown = 0u;
constexpr uint32_t kRtIoObservationTrue = 2u;
constexpr uint32_t kRtIoFailureStageNone = 0u;
constexpr size_t kRtIoOperatorJsonCapacity =
    2u * SAO_LAUNCHER_RT_IO_STRICT_HELPER_IMAGE_CAPACITY * 6u + 8192u;
constexpr DWORD kEnvironmentValueCapacity = 32768u;

// tagWND / capture-shield chain constants.  The decoy rect and the ExStyle mask
// mirror the Python authoritative sources: `_dc.hide_window_rect(hwnd,0,0,1,1)`
// and `_dc.OVERLAY_EXSTYLE_MASK` (mem_probe/_dc.py L794) as consumed by
// `render/overlay_host.py` (L465, L1004).
constexpr SaoUiDcMutationRect kCaptureShieldRectScrub{0, 0, 1, 1};
constexpr uint32_t kCaptureShieldScrubSettleMs = 40;
constexpr uint32_t kCaptureShieldScrubTimeoutMs = 2000;
constexpr uint32_t kOverlayExstyleScrubMask = 0x00000008u | 0x00000020u | 0x00000080u |
                                              0x00200000u | 0x08000000u;
constexpr uint32_t kCaptureShieldDrainAttempts = 40u;
constexpr DWORD kCaptureShieldDrainSleepMs = 25u;

class EnvironmentVariableRollback {
  public:
    explicit EnvironmentVariableRollback(const wchar_t* name) noexcept : name_(name) {
        if (name_ == nullptr || !*name_)
            return;

        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(name_, previous_value_.data(),
                                                     static_cast<DWORD>(previous_value_.size()));
        if (length == 0u) {
            const DWORD error = GetLastError();
            previous_exists_ = error != ERROR_ENVVAR_NOT_FOUND;
            valid_ = error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND;
            return;
        }
        if (length >= previous_value_.size())
            return;
        previous_exists_ = true;
        valid_ = true;
    }

    ~EnvironmentVariableRollback() {
        if (!committed_)
            (void)restore();
    }

    bool valid() const noexcept {
        return valid_;
    }

    bool set(const wchar_t* value) noexcept {
        return valid_ && value != nullptr && SetEnvironmentVariableW(name_, value) != FALSE;
    }

    void commit() noexcept {
        committed_ = true;
    }

    bool restore() noexcept {
        if (!valid_)
            return false;
        return SetEnvironmentVariableW(name_, previous_exists_ ? previous_value_.data()
                                                               : nullptr) != FALSE;
    }

  private:
    const wchar_t* name_ = nullptr;
    std::array<wchar_t, kEnvironmentValueCapacity> previous_value_{};
    bool previous_exists_ = false;
    bool valid_ = false;
    bool committed_ = false;
};

using PluginAuthoritySyncFn = sao_status_t (*)(void* user_data);

sao_status_t bindPluginsWithAuthority(sao_plugins_registry*& current,
                                      sao_plugins_registry* candidate, PluginAuthoritySyncFn sync,
                                      void* user_data) noexcept {
    sao_plugins_registry* previous = current;
    current = candidate;
    const sao_status_t status = sync != nullptr ? sync(user_data) : SAO_STATUS_OK;
    if (status != SAO_STATUS_OK)
        current = previous;
    return status;
}

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER) &&                                         \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
static_assert(kRtIoObservationUnknown == SAO_RT_IO_OBSERVATION_UNKNOWN);
static_assert(kRtIoObservationTrue == SAO_RT_IO_OBSERVATION_TRUE);
static_assert(kRtIoFailureStageNone == SAO_RT_IO_FAILURE_STAGE_NONE);
#endif

bool rtIoOperatorPreflightObservationsComplete(uint32_t is_admin, uint32_t is_elevated,
                                               uint32_t load_driver_privilege_present,
                                               uint32_t load_driver_privilege_enabled,
                                               uint32_t hvci_enabled, uint32_t vbs_enabled,
                                               uint32_t provider_observable) noexcept {
    return is_admin == kRtIoObservationTrue && is_elevated == kRtIoObservationTrue &&
           load_driver_privilege_present == kRtIoObservationTrue &&
           load_driver_privilege_enabled != kRtIoObservationUnknown &&
           hvci_enabled != kRtIoObservationUnknown && vbs_enabled != kRtIoObservationUnknown &&
           provider_observable == kRtIoObservationTrue;
}

bool rtIoOperatorPreflightFailureStateReady(int32_t last_failure_code,
                                            uint32_t last_failure_stage) noexcept {
    return last_failure_stage == kRtIoFailureStageNone &&
           (last_failure_code == SAO_STATUS_OK ||
            last_failure_code == SAO_STATUS_ERR_NOT_INITIALIZED);
}

bool rtIoOperatorOptionsValid(const sao_launcher_rt_io_operator_options_t* options) noexcept {
    return options != nullptr && options->struct_size == sizeof(*options) &&
           options->reserved == 0u;
}

uint64_t requestedRtIoLiveStepMask(const sao_launcher_rt_io_operator_options_t& options) noexcept {
    uint64_t mask = kRtIoMandatoryLiveStepMask;
    if (options.input_checks != 0u) {
        mask |= uint64_t{1} << 9u;
        mask |= uint64_t{1} << 10u;
    }
    if (options.r5_check != 0u)
        mask |= uint64_t{1} << 11u;
    if (options.mf_check != 0u)
        mask |= uint64_t{1} << 12u;
    return mask;
}

void initializeRtIoOperatorReport(const sao_launcher_rt_io_operator_options_t* options,
                                  uint32_t stage,
                                  sao_launcher_rt_io_operator_report_t* report) noexcept {
    if (report == nullptr)
        return;
    *report = {};
    report->struct_size = sizeof(*report);
    report->stage = stage;
    report->status = SAO_STATUS_INTERNAL;
    report->operation_status = SAO_STATUS_INTERNAL;
    report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_NOT_SUBMITTED;
    if (options == nullptr)
        return;
    report->preflight_only = options->preflight_only != 0u ? 1u : 0u;
    report->input_checks = options->input_checks != 0u ? 1u : 0u;
    report->r5_check = options->r5_check != 0u ? 1u : 0u;
    report->mf_check = options->mf_check != 0u ? 1u : 0u;
    report->exit_after_validation = options->exit_after_validation != 0u ? 1u : 0u;
    report->requested_step_mask = requestedRtIoLiveStepMask(*options);
}

const char* rtIoOperatorStageName(uint32_t stage) noexcept {
    switch (stage) {
    case SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT:
        return "preflight";
    case SAO_LAUNCHER_RT_IO_STAGE_INIT:
        return "init";
    case SAO_LAUNCHER_RT_IO_STAGE_LIVE:
        return "live";
    case SAO_LAUNCHER_RT_IO_STAGE_STATUS:
        return "status";
    case SAO_LAUNCHER_RT_IO_STAGE_CLEANUP:
        return "cleanup";
    default:
        return "unknown";
    }
}

const char* rtIoOperatorFailureName(uint32_t failure) noexcept {
    switch (failure) {
    case SAO_LAUNCHER_RT_IO_FAILURE_NONE:
        return "none";
    case SAO_LAUNCHER_RT_IO_FAILURE_NOT_SUBMITTED:
        return "not_submitted";
    case SAO_LAUNCHER_RT_IO_FAILURE_CALL:
        return "call";
    case SAO_LAUNCHER_RT_IO_FAILURE_PREFLIGHT_INCOMPLETE:
        return "preflight_incomplete";
    case SAO_LAUNCHER_RT_IO_FAILURE_INIT_INCOMPLETE:
        return "init_incomplete";
    case SAO_LAUNCHER_RT_IO_FAILURE_LIVE_INCOMPLETE:
        return "live_incomplete";
    case SAO_LAUNCHER_RT_IO_FAILURE_STATUS_INCONSISTENT:
        return "status_inconsistent";
    case SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE:
        return "cleanup_incomplete";
    default:
        return "unknown";
    }
}

const char* jsonBool(uint32_t value) noexcept {
    return value != 0u ? "true" : "false";
}

void appendJsonEscaped(std::string& json, const char* text, size_t text_capacity) {
    static constexpr char kHex[] = "0123456789abcdef";
    json.push_back('"');
    if (text != nullptr) {
        for (size_t index = 0u; index < text_capacity && text[index] != '\0'; ++index) {
            const unsigned char value = static_cast<unsigned char>(text[index]);
            switch (value) {
            case '"':
                json.append("\\\"");
                break;
            case '\\':
                json.append("\\\\");
                break;
            case '\b':
                json.append("\\b");
                break;
            case '\f':
                json.append("\\f");
                break;
            case '\n':
                json.append("\\n");
                break;
            case '\r':
                json.append("\\r");
                break;
            case '\t':
                json.append("\\t");
                break;
            default:
                if (value < 0x20u) {
                    json.append("\\u00");
                    json.push_back(kHex[value >> 4u]);
                    json.push_back(kHex[value & 0x0fu]);
                } else {
                    json.push_back(static_cast<char>(value));
                }
                break;
            }
        }
    }
    json.push_back('"');
}

struct HeadlessCleanupState {
    HeadlessCleanupState() : base_dir_environment(L"SAO_BASE_DIR") {
        previous_base_dir_dynamic = sao_launcher_base_dir();
        std::memcpy(previous_base_dir, SaoLauncherBaseDir, sizeof(previous_base_dir));
        (void)sao::launcher::getCrashDumpDirectory(previous_crash_dir);
    }

    ~HeadlessCleanupState() {
        if (dual_run_driver_acquired && dual_run_driver_mutex != nullptr) {
            sao_launcher_dual_run_release_driver_mutex(dual_run_driver_mutex);
        }
        if (mutex_acquired && single_instance_mutex != nullptr) {
            sao::launcher::releaseOwnedSingleInstanceMutex(single_instance_mutex);
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
    EnvironmentVariableRollback base_dir_environment;
    wchar_t previous_base_dir[260] = {0};
    std::wstring previous_base_dir_dynamic;
    std::wstring previous_crash_dir = L".";
    bool launcher_globals_published = false;
};

bool restoreHeadlessProcessGlobals(HeadlessCleanupState& cleanup) noexcept {
    const bool environment_restored = cleanup.base_dir_environment.restore();
    if (environment_restored)
        cleanup.base_dir_environment.commit();
    if (cleanup.launcher_globals_published) {
        sao_launcher_set_base_dir(cleanup.previous_base_dir_dynamic.empty()
                                      ? cleanup.previous_base_dir
                                      : cleanup.previous_base_dir_dynamic.c_str());
        sao::launcher::setCrashDumpDirectory(cleanup.previous_crash_dir.c_str());
        cleanup.launcher_globals_published = false;
    }
    return environment_restored;
}

std::mutex g_pending_cleanup_mutex;
std::unique_ptr<HeadlessCleanupState> g_pending_cleanup;
bool publishLauncherBaseDir(const std::wstring& base_dir) noexcept {
    if (base_dir.empty())
        return false;
    sao_launcher_set_base_dir(base_dir.c_str());
    return true;
}

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
sao_launcher_composition_test_hooks_t g_composition_test_hooks{};
#endif

// ---------------------------------------------------------------------------
// Runtime installer hook (installer-core track owns the real ABI)
//
// The launcher does not include installer headers directly. The installer
// track exposes sao_runtime_installer_ensure_all(...) once landed; until
// then (and for tests) we route through a function pointer that ships as
// pass-through no-op. Tests inject a mock via the setter below to observe
// the pre-plugins call site without dragging in the real installer.
//
// Signatures mirror the ABI the installer-core track publishes:
//   * progress_cb receives (kind_opaque_id_utf8, bytes_done, bytes_total,
//     user_data) at least once per runtime being installed. The opaque id
//     is the manifest ``kind`` field (dotnet_runtime, lua_source,
//     angelscript_source, ...); the launcher does not interpret it beyond
//     handing it to the progress overlay for display.
//   * Returning any non-zero status short-circuits plugin discovery and
//     records the failure in EntityBuiltinAuthorityState::runtime_installer
//     so the Panel shows plugin runtimes as unavailable.
// ---------------------------------------------------------------------------

// Pass-through default. When SAO_PLUGINS_ENABLE_RUNTIME_AUTOINSTALL is
// OFF (default in windows-hardened), or when the installer-core track has
// not been linked in yet, this returns OK immediately so plugin hosts are
// left to their pre-installed runtimes.
sao_status_t
runtime_installer_ensure_all_passthrough(const wchar_t* /*base_dir*/,
                                         sao::launcher::runtime_installer_glue::ProgressFn
                                         /*progress_cb*/,
                                         void* /*progress_user_data*/) {
    return SAO_STATUS_OK;
}

std::mutex g_runtime_installer_hook_mutex;
sao::launcher::runtime_installer_glue::EnsureAllFn g_runtime_installer_ensure_all_hook =
    &runtime_installer_ensure_all_passthrough;
bool g_runtime_installer_hook_is_production = false;

struct RuntimeInstallerHookSnapshot {
    sao::launcher::runtime_installer_glue::EnsureAllFn ensure_all = nullptr;
    bool production = false;
};

RuntimeInstallerHookSnapshot current_runtime_installer_hook() {
    std::lock_guard lock(g_runtime_installer_hook_mutex);
    return {
        g_runtime_installer_ensure_all_hook == nullptr ? &runtime_installer_ensure_all_passthrough
                                                       : g_runtime_installer_ensure_all_hook,
        g_runtime_installer_hook_is_production,
    };
}

#if defined(SAO_LAUNCHER_HAS_RUNTIME_INSTALLER)
// When the installer-core track is linked in, its ABI provides a real
// implementation of ensure_all(). We swap the default hook at TU init
// time so the launcher's pipeline picks it up without every call site
// having to touch the setter. Tests still override via
// sao_launcher_init_pipeline_test_set_runtime_installer_hook.
extern "C" sao_status_t
sao_runtime_installer_ensure_all(const wchar_t* base_dir,
                                 sao::launcher::runtime_installer_glue::ProgressFn progress_cb,
                                 void* progress_user_data);
struct sao_runtime_manifest_s;
using sao_runtime_manifest_handle_t = sao_runtime_manifest_s*;
extern "C" sao_status_t
sao_runtime_installer_load_manifest(const char* manifest_json_utf8, size_t manifest_json_length,
                                    sao_runtime_manifest_handle_t* out_handle);
extern "C" void sao_runtime_installer_manifest_release(sao_runtime_manifest_handle_t handle);
extern "C" sao_status_t sao_runtime_installer_bind_manifest(sao_runtime_manifest_handle_t handle);

struct RuntimeInstallerHookInstall {
    RuntimeInstallerHookInstall() noexcept {
        std::lock_guard lock(g_runtime_installer_hook_mutex);
        g_runtime_installer_ensure_all_hook = &sao_runtime_installer_ensure_all;
        g_runtime_installer_hook_is_production = true;
    }
};
RuntimeInstallerHookInstall g_runtime_installer_hook_install;
#endif

// Progress callback context. The launcher forwards each progress tick to
// the compositor-native progress overlay owned by sao_ui_*; when the
// overlay is unavailable (fail-closed platform composition) the tick is
// silently dropped so the installer path still runs.
struct RuntimeInstallerProgressContext {
    sao_platform_ctx* platform = nullptr;
    // Opaque handle owned by the compositor progress overlay; nullptr
    // when the overlay is unavailable in this build configuration.
    void* overlay_handle = nullptr;
};

void forward_runtime_installer_progress(const char* kind_opaque_id_utf8, uint64_t bytes_done,
                                        uint64_t bytes_total, void* user_data) {
    // Silent when the overlay handle is nullptr; the installer still runs
    // to completion, the user just gets no visible progress bar. The
    // production overlay wiring lands alongside the installer-core track;
    // for now the launcher's job is to plumb the ticks through so tests
    // can observe the call sequence.
    auto* ctx = static_cast<RuntimeInstallerProgressContext*>(user_data);
    if (ctx == nullptr) {
        return;
    }
    // Guard against pathological callbacks that hand back nullptr in the
    // kind slot — the compositor overlay contract requires a stable string
    // for the label so we swap in a placeholder rather than crashing.
    (void)kind_opaque_id_utf8;
    (void)bytes_done;
    (void)bytes_total;
    // Real overlay updates land when the installer-core track lands; the
    // pass-through here keeps the launcher fail-closed and the tests
    // deterministic without an active compositor.
}

bool read_runtime_manifest(const std::wstring& path, std::string& manifest) {
    constexpr LONGLONG maximum_bytes = 4ll * 1024ll * 1024ll;
    HANDLE raw = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return false;
    std::unique_ptr<void, decltype(&CloseHandle)> file(raw, &CloseHandle);
    if (GetFileType(file.get()) != FILE_TYPE_DISK)
        return false;

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > maximum_bytes) {
        return false;
    }
    manifest.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0u;
    while (offset < manifest.size()) {
        DWORD read = 0u;
        const DWORD remaining = static_cast<DWORD>(manifest.size() - offset);
        if (!ReadFile(file.get(), manifest.data() + offset, remaining, &read, nullptr) ||
            read == 0u) {
            manifest.clear();
            return false;
        }
        offset += read;
    }
    return true;
}

std::mutex g_runtime_installer_manifest_mutex;

sao_status_t run_configured_runtime_installer(
    const sao::launcher::AppState& state,
    const sao::launcher::PluginsProviderConfiguration& configuration) noexcept {
    using sao::launcher::runtime_installer_glue::g_record_outcome;
    const auto record = [&](bool ok) noexcept {
        if (g_record_outcome != nullptr)
            g_record_outcome(state.platform_ctx, ok);
    };
    try {
        if (state.safe_mode || !configuration.enabled) {
            record(true);
            return SAO_STATUS_OK;
        }
        const bool manifest_configured = !configuration.runtime_manifest_path.empty();
        const RuntimeInstallerHookSnapshot hook = current_runtime_installer_hook();
        if (hook.ensure_all == nullptr ||
            hook.ensure_all == &runtime_installer_ensure_all_passthrough) {
            const sao_status_t status =
                manifest_configured ? SAO_STATUS_NOT_IMPLEMENTED : SAO_STATUS_OK;
            record(status == SAO_STATUS_OK);
            return status;
        }

        RuntimeInstallerProgressContext progress{};
        progress.platform = static_cast<sao_platform_ctx*>(state.platform_ctx);
        sao_status_t status = SAO_STATUS_OK;
#if defined(SAO_LAUNCHER_HAS_RUNTIME_INSTALLER)
        if (hook.production) {
            if (!manifest_configured) {
                record(true);
                return SAO_STATUS_OK;
            }
            std::lock_guard manifest_lock(g_runtime_installer_manifest_mutex);
            std::string manifest;
            if (!read_runtime_manifest(configuration.runtime_manifest_path, manifest)) {
                record(false);
                return SAO_STATUS_INVALID_ARGUMENT;
            }
            sao_runtime_manifest_handle_t handle = nullptr;
            status = sao_runtime_installer_load_manifest(manifest.data(), manifest.size(), &handle);
            if (status == SAO_STATUS_OK && handle == nullptr) {
                status = SAO_STATUS_INTERNAL;
            }
            if (status == SAO_STATUS_OK) {
                status = sao_runtime_installer_bind_manifest(handle);
            }
            if (status == SAO_STATUS_OK) {
                status = hook.ensure_all(state.base_dir.c_str(),
                                         &forward_runtime_installer_progress, &progress);
            }
            const sao_status_t unbind_status = sao_runtime_installer_bind_manifest(nullptr);
            if (handle != nullptr) {
                sao_runtime_installer_manifest_release(handle);
            }
            if (status == SAO_STATUS_OK && unbind_status != SAO_STATUS_OK) {
                status = unbind_status;
            }
        } else
#endif
        {
            status = hook.ensure_all(state.base_dir.c_str(), &forward_runtime_installer_progress,
                                     &progress);
        }
        record(status == SAO_STATUS_OK);
        return status;
    } catch (...) {
        record(false);
        return SAO_STATUS_INTERNAL;
    }
}

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
                sao_dual_run_config& dual_cfg, sao_dual_run_config_v2& dual_cfg_v2,
                HANDLE& single_instance_mutex, HANDLE& dual_run_driver_mutex, bool& crash_installed,
                bool& mutex_acquired, bool& dual_run_driver_acquired, bool& base_dir_resolved,
                bool& license_verified, bool& shell_verified, bool& security_initialized,
                bool& platform_up, bool& plugins_discovered, bool& ui_online,
                bool& handed_off_to_python, bool& dual_run_gate_failed,
                sao_status_t& pipeline_status, int& handed_off_exit_code) {
    using namespace sao::launcher;
    (void)crash_installed;

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
            dual_cfg.mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
            dual_cfg_v2.legacy.mode = SAO_DUAL_RUN_MODE_CPP_ONLY;
        } else if (ms == SAO_STATUS_OK) {
            dual_run_driver_acquired = true;
        } else {
            dual_run_gate_failed = true;
            pipeline_status = ms;
            return SAO_EXIT_PLATFORM_INIT_FAIL;
        }
    }

    int32_t should_continue = 1;
    int32_t handoff_exit_code = 0;
    sao_status_t zs =
        sao_launcher_dual_run_step_zero_v2(&dual_cfg_v2, &should_continue, &handoff_exit_code);
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

    // Step 1 — single-instance mutex.
    const auto acquire_result = acquireSingleInstance(single_instance_mutex);
    if (acquire_result == SingleInstanceAcquireResult::already_running) {
        forwardCommandLineToRunningInstance(GetCommandLineW());
        return SAO_EXIT_ALREADY_RUNNING;
    }
    if (acquire_result == SingleInstanceAcquireResult::failed) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    mutex_acquired = true;

    // Step 3 — resolve base_dir.  Failure is a fatal init error.
    if (!resolveWorkingDir(state)) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    base_dir_resolved = true;

    // Publish base_dir globally for downstream providers without truncation.
    if (!publishLauncherBaseDir(state.base_dir))
        return SAO_EXIT_PLATFORM_INIT_FAIL;

    // Prep the crash directory now that we know where to write dumps.
    const std::wstring crash_dir = state.base_dir + L"\\crash";
    if (!ensureDirectoryExists(crash_dir.c_str()))
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    setCrashDumpDirectory(crash_dir.c_str());
    std::wstring active_crash_dir;
    if (!getCrashDumpDirectory(active_crash_dir) ||
        _wcsicmp(active_crash_dir.c_str(), crash_dir.c_str()) != 0)
        return SAO_EXIT_PLATFORM_INIT_FAIL;

    // Step 4 — load settings.json (optional, non-fatal).  The launcher
    // itself doesn't consume the values yet; loading the file here
    // proves the plumbing works and gives the platform subsystem a
    // predictable place to pull from.  With no explicit --config all
    // optional providers remain disabled.  An explicit malformed config is
    // fatal rather than partially enabling a subsystem.
    if (loadLauncherProviderConfiguration(
            state.base_dir.c_str(),
            state.config_path.empty() ? nullptr : state.config_path.c_str()) != SAO_STATUS_OK) {
        return SAO_EXIT_PLATFORM_INIT_FAIL;
    }
    const auto provider_configuration = launcherProviderConfigurationSnapshot();

    // Step 5 — license verify.  The provider is optional until explicitly
    // enabled; once enabled, provider absence and invalid licenses are fatal.
    if (!sao::launcher::actualDebugNoLicenseRequested(state) &&
        provider_configuration.license.enabled) {
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

    // Step 7 — security init.
    {
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
        if (cfg.enable_anti_screencap &&
            sao_security_anti_screencap_register_process_windows() < 0) {
            (void)sao_security_shutdown();
            return SAO_EXIT_PLATFORM_INIT_FAIL;
        }
#endif
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

    // Step 8.5 — configured runtime installation is one canonical, fail-closed
    // step shared with the GUI launcher.  With no manifest configured it is
    // a no-op; test hooks still exercise this ordering boundary directly.
    const sao_status_t installer_status =
        ensureConfiguredPluginRuntimes(state, provider_configuration.plugins);
    if (installer_status != SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
        (void)sao_core_logf(SAO_LOG_ERROR, "launcher.runtime_installer",
                            "configured runtime installation failed: status=%d", installer_status);
#endif
        return SAO_EXIT_PLUGIN_LOAD_FAIL;
    }

    // Step 9 — plugin discovery (skipped in safe mode).
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

    // Step 10 — UI online.
    {
        sao_status_t s = sao_ui_bring_online(static_cast<sao_platform_ctx*>(state.platform_ctx));
        if (s != SAO_STATUS_OK) {
            return SAO_EXIT_UI_ONLINE_FAIL;
        }
        ui_online = true;
    }

    // Step 11 — pump the message loop until the hook (or WM_QUIT) tells
    // us to exit.  Tests supply a hook that returns non-zero on the
    // first call so the pipeline exits immediately and we can observe
    // teardown ordering.
    if (hooks && hooks->poll_should_exit) {
        while (true) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    return static_cast<int>(msg.wParam);
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
        return static_cast<int>(msg.wParam);
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
        releaseOwnedSingleInstanceMutex(single_instance_mutex);
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
    if (!restoreHeadlessProcessGlobals(cleanup))
        return SAO_STATUS_INTERNAL;
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

namespace sao::launcher {

sao_status_t ensureConfiguredPluginRuntimes(const AppState& state,
                                            const PluginsProviderConfiguration& plugins) noexcept {
    return run_configured_runtime_installer(state, plugins);
}

} // namespace sao::launcher

#if defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
namespace sao::launcher::testing {

bool rt_io_operator_preflight_observations_complete_for_test(
    uint32_t is_admin, uint32_t is_elevated, uint32_t load_driver_privilege_present,
    uint32_t load_driver_privilege_enabled, uint32_t hvci_enabled, uint32_t vbs_enabled,
    uint32_t provider_observable) noexcept {
    return ::rtIoOperatorPreflightObservationsComplete(
        is_admin, is_elevated, load_driver_privilege_present, load_driver_privilege_enabled,
        hvci_enabled, vbs_enabled, provider_observable);
}

bool rt_io_operator_preflight_failure_state_ready_for_test(int32_t last_failure_code,
                                                           uint32_t last_failure_stage) noexcept {
    return ::rtIoOperatorPreflightFailureStateReady(last_failure_code, last_failure_stage);
}

} // namespace sao::launcher::testing
#endif

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

    cleanup->crash_installed = installCrashHandler();

    LauncherLifecycleDecision lifecycle;
    const sao_status_t lifecycle_status = prepareLauncherLifecycle(lifecycle, state.rt_io_operator);
    if (lifecycle_status != SAO_STATUS_OK) {
        if (exit_code_out)
            *exit_code_out = SAO_EXIT_PLATFORM_INIT_FAIL;
        return lifecycle_status;
    }

    bool handed_off_to_python = false;
    bool dual_run_gate_failed = false;
    sao_status_t pipeline_status = SAO_STATUS_OK;
    int handed_off_exit_code = 0;

    int rc = runPipeline(
        hooks, state, lifecycle.dual_config, lifecycle.dual_config_v2,
        cleanup->single_instance_mutex, cleanup->dual_run_driver_mutex, cleanup->crash_installed,
        cleanup->mutex_acquired, cleanup->dual_run_driver_acquired, cleanup->base_dir_resolved,
        cleanup->license_verified, cleanup->shell_verified, cleanup->security_initialized,
        cleanup->platform_up, cleanup->plugins_discovered, cleanup->ui_online, handed_off_to_python,
        dual_run_gate_failed, pipeline_status, handed_off_exit_code);
    lifecycle.selected_mode = lifecycle.dual_config.mode;
    lifecycle.dual_config_v2.legacy.mode = lifecycle.selected_mode;
    cleanup->launcher_globals_published = cleanup->base_dir_resolved;

    // CPP_PREFERRED_PYTHON_FALLBACK — if any CPP step failed AFTER step-zero
    // let us know it wanted CPP, retry via Python.  step-zero itself didn't
    // spawn Python; we do that lazily here so the fallback is only paid for
    // when the CPP path actually fails.
    wchar_t inherited_role[64]{};
    const bool is_child_of_dual_run_driver =
        GetEnvironmentVariableW(SAO_DUAL_RUN_ENV_VAR_NAME, inherited_role, 64) > 0 &&
        inherited_role[0] != L'\0';
    if (!dual_run_gate_failed && !is_child_of_dual_run_driver && !handed_off_to_python &&
        rc != SAO_EXIT_OK && rc != SAO_EXIT_ALREADY_RUNNING && rc != SAO_EXIT_BAD_ARGS) {
        if (lifecycle.dual_config.mode == SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK) {
            int32_t fbec = 0;
            const wchar_t* step_name = L"cpp_pipeline";
            if (!cleanup->platform_up)
                step_name = L"platform_bringup";
            else if (!cleanup->plugins_discovered && !state.safe_mode)
                step_name = L"plugins_discover";
            else if (!cleanup->ui_online)
                step_name = L"ui_bring_online";
            if (sao_launcher_dual_run_maybe_fallback_to_python_v2(&lifecycle.dual_config_v2, rc,
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
    if (!teardown_complete && (rc == SAO_EXIT_OK || handed_off_to_python)) {
        rc = SAO_EXIT_PLATFORM_INIT_FAIL;
        handed_off_to_python = false;
    }
    if (teardown_complete) {
        if (!restoreHeadlessProcessGlobals(*cleanup)) {
            rc = SAO_EXIT_PLATFORM_INIT_FAIL;
            handed_off_to_python = false;
            teardown_complete = false;
        }
    }
    if (teardown_complete && cleanup->dual_run_driver_acquired && cleanup->dual_run_driver_mutex) {
        sao_launcher_dual_run_release_driver_mutex(cleanup->dual_run_driver_mutex);
        cleanup->dual_run_driver_mutex = nullptr;
        cleanup->dual_run_driver_acquired = false;
    }
    completeLauncherLifecycle(lifecycle, rc, failure_hint);
    if (exit_code_out)
        *exit_code_out = rc;
    if (!teardown_complete) {
        std::lock_guard lock(g_pending_cleanup_mutex);
        g_pending_cleanup = std::move(cleanup);
        return SAO_STATUS_INTERNAL;
    }
    if (pipeline_status != SAO_STATUS_OK)
        return pipeline_status;
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

extern "C" sao_status_t
sao_launcher_rt_io_operator_format_json(const sao_launcher_rt_io_operator_report_t* report,
                                        char* out_utf8, size_t out_capacity,
                                        size_t* out_bytes_written) {
    if (out_bytes_written != nullptr)
        *out_bytes_written = 0u;
    if (report == nullptr || out_utf8 == nullptr || out_capacity == 0u ||
        report->struct_size != sizeof(*report)) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    std::array<char, 2048u> generic_json{};
    const int generic_written = std::snprintf(
        generic_json.data(), generic_json.size(),
        "{\"stage\":\"%s\",\"status\":%d,\"operation_status\":%d,"
        "\"success\":%s,\"complete\":%s,"
        "\"preflight_only\":%s,\"input_checks\":%s,"
        "\"r5_check\":%s,\"mf_check\":%s,"
        "\"exit_after_validation\":%s,"
        "\"requested_step_mask\":%llu,\"attempted_step_mask\":%llu,"
        "\"passed_step_mask\":%llu,\"unknown_step_mask\":%llu,"
        "\"partial_step_mask\":%llu,\"selected_engine\":%u,"
        "\"runtime_tier\":%u,\"backend\":%u,"
        "\"selected_backend\":%u,\"driver_strategy\":%u,"
        "\"residue_gate\":%u,\"residue_count\":%u,"
        "\"unknown_count\":%u,\"is_admin\":%u,\"is_elevated\":%u,"
        "\"load_driver_privilege_present\":%u,"
        "\"load_driver_privilege_enabled\":%u,\"hvci_enabled\":%u,"
        "\"vbs_enabled\":%u,\"provider_observable\":%u,"
        "\"admission_mask\":%u,"
        "\"capability_mask\":%u,\"restore_mask\":%u,"
        "\"state_observed\":%s,\"resources_absent\":%s,"
        "\"loaded\":%s,\"probe_passed\":%s,"
        "\"backend_ready\":%s,\"call_authenticated\":%s,"
        "\"call_transport_complete\":%s,"
        "\"call_request_id_matched\":%s,\"call_committed\":%s,"
        "\"cleanup_acknowledged\":%s,\"cleanup_clean\":%s,"
        "\"cleanup_keep_running\":%s,\"provider_retained\":%s,"
        "\"wiper_joined\":%s,\"engine_cleanup_confirmed\":%s,"
        "\"etw_restore_confirmed\":%s,"
        "\"last_failure_code\":%d,\"last_failure_stage\":%u,"
        "\"r3_uc_patch_failure_reason\":%u,"
        "\"hid_fallback_reason\":%u,"
        "\"failure_classification\":\"%s\"",
        rtIoOperatorStageName(report->stage), report->status, report->operation_status,
        jsonBool(report->success), jsonBool(report->complete), jsonBool(report->preflight_only),
        jsonBool(report->input_checks), jsonBool(report->r5_check), jsonBool(report->mf_check),
        jsonBool(report->exit_after_validation),
        static_cast<unsigned long long>(report->requested_step_mask),
        static_cast<unsigned long long>(report->attempted_step_mask),
        static_cast<unsigned long long>(report->passed_step_mask),
        static_cast<unsigned long long>(report->unknown_step_mask),
        static_cast<unsigned long long>(report->partial_step_mask), report->selected_engine,
        report->runtime_tier, report->backend, report->selected_backend, report->driver_strategy,
        report->residue_gate, report->residue_count, report->unknown_count, report->is_admin,
        report->is_elevated, report->load_driver_privilege_present,
        report->load_driver_privilege_enabled, report->hvci_enabled, report->vbs_enabled,
        report->provider_observable, report->admission_mask, report->capability_mask,
        report->restore_mask, jsonBool(report->state_observed), jsonBool(report->resources_absent),
        jsonBool(report->loaded), jsonBool(report->probe_passed), jsonBool(report->backend_ready),
        jsonBool(report->call_authenticated), jsonBool(report->call_transport_complete),
        jsonBool(report->call_request_id_matched), jsonBool(report->call_committed),
        jsonBool(report->cleanup_acknowledged), jsonBool(report->cleanup_clean),
        jsonBool(report->cleanup_keep_running), jsonBool(report->provider_retained),
        jsonBool(report->wiper_joined), jsonBool(report->engine_cleanup_confirmed),
        jsonBool(report->etw_restore_confirmed), report->last_failure_code,
        report->last_failure_stage, report->r3_uc_patch_failure_reason, report->hid_fallback_reason,
        rtIoOperatorFailureName(report->failure_classification));
    if (generic_written < 0 || static_cast<size_t>(generic_written) >= generic_json.size()) {
        out_utf8[0] = '\0';
        return SAO_STATUS_INTERNAL;
    }
    try {
        std::string json(generic_json.data(), static_cast<size_t>(generic_written));
        json.reserve(kRtIoOperatorJsonCapacity);
        const auto append_unsigned = [&json](uint64_t value) {
            json.append(std::to_string(value));
        };
        const auto append_signed = [&json](int64_t value) { json.append(std::to_string(value)); };

        json.append(",\"strict_policy\":");
        append_unsigned(report->strict_policy);
        json.append(",\"strict_stage\":");
        append_unsigned(report->strict_stage);
        json.append(",\"strict_transaction_state\":");
        append_unsigned(report->strict_transaction_state);
        json.append(",\"strict_transaction_outcome\":");
        append_unsigned(report->strict_transaction_outcome);
        json.append(",\"strict_transaction_id\":");
        append_unsigned(report->strict_transaction_id);
        json.append(",\"strict_chain_generation\":");
        append_unsigned(report->strict_chain_generation);
        json.append(",\"strict_required_mask\":");
        append_unsigned(report->strict_required_mask);
        json.append(",\"strict_prepared_mask\":");
        append_unsigned(report->strict_prepared_mask);
        json.append(",\"strict_committed_mask\":");
        append_unsigned(report->strict_committed_mask);
        json.append(",\"strict_unknown_mask\":");
        append_unsigned(report->strict_unknown_mask);
        json.append(",\"strict_rollback_attempted_mask\":");
        append_unsigned(report->strict_rollback_attempted_mask);
        json.append(",\"strict_rollback_complete_mask\":");
        append_unsigned(report->strict_rollback_complete_mask);
        json.append(",\"strict_category_required_mask\":");
        append_unsigned(report->strict_category_required_mask);
        json.append(",\"strict_category_prepared_mask\":");
        append_unsigned(report->strict_category_prepared_mask);
        json.append(",\"strict_category_committed_mask\":");
        append_unsigned(report->strict_category_committed_mask);
        json.append(",\"strict_category_unknown_mask\":");
        append_unsigned(report->strict_category_unknown_mask);
        json.append(",\"strict_category_rollback_attempted_mask\":");
        append_unsigned(report->strict_category_rollback_attempted_mask);
        json.append(",\"strict_category_rollback_complete_mask\":");
        append_unsigned(report->strict_category_rollback_complete_mask);
        json.append(",\"strict_category_count\":");
        append_unsigned(report->strict_category_count);
        json.append(",\"strict_categories\":[");
        for (uint32_t index = 0u; index < SAO_LAUNCHER_RT_IO_STRICT_CATEGORY_COUNT; ++index) {
            if (index != 0u)
                json.push_back(',');
            const auto& category = report->strict_categories[index];
            json.append("{\"category\":");
            append_unsigned(category.category);
            json.append(",\"outcome\":");
            append_unsigned(category.outcome);
            json.append(",\"prepare_status\":");
            append_signed(category.prepare_status);
            json.append(",\"apply_status\":");
            append_signed(category.apply_status);
            json.append(",\"commit_status\":");
            append_signed(category.commit_status);
            json.append(",\"rollback_status\":");
            append_signed(category.rollback_status);
            json.push_back('}');
        }
        json.append("],\"strict_vt_vendor\":");
        append_unsigned(report->strict_vt_vendor);
        json.append(",\"strict_vt_root_active\":");
        json.append(jsonBool(report->strict_vt_root_active));
        json.append(",\"strict_vt_control_status\":");
        append_signed(report->strict_vt_control_status);
        json.append(",\"strict_vt_session_id\":");
        append_unsigned(report->strict_vt_session_id);
        json.append(",\"strict_vt_owner_generation\":");
        append_unsigned(report->strict_vt_owner_generation);
        json.append(",\"strict_vt_requested_engine\":");
        append_signed(report->strict_vt_requested_engine);
        json.append(",\"strict_vt_runtime_engine\":");
        append_signed(report->strict_vt_runtime_engine);
        json.append(",\"strict_vt_load_route\":");
        append_signed(report->strict_vt_load_path);
        json.append(",\"strict_vt_stage\":");
        append_signed(report->strict_vt_stage);
        json.append(",\"strict_vt_capture_status\":");
        append_signed(report->strict_vt_capture_status);
        json.append(",\"strict_vt_validation_status\":");
        append_signed(report->strict_vt_validation_status);
        json.append(",\"strict_vt_cleanup_status\":");
        append_signed(report->strict_vt_cleanup_status);
        json.append(",\"strict_vt_recovery_status\":");
        append_signed(report->strict_vt_recovery_status);
        json.append(",\"strict_vt_terminal_reason\":");
        append_signed(report->strict_vt_terminal_reason);
        json.append(",\"strict_final_residue_gate\":");
        append_unsigned(report->strict_final_residue_gate);
        json.append(",\"strict_response_flags\":");
        append_unsigned(report->strict_response_flags);
        json.append(",\"strict_helper_system\":");
        json.append(jsonBool(report->strict_helper_system));
        json.append(",\"strict_helper_identity_authenticated\":");
        json.append(jsonBool(report->strict_helper_identity_authenticated));
        json.append(",\"strict_helper_session_id\":");
        append_unsigned(report->strict_helper_session_id);
        json.append(",\"strict_helper_actual_image\":");
        appendJsonEscaped(json, report->strict_helper_actual_image,
                          sizeof(report->strict_helper_actual_image));
        json.append(",\"strict_helper_parent_image\":");
        appendJsonEscaped(json, report->strict_helper_parent_image,
                          sizeof(report->strict_helper_parent_image));
        json.append(",\"strict_success\":");
        json.append(jsonBool(report->strict_success));
        json.push_back('}');

        const size_t required = json.size();
        if (required >= out_capacity) {
            out_utf8[0] = '\0';
            if (out_bytes_written != nullptr)
                *out_bytes_written = required + 1u;
            return SAO_STATUS_INVALID_ARGUMENT;
        }
        std::memcpy(out_utf8, json.data(), required);
        out_utf8[required] = '\0';
        if (out_bytes_written != nullptr)
            *out_bytes_written = required;
        return SAO_STATUS_OK;
    } catch (...) {
        out_utf8[0] = '\0';
        return SAO_STATUS_INTERNAL;
    }
}

extern "C" sao_status_t sao_launcher_rt_io_operator_run(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_output_fn output, void* output_user_data, int32_t* out_ready) {
    constexpr sao_status_t kNotSubmitted = -4085;
    if (out_ready != nullptr)
        *out_ready = 0;
    if (ctx == nullptr || !rtIoOperatorOptionsValid(options) || output == nullptr ||
        out_ready == nullptr) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }

    auto emit = [&](const sao_launcher_rt_io_operator_report_t& report) {
        std::array<char, kRtIoOperatorJsonCapacity> json{};
        size_t written = 0u;
        const sao_status_t status =
            sao_launcher_rt_io_operator_format_json(&report, json.data(), json.size(), &written);
        if (status == SAO_STATUS_OK && written != 0u)
            output(json.data(), output_user_data);
        return status;
    };
    auto prepare_not_submitted = [&](uint32_t stage) {
        sao_launcher_rt_io_operator_report_t report{};
        initializeRtIoOperatorReport(options, stage, &report);
        report.status = kNotSubmitted;
        report.operation_status = kNotSubmitted;
        return report;
    };

    sao_status_t overall = SAO_STATUS_OK;
    sao_launcher_rt_io_operator_report_t preflight =
        prepare_not_submitted(SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT);
    sao_status_t stage_status = sao_platform_rt_io_operator_preflight(ctx, options, &preflight);
    if (preflight.status == SAO_STATUS_INTERNAL)
        preflight.status = stage_status;
    if (preflight.operation_status == SAO_STATUS_INTERNAL)
        preflight.operation_status = stage_status;
    if (stage_status != SAO_STATUS_OK || preflight.success == 0u) {
        overall = stage_status != SAO_STATUS_OK ? stage_status : SAO_STATUS_INTERNAL;
    }
    if (emit(preflight) != SAO_STATUS_OK)
        overall = SAO_STATUS_INTERNAL;

    if (options->preflight_only != 0u)
        return overall;

    sao_launcher_rt_io_operator_report_t init =
        prepare_not_submitted(SAO_LAUNCHER_RT_IO_STAGE_INIT);
    sao_launcher_rt_io_operator_report_t live =
        prepare_not_submitted(SAO_LAUNCHER_RT_IO_STAGE_LIVE);
    sao_launcher_rt_io_operator_report_t status =
        prepare_not_submitted(SAO_LAUNCHER_RT_IO_STAGE_STATUS);

    if (overall == SAO_STATUS_OK) {
        stage_status = sao_platform_rt_io_operator_init(ctx, options, &init);
        if (init.status == SAO_STATUS_INTERNAL)
            init.status = stage_status;
        if (init.operation_status == SAO_STATUS_INTERNAL)
            init.operation_status = stage_status;
        if (stage_status != SAO_STATUS_OK || init.success == 0u) {
            overall = stage_status != SAO_STATUS_OK ? stage_status : SAO_STATUS_INTERNAL;
        }
    }
    if (emit(init) != SAO_STATUS_OK)
        overall = SAO_STATUS_INTERNAL;

    if (overall == SAO_STATUS_OK) {
        stage_status = sao_platform_rt_io_operator_live_validate(ctx, options, &live);
        if (live.status == SAO_STATUS_INTERNAL)
            live.status = stage_status;
        if (live.operation_status == SAO_STATUS_INTERNAL)
            live.operation_status = stage_status;
        if (stage_status != SAO_STATUS_OK || live.success == 0u) {
            overall = stage_status != SAO_STATUS_OK ? stage_status : SAO_STATUS_INTERNAL;
        }
    }
    if (emit(live) != SAO_STATUS_OK)
        overall = SAO_STATUS_INTERNAL;

    if (overall == SAO_STATUS_OK) {
        stage_status = sao_platform_rt_io_operator_status(ctx, options, &status);
        if (status.status == SAO_STATUS_INTERNAL)
            status.status = stage_status;
        if (status.operation_status == SAO_STATUS_INTERNAL)
            status.operation_status = stage_status;
        if (stage_status != SAO_STATUS_OK || status.success == 0u) {
            overall = stage_status != SAO_STATUS_OK ? stage_status : SAO_STATUS_INTERNAL;
        }
    }
    if (emit(status) != SAO_STATUS_OK)
        overall = SAO_STATUS_INTERNAL;

    sao_launcher_rt_io_operator_report_t cleanup =
        prepare_not_submitted(SAO_LAUNCHER_RT_IO_STAGE_CLEANUP);
    const sao_status_t cleanup_status = sao_platform_rt_io_operator_cleanup(ctx, options, &cleanup);
    if (cleanup.status == SAO_STATUS_INTERNAL)
        cleanup.status = cleanup_status;
    if (cleanup.operation_status == SAO_STATUS_INTERNAL)
        cleanup.operation_status = cleanup_status;
    if (cleanup_status != SAO_STATUS_OK || cleanup.success == 0u) {
        overall = cleanup_status != SAO_STATUS_OK ? cleanup_status : SAO_STATUS_INTERNAL;
    }
    if (emit(cleanup) != SAO_STATUS_OK)
        overall = SAO_STATUS_INTERNAL;

    if (overall == SAO_STATUS_OK)
        *out_ready = 1;
    return overall;
}

// Test-only setter for the runtime installer hook. Available in every
// build so headless integration tests can install a mock without linking
// the installer-core track. Passing nullptr restores the pass-through
// default so subsequent tests observe the natural skip-when-unavailable
// behaviour.
extern "C" void sao_launcher_init_pipeline_test_set_runtime_installer_hook(
    sao::launcher::runtime_installer_glue::EnsureAllFn ensure_all) {
    std::lock_guard lock(g_runtime_installer_hook_mutex);
    g_runtime_installer_ensure_all_hook =
        ensure_all == nullptr ? &runtime_installer_ensure_all_passthrough : ensure_all;
    g_runtime_installer_hook_is_production = false;
}

#if defined(SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER) &&                                         \
    !defined(SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER)
namespace {

constexpr uint32_t kDefaultAntiDebugPollIntervalSeconds = 5u;

struct AntiDebugWorkerState {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stop_requested = false;

    ~AntiDebugWorkerState() noexcept {
        {
            std::lock_guard lock(mutex);
            stop_requested = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }
};

AntiDebugWorkerState& anti_debug_worker_state() {
    static AntiDebugWorkerState state;
    return state;
}

void log_anti_debug_result(const char* phase, sao_status_t status,
                           const SaoSecurityAntiDebugEvidence& evidence) noexcept {
    if (status == SAO_STATUS_OK && evidence.verdict == SAO_SECURITY_ANTI_DEBUG_VERDICT_ALLOW) {
        return;
    }
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    (void)sao_core_logf(
        evidence.verdict == SAO_SECURITY_ANTI_DEBUG_VERDICT_BLOCK ? SAO_LOG_ERROR : SAO_LOG_WARN,
        "launcher.security.anti_debug",
        "%s status=%d verdict=%u positive_mask=0x%llx unavailable_mask=0x%llx reason_bits=0x%llx",
        phase, static_cast<int>(status), evidence.verdict,
        static_cast<unsigned long long>(evidence.positive_mask),
        static_cast<unsigned long long>(evidence.unavailable_mask),
        static_cast<unsigned long long>(evidence.reason_bits));
#else
    std::fprintf(stderr, "anti_debug %s status=%d verdict=%u positive=0x%llx unavailable=0x%llx\n",
                 phase, static_cast<int>(status), evidence.verdict,
                 static_cast<unsigned long long>(evidence.positive_mask),
                 static_cast<unsigned long long>(evidence.unavailable_mask));
#endif
}

void anti_debug_worker_main(uint32_t interval_seconds, DWORD launcher_thread_id) noexcept {
    AntiDebugWorkerState& state = anti_debug_worker_state();
    try {
        for (;;) {
            std::unique_lock lock(state.mutex);
            if (state.cv.wait_for(lock, std::chrono::seconds(interval_seconds),
                                  [&state] { return state.stop_requested; })) {
                return;
            }
            lock.unlock();

            SaoSecurityAntiDebugEvidence evidence{};
            evidence.struct_size = SAO_SECURITY_ANTI_DEBUG_EVIDENCE_SIZE;
            evidence.abi_version = SAO_SECURITY_ANTI_DEBUG_EVIDENCE_ABI_VERSION;
            const sao_status_t status = sao_security_anti_debug_evaluate(
                SAO_SECURITY_ANTI_DEBUG_POLICY_FAIL_STRONG, &evidence);
            log_anti_debug_result("periodic", status, evidence);
            if (status == SAO_STATUS_OK &&
                evidence.verdict == SAO_SECURITY_ANTI_DEBUG_VERDICT_BLOCK) {
                if (!PostThreadMessageW(
                        launcher_thread_id, WM_QUIT,
                        static_cast<WPARAM>(sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL), 0)) {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
                    (void)sao_core_logf(SAO_LOG_ERROR, "launcher.security.anti_debug",
                                        "failed to post policy shutdown error=%lu",
                                        static_cast<unsigned long>(GetLastError()));
#endif
                    (void)TerminateProcess(
                        GetCurrentProcess(),
                        static_cast<UINT>(sao::launcher::SAO_EXIT_PLATFORM_INIT_FAIL));
                }
                return;
            }
        }
    } catch (...) {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
        (void)sao_core_log(SAO_LOG_ERROR, "launcher.security.anti_debug",
                           "periodic worker terminated after an unexpected exception");
#endif
    }
}

void stop_anti_debug_worker() noexcept {
    AntiDebugWorkerState& state = anti_debug_worker_state();
    std::thread worker;
    {
        std::lock_guard lock(state.mutex);
        state.stop_requested = true;
        state.cv.notify_all();
        worker = std::move(state.worker);
    }
    if (worker.joinable())
        worker.join();
    {
        std::lock_guard lock(state.mutex);
        state.stop_requested = false;
    }
}

sao_status_t start_anti_debug_worker(uint32_t interval_seconds) noexcept {
    stop_anti_debug_worker();
    if (interval_seconds == 0)
        interval_seconds = kDefaultAntiDebugPollIntervalSeconds;
    try {
        AntiDebugWorkerState& state = anti_debug_worker_state();
        std::lock_guard lock(state.mutex);
        state.stop_requested = false;
        state.worker = std::thread(anti_debug_worker_main, interval_seconds, GetCurrentThreadId());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_INTERNAL;
    }
}

#if defined(SAO_LAUNCHER_USER_EVASION_PROVIDER)
struct UserEvasionEvidence {
    uint32_t abi_version = 0;
    int32_t ntdll_verify_status = SAO_STATUS_NOT_IMPLEMENTED;
    uint32_t ntdll_hook_count = 0;
    uint32_t halo_resolved_count = 0;
    uint32_t halo_hooked_count = 0;
    uint32_t halo_unavailable_count = 0;
    int32_t syscall_gadget_status = SAO_STATUS_NOT_IMPLEMENTED;
    uint32_t syscall_gadget_available = 0;
};

uint32_t user_evasion_hash(const char* name) noexcept {
    uint32_t hash = 0;
    while (*name != '\0') {
        hash = (hash >> 13) | (hash << 19);
        hash += static_cast<uint8_t>(*name++);
    }
    return hash;
}

UserEvasionEvidence collect_user_evasion_evidence() {
    UserEvasionEvidence evidence{};
    evidence.abi_version = sao_security_user_evasion_abi_version();
    evidence.ntdll_verify_status =
        sao_security_user_evasion_unhook_verify(&evidence.ntdll_hook_count);

    constexpr std::array<const char*, 3> targets = {
        "NtClose",
        "NtQuerySystemTime",
        "NtQueryInformationProcess",
    };
    for (const char* target : targets) {
        const uint32_t hash = user_evasion_hash(target);
        uint32_t ssn = 0;
        const int32_t status = sao_security_user_evasion_halo_gate_resolve(hash, 32u, &ssn);
        if (status != SAO_STATUS_OK) {
            ++evidence.halo_unavailable_count;
            continue;
        }
        SaoHaloGateProbe probe{};
        if (sao_security_user_evasion_halo_gate_last_probe(&probe) != SAO_STATUS_OK ||
            probe.target_hash != hash || probe.resolved_ssn != ssn) {
            ++evidence.halo_unavailable_count;
            continue;
        }
        ++evidence.halo_resolved_count;
        if (probe.target_was_hooked != 0)
            ++evidence.halo_hooked_count;
    }

    void* gadget = nullptr;
    evidence.syscall_gadget_status =
        sao_security_user_evasion_find_syscall_gadget(nullptr, &gadget);
    evidence.syscall_gadget_available =
        evidence.syscall_gadget_status == SAO_STATUS_OK && gadget != nullptr ? 1u : 0u;
    return evidence;
}

bool evaluate_user_evasion(bool strict) {
    const UserEvasionEvidence evidence = collect_user_evasion_evidence();
    const bool incomplete =
        evidence.abi_version == 0 ||
        (evidence.ntdll_verify_status != SAO_STATUS_OK && evidence.halo_resolved_count == 0);
    const bool finding =
        evidence.ntdll_hook_count != 0 || evidence.halo_hooked_count != 0 || incomplete;
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
    (void)sao_core_logf(finding ? SAO_LOG_WARN : SAO_LOG_INFO, "launcher.security.user_evasion",
                        "abi=0x%08x verify_status=%d hook_count=%u halo_resolved=%u halo_hooked=%u "
                        "halo_unavailable=%u gadget_status=%d gadget_available=%u verdict=%s",
                        evidence.abi_version, evidence.ntdll_verify_status,
                        evidence.ntdll_hook_count, evidence.halo_resolved_count,
                        evidence.halo_hooked_count, evidence.halo_unavailable_count,
                        evidence.syscall_gadget_status, evidence.syscall_gadget_available,
                        finding ? (strict ? "block" : "warn") : "allow");
#else
    std::fprintf(stderr, "user_evasion abi=0x%08x hooks=%u halo_hooked=%u verdict=%s\n",
                 evidence.abi_version, evidence.ntdll_hook_count, evidence.halo_hooked_count,
                 finding ? (strict ? "block" : "warn") : "allow");
#endif
    return !finding || !strict;
}
#endif

} // namespace
#endif

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

sao_status_t sao_launcher_init_pipeline_test_bind_plugins_preserves_registry(
    sao_plugins_registry* previous, sao_plugins_registry* candidate,
    sao_status_t authority_sync_status, sao_plugins_registry** current_out) {
    auto sync = [](void* user_data) -> sao_status_t {
        return *static_cast<const sao_status_t*>(user_data);
    };
    sao_plugins_registry* current = previous;
    const sao_status_t status =
        bindPluginsWithAuthority(current, candidate, sync, &authority_sync_status);
    if (current_out != nullptr)
        *current_out = current;
    return status;
}

sao_status_t sao_platform_bringup(const sao_platform_config* cfg, sao_platform_ctx** ctx_out) {
    if (!g_composition_test_hooks.platform_bringup) {
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    EnvironmentVariableRollback base_dir_environment(L"SAO_BASE_DIR");
    if (cfg != nullptr && cfg->base_dir != nullptr &&
        (!base_dir_environment.valid() || !base_dir_environment.set(cfg->base_dir))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
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
sao_status_t sao_ui_linkstart_poll_finished(sao_platform_ctx*, int32_t* out_just_finished) {
    int32_t reason = 0;
    return sao_ui_linkstart_poll_finished_ex(nullptr, out_just_finished, &reason);
}
sao_status_t sao_ui_linkstart_poll_finished_ex(sao_platform_ctx*, int32_t* out_just_finished,
                                               int32_t* out_completion_reason) {
    if (out_just_finished == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_completion_reason == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_just_finished = 0;
    *out_completion_reason = 0;
    return SAO_STATUS_OK;
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
sao_status_t sao_platform_bind_user_menu(sao_platform_ctx*, void*) {
    return SAO_STATUS_OK;
}
sao_status_t sao_platform_unbind_user_menu(sao_platform_ctx*, void*) {
    return SAO_STATUS_OK;
}
sao_status_t sao_platform_user_guide_presented(sao_platform_ctx*, int32_t* out_presented) {
    if (out_presented == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_presented = 1;
    return SAO_STATUS_OK;
}
sao_status_t sao_platform_mark_user_guide_presented(sao_platform_ctx*) {
    return SAO_STATUS_OK;
}

sao_status_t
sao_platform_rt_io_operator_preflight(sao_platform_ctx* ctx,
                                      const sao_launcher_rt_io_operator_options_t* options,
                                      sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT, out_report);
    if (!g_composition_test_hooks.rt_io_operator_preflight) {
        if (out_report != nullptr) {
            out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
        }
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.rt_io_operator_preflight(ctx, options, out_report,
                                                             g_composition_test_hooks.user_data);
}

sao_status_t sao_platform_rt_io_operator_init(sao_platform_ctx* ctx,
                                              const sao_launcher_rt_io_operator_options_t* options,
                                              sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_INIT, out_report);
    if (!g_composition_test_hooks.rt_io_operator_init) {
        if (out_report != nullptr) {
            out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
        }
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.rt_io_operator_init(ctx, options, out_report,
                                                        g_composition_test_hooks.user_data);
}
sao_status_t
sao_platform_rt_io_operator_live_validate(sao_platform_ctx* ctx,
                                          const sao_launcher_rt_io_operator_options_t* options,
                                          sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_LIVE, out_report);
    if (!g_composition_test_hooks.rt_io_operator_live_validate) {
        if (out_report != nullptr) {
            out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
        }
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.rt_io_operator_live_validate(
        ctx, options, out_report, g_composition_test_hooks.user_data);
}
sao_status_t
sao_platform_rt_io_operator_status(sao_platform_ctx* ctx,
                                   const sao_launcher_rt_io_operator_options_t* options,
                                   sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_STATUS, out_report);
    if (!g_composition_test_hooks.rt_io_operator_status) {
        if (out_report != nullptr) {
            out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
        }
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.rt_io_operator_status(ctx, options, out_report,
                                                          g_composition_test_hooks.user_data);
}
sao_status_t
sao_platform_rt_io_operator_cleanup(sao_platform_ctx* ctx,
                                    const sao_launcher_rt_io_operator_options_t* options,
                                    sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_CLEANUP, out_report);
    if (!g_composition_test_hooks.rt_io_operator_cleanup) {
        if (out_report != nullptr) {
            out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
            out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
        }
        return SAO_STATUS_NOT_IMPLEMENTED;
    }
    return g_composition_test_hooks.rt_io_operator_cleanup(ctx, options, out_report,
                                                           g_composition_test_hooks.user_data);
}

// Test-hook composition has no staged surface/driver/engine split: the whole
// bring-up happens in the surface step and the later stages are no-ops.
sao_status_t sao_platform_bringup_surface(const sao_platform_config* cfg,
                                          sao_platform_ctx** ctx_out) {
    return sao_platform_bringup(cfg, ctx_out);
}

sao_status_t sao_platform_bringup_drivers(const sao_platform_config*, sao_platform_ctx*) {
    return SAO_STATUS_OK;
}

sao_status_t sao_platform_bringup_engines(const sao_platform_config*, sao_platform_ctx*,
                                          sao_platform_ctx**) {
    return SAO_STATUS_OK;
}

sao_status_t sao_platform_bringup_capture_shield(const sao_platform_config*, sao_platform_ctx*) {
    return SAO_STATUS_OK;
}

sao_status_t sao_platform_bringup_capture_sweep(sao_platform_ctx*) {
    return SAO_STATUS_OK;
}

sao_status_t sao_platform_bringup_wnd_scrub(const sao_platform_config*, sao_platform_ctx*) {
    return SAO_STATUS_OK;
}

sao_status_t sao_ui_intro_show(sao_platform_ctx*, int32_t) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}

sao_status_t sao_ui_intro_publish_bootstrap(sao_platform_ctx*, const SaoUiLinkStartBootstrap*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}

sao_status_t sao_ui_intro_release_bootstrap(sao_platform_ctx*, int32_t) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}

sao_status_t sao_ui_intro_pump(sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
#elif defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
struct sao_platform_ctx {
    sao_rt_io_proxy_handle_t rt_io_proxy;
    uint64_t rt_io_strict_transaction_id = 0u;
    uint64_t rt_io_strict_chain_generation = 0u;
    sao_rt_io_window_rect_controller_t window_rect_controller;
    SaoRtIoWindowToken window_rect_token;
    bool window_rect_registered;
    // Capture-shield / tagWND chains.  The auxiliary windows (hControl decoy,
    // suppressed owner) and the coordinator registration for hControl are owned
    // here because the overlay host only registers hRender.
    SaoRtIoWindowToken window_rect_control_token;
    SaoRtIoWindowToken window_rect_owner_token;
    bool window_rect_aux_registered = false;
    void* dc_mutation_control_token = nullptr;
    uint32_t capture_shield_methods = 0u;
    uint32_t capture_shield_threat_flags = 0u;
    bool capture_shield_active = false;
    bool wnd_scrub_applied = false;
    sao_ui_dc_mutation_coordinator_handle_t dc_mutation_coordinator;
    sao_ui_overlay_host_handle_t overlay_host;
    sao_ui_compositor_handle_t compositor;
    sao_ui_input_router_deep_handle_t keyboard_router = nullptr;
    bool sdk_compositor_bound;
    void* user_menu = nullptr;
    sao_ui_entity_shell_handle_t entity_shell;
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    sao_ui_fisheye_backdrop_handle_t fisheye_backdrop;
    std::unique_ptr<sao::launcher::plugin_manager_panel::Owner> plugin_manager_panel;
    std::unique_ptr<sao::launcher::process_selector_panel::Owner> process_selector_panel;
    std::unique_ptr<sao::launcher::workshop_panel::Owner> workshop_panel;
    bool workshop_panel_visible;
    std::unique_ptr<sao::launcher::license_panel::Owner> license_panel;
    bool license_panel_visible;
#endif
    std::unique_ptr<sao::launcher::hotkey::Owner> hotkey_owner;
    SaoUiThemeId previous_theme = SAO_UI_THEME_DARK;
    bool restore_theme_on_rollback = false;
    bool settings_save_enabled = false;
    bool rt_io_operator_shutdown = false;
    sao_launcher_rt_io_operator_report_t rt_io_cleanup_report{};
    bool nervgear_mode{true};
    bool screencap_protection{true};
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
    // 自动播的 Link Start 开场；tick 驱动，结束时打 just_finished 边沿。
    sao_ui_linkstart_handle_t linkstart = nullptr;
    SaoUiLinkStartCompletionReason linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    bool linkstart_pending_completion = false;
    ULONGLONG linkstart_last_tick = 0;
};

void SAO_UI_CALL resize_linkstart_for_host(int32_t width, int32_t height,
                                           void* user_data) noexcept {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || ctx->linkstart == nullptr || width <= 0 || height <= 0)
        return;
    const uint32_t dpi = sao_ui_overlay_host_current_dpi(ctx->overlay_host);
    (void)sao_ui_linkstart_resize(ctx->linkstart, static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height), dpi);
}

void SAO_UI_CALL resize_linkstart_for_dpi(uint32_t dpi, int32_t, int32_t, int32_t width,
                                          int32_t height, void* user_data) noexcept {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || ctx->linkstart == nullptr || width <= 0 || height <= 0)
        return;
    (void)sao_ui_linkstart_resize(ctx->linkstart, static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height), dpi);
}

void capture_linkstart_completion(sao_platform_ctx* ctx,
                                  SaoUiLinkStartCompletionReason fallback) noexcept {
    if (ctx == nullptr)
        return;
    SaoUiLinkStartCompletionReason reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    if (ctx->linkstart != nullptr)
        (void)sao_ui_linkstart_poll_completion(ctx->linkstart, &reason);
    ctx->linkstart_completion_reason =
        reason == SAO_UI_LINKSTART_COMPLETION_NONE ? fallback : reason;
    ctx->linkstart_pending_completion = false;
}

void clear_settings_bindings() noexcept {
    (void)sao_launcher_hotkey_set_settings_owner(nullptr);
    (void)sao::launcher::settings::settings_profiles_unbind_owner(nullptr);
    (void)sao::launcher::settings::settings_panel_unbind_owner(nullptr);
}

void drain_deferred_cleanup_for_owner() noexcept {
    sao::launcher::hotkey::Owner::drain_deferred_cleanup_for_owner();
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
    sao::launcher::license_panel::Owner::drain_deferred_cleanup_for_owner();
#endif
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    sao::launcher::workshop_panel::Owner::drain_deferred_cleanup_for_owner();
    sao::launcher::process_selector_panel::Owner::drain_deferred_cleanup_for_owner();
    sao::launcher::plugin_manager_panel::Owner::drain_deferred_cleanup_for_owner();
#endif
}

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
    bool native_panel = false;
};

sao_status_t reload_plugins(void* user_data);
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
void sync_entity_publication_authority(sao_platform_ctx* ctx) noexcept;
#endif

void SAO_UI_CALL collect_shared_panel_visibility(sao_ui_panel_handle_t,
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
    const bool visible = descriptor->visible;
    visibility->native_panel = visibility->native_panel || visible;
    visibility->plugin_manager = visibility->plugin_manager || (plugin_manager && visible);
    visibility->process_selector = visibility->process_selector || (process_selector && visible);
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
        ctx->workshop_panel_visible, panels.plugin_manager,
        panels.process_selector,     entity.overlay_visible && entity.menu_visible,
        panels.native_panel,
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

sao_status_t create_shared_ui_owners(const wchar_t* base_dir, sao_platform_ctx* ctx,
                                     bool safe_mode) noexcept {
    if (base_dir == nullptr || ctx == nullptr || ctx->compositor == nullptr ||
        (!safe_mode && ctx->rt_io_proxy == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sao_status_t status = sao_ui_fisheye_backdrop_create(ctx->compositor, &ctx->fisheye_backdrop);
    if (status != SAO_STATUS_OK)
        return status;
    try {
        if (!safe_mode) {
            ctx->plugin_manager_panel =
                std::make_unique<sao::launcher::plugin_manager_panel::Owner>(
                    ctx->compositor, [ctx] { return reload_plugins(ctx); });
            ctx->process_selector_panel =
                std::make_unique<sao::launcher::process_selector_panel::Owner>(ctx->compositor,
                                                                               ctx->rt_io_proxy);
        }
        ctx->workshop_panel = std::make_unique<sao::launcher::workshop_panel::Owner>(
            ctx->compositor, std::filesystem::path(base_dir));
        ctx->workshop_panel->set_visibility_changed_callback([ctx](bool visible) {
            ctx->workshop_panel_visible = visible;
            (void)update_shared_fisheye_visibility(ctx);
        });
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        ctx->license_panel = std::make_unique<sao::launcher::license_panel::Owner>(ctx->compositor);
#endif
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
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
    if (ctx->license_panel) {
        const sao_status_t status = ctx->license_panel->take_offline();
        if (status != SAO_STATUS_OK)
            return status;
        ctx->license_panel.reset();
        ctx->license_panel_visible = false;
        ctx->builtin_action_state.authority.license_activation = false;
    }
#endif
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
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    sync_entity_publication_authority(ctx);
#endif
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
sao_status_t retire_hotkeys(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    const sao_status_t menu_status = sao_platform_unbind_user_menu(ctx, nullptr);
    if (menu_status != SAO_STATUS_OK)
        return menu_status;
    if (!sao::launcher::hotkey::unregister_all().empty())
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    sao::launcher::hotkey::clear_callbacks();
    if (ctx->hotkey_owner) {
        const sao_status_t status = ctx->hotkey_owner->take_offline();
        if (status != SAO_STATUS_OK)
            return status;
        ctx->hotkey_owner.reset();
    }
    sao_launcher_hotkey_set_settings_owner(nullptr);
    return SAO_STATUS_OK;
}

sao_status_t retire_settings_panel() noexcept {
    const sao_status_t panel_status = sao::launcher::settings::take_offline_for_testing();
    if (panel_status != SAO_STATUS_OK)
        return panel_status;
    clear_settings_bindings();
    return SAO_STATUS_OK;
}

void write_platform_bringup_diagnostic(const char* line, int length) noexcept {
    if (line == nullptr || length <= 0)
        return;
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0u;
    (void)WriteFile(output, line, static_cast<DWORD>(length), &written, nullptr);
}

sao_status_t trace_platform_bringup_failure(const char* stage,
                                            sao_status_t failure_status) noexcept {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"SAO_LAUNCHER_STARTUP_DIAGNOSTICS", enabled,
                                static_cast<DWORD>(std::size(enabled))) == 0u) {
        return failure_status;
    }

    char line[512]{};
    int length = sprintf_s(line, sizeof(line), "SAO_STARTUP stage=%s status=%d\r\n",
                           stage != nullptr ? stage : "unknown", failure_status);
    write_platform_bringup_diagnostic(line, length);

    if (stage == nullptr || (std::strcmp(stage, "rt_io_proxy_open_v2") != 0 &&
                             std::strcmp(stage, "rt_io_proxy_open_v3") != 0))
        return failure_status;

    SaoRtIoHelperLaunchDiagnostics diagnostics{};
    diagnostics.struct_size = sizeof(diagnostics);
    diagnostics.abi_version = SAO_RT_IO_HELPER_LAUNCH_DIAGNOSTICS_ABI_VERSION;
    const sao_status_t diagnostics_status =
        sao_rt_io_helper_get_last_launch_diagnostics(&diagnostics);
    length = sprintf_s(
        line, sizeof(line),
        "SAO_STARTUP helper_diagnostics_status=%d launch_status=%d flags=0x%08X "
        "requested_ppid=%u effective_ppid=%u ppid_reason=%u ppid_os_error=%u "
        "requested_strict=%u effective_strict=%u strict_reason=%u identity_mismatch=0x%08X\r\n",
        diagnostics_status, diagnostics.status, diagnostics.flags,
        diagnostics.requested_ppid_policy, diagnostics.effective_ppid_policy,
        diagnostics.ppid_reason, diagnostics.ppid_os_error,
        diagnostics.requested_strict_bootstrap_policy,
        diagnostics.effective_strict_bootstrap_policy, diagnostics.strict_reason,
        diagnostics.identity_mismatch_fields);
    write_platform_bringup_diagnostic(line, length);
    return failure_status;
}

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

sao_status_t SAO_UI_CALL unlink_z_order(void* user_data, void* hwnd, uint32_t timeout_ms) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || hwnd == nullptr) {
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
    return sao_rt_io_unlink_z_order(ctx->window_rect_controller, &token, timeout_ms, &result);
}

sao_status_t SAO_UI_CALL apply_overlay_protection_provider(void* render_hwnd, void* control_hwnd,
                                                           void* owner_hwnd, bool enable,
                                                           void* user_data) {
    (void)render_hwnd;
    (void)control_hwnd;
    (void)owner_hwnd;
    (void)enable;
    (void)user_data;
    // WdiSvcHost is deliberately not an affinity authority.  The local
    // anti-screencap facade owns these SaoAuto HWNDs; a foreign helper
    // request would create a second WDA owner and could make the UI roll
    // back a successful local transition when the helper rejects it.
    return SAO_STATUS_OK;
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
    target.license_activation = source.license_activation;
    target.plugin_manager = source.plugin_manager;
    target.reload_plugins = source.reload_plugins;
    target.plugin_status = source.plugin_status;
    target.fisheye_procedural = source.fisheye_procedural;
    target.fisheye_live = source.fisheye_live;
    target.theme = source.theme;
    target.about = source.about;
    // runtime_installer mirrors the action-side authority so the Panel
    // catalog can hide plugin-runtime entries when the installer failed
    // without the launcher having to walk the plugin registry a second
    // time. See entity_builtin_action_internal.h for the source semantics.
    target.runtime_installer = source.runtime_installer;
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
            auto* ctx = static_cast<sao_platform_ctx*>(context);
            return sao_ui_overlay_host_set_capture_mode(ctx->overlay_host,
                                                        exclude && ctx->screencap_protection);
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

sao_status_t open_license_panel(void* user_data) {
    auto* ctx = static_cast<sao_platform_ctx*>(user_data);
    if (ctx == nullptr || !ctx->license_panel)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    const sao_status_t status = ctx->license_panel->open();
    if (status != SAO_STATUS_OK)
        return status;
    (void)update_shared_fisheye_visibility(ctx);
    return SAO_STATUS_OK;
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
    case SAO_UI_ENTITY_ACTION_OPEN_LICENSE_ACTIVATION:
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
        operations.open_license_panel = &open_license_panel;
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
    if (sao::launcher::openUserDocsIndex(sao_launcher_base_dir(), owner)) {
        return SAO_STATUS_OK;
    }
    if (ctx != nullptr && ctx->compositor != nullptr) {
        sao_ui_dialog_show_error(ctx->compositor, nullptr, "SAO Auto",
                                 "用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。", nullptr,
                                 nullptr);
    } else {
        MessageBoxW(owner, L"用户指南暂时不可用。请重新安装或修复 SAO Auto 后重试。", L"SAO Auto",
                    MB_OK | MB_ICONERROR | MB_TASKMODAL);
    }
    return SAO_STATUS_ERR_NOT_FOUND;
}

uint32_t rt_io_operator_restore_mask(const SaoRtIoProductionStateWireV1& state) noexcept {
    uint32_t mask = 0u;
    if (state.retain_for_recovery != 0u || state.restore_pending != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_PROVIDER_RETAINED;
    if (state.handle_recovery_pending != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_HANDLE;
    if (state.mf_restore_pending != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_MF;
    if (state.ob_restore_pending != 0u || state.ob_restore_state != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_OB;
    if (state.ob_recovery_pending != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_OB_RECOVERY;
    if (state.hid.shared_restore_pending != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_HID_SHARED;
    if (state.native_io_pending != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_NATIVE_IO;
    if (state.active_r3_maps != 0u || state.r3_operations_inflight != 0u ||
        state.r3_close_in_progress != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_R3_ACTIVITY;
    if (state.cached_write_state == 3u || state.cached_write_state == 4u ||
        state.cached_write_in_flight != 0u || state.cached_write_cleanup_owner != 0u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_CACHED_WRITE;
    if (state.r1_state == 3u || state.r3_state == 3u)
        mask |= SAO_LAUNCHER_RT_IO_RESTORE_RESOURCE_UNKNOWN;
    return mask;
}

uint32_t rt_io_operator_admission_mask(const SaoRtIoProductionStateWireV1& state) noexcept {
    uint32_t mask = 0u;
    if (state.r3_map_admission_open != 0u)
        mask |= SAO_LAUNCHER_RT_IO_ADMISSION_R3_MAP;
    if (state.cached_write_state == 2u && state.cached_write_binding_valid != 0u &&
        state.cached_write_binding_current != 0u)
        mask |= SAO_LAUNCHER_RT_IO_ADMISSION_CACHED_WRITE;
    if (state.hid_shared_owner_current != 0u)
        mask |= SAO_LAUNCHER_RT_IO_ADMISSION_HID_OWNER;
    if (state.hid_shared_probe_ready != 0u)
        mask |= SAO_LAUNCHER_RT_IO_ADMISSION_HID_PROBE;
    return mask;
}

uint32_t rt_io_operator_capability_mask(const SaoRtIoProductionStateWireV1& state) noexcept {
    uint32_t mask = 0u;
    if (state.hid.r3_shared_ready != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_R3_SHARED;
    if (state.hid.r5_direct_ready != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_R5_DIRECT;
    if (state.hid.mf_ready != 0u || state.mf_ready != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_MF;
    if (state.hid_mouse_provenance != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_MOUSE_PROVENANCE;
    if (state.hid_keyboard_provenance != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_KEYBOARD_PROVENANCE;
    if (state.ob_ready != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_OB;
    if (state.watchdog_hooks_ready != 0u)
        mask |= SAO_LAUNCHER_RT_IO_CAP_WATCHDOG;
    return mask;
}

bool rt_io_operator_production_header_valid(const SaoRtIoProductionStateWireV1& state) noexcept {
    return std::memcmp(state.header.magic, SAO_RT_IO_PRODUCTION_STATE_WIRE_MAGIC, 4u) == 0 &&
           state.header.version == SAO_RT_IO_PRODUCTION_STATE_WIRE_VERSION &&
           state.header.header_size == sizeof(SaoRtIoVersionedPayloadHeader) &&
           state.header.struct_size == sizeof(state) && state.header.reserved == 0u;
}

bool rt_io_operator_production_enums_known(const SaoRtIoProductionStateWireV1& state) noexcept {
    return state.r1_state <= 3u && state.reserved_resource_state_2 == 0u && state.r3_state <= 3u &&
           state.cached_write_state <= 4u && state.hid.selected_backend <= 4u;
}

void rt_io_operator_copy_call(const SaoRtIoCallResult& call,
                              sao_launcher_rt_io_operator_report_t* report) noexcept {
    report->call_authenticated = call.authenticated != 0u ? 1u : 0u;
    report->call_transport_complete = call.transport_complete != 0u ? 1u : 0u;
    report->call_request_id_matched = call.request_id_matched != 0u ? 1u : 0u;
    report->call_committed = call.outcome == SAO_RT_IO_OUTCOME_COMMITTED ? 1u : 0u;
}

void rt_io_operator_copy_state(const SaoRtIoProductionStateWireV1& state,
                               sao_launcher_rt_io_operator_report_t* report) noexcept {
    report->state_observed = rt_io_operator_production_header_valid(state) ? 1u : 0u;
    report->selected_backend = state.hid.selected_backend;
    report->admission_mask = rt_io_operator_admission_mask(state);
    report->capability_mask = rt_io_operator_capability_mask(state);
    report->restore_mask = rt_io_operator_restore_mask(state);
    report->provider_retained = state.retain_for_recovery != 0u ? 1u : 0u;
    report->wiper_joined = state.wiper_joined != 0u ? 1u : 0u;
    report->engine_cleanup_confirmed = state.engine_cleanup_confirmed != 0u ? 1u : 0u;
    report->etw_restore_confirmed = state.etw_restore_confirmed != 0u ? 1u : 0u;
    report->resources_absent =
        state.r1_state == 0u && state.reserved_resource_state_2 == 0u && state.r3_state == 0u ? 1u
                                                                                              : 0u;
    report->last_failure_code = state.last_failure_code;
    report->last_failure_stage = state.last_failure_stage;
    report->r3_uc_patch_failure_reason = state.r3_uc_patch_failure_reason;
    report->hid_fallback_reason = state.hid.fallback_reason;
}

static_assert(SAO_LAUNCHER_RT_IO_STRICT_CATEGORY_COUNT == SAO_RT_IO_STRICT_CHAIN_CATEGORY_COUNT);

uint32_t rt_io_operator_strict_residue_gate(uint32_t strict_gate) noexcept {
    switch (strict_gate) {
    case SAO_RT_IO_STRICT_CHAIN_RESIDUE_CLEAN:
        return SAO_RT_IO_OPERATOR_RESIDUE_GATE_CLEAN;
    case SAO_RT_IO_STRICT_CHAIN_RESIDUE_UNKNOWN:
        return SAO_RT_IO_OPERATOR_RESIDUE_GATE_UNKNOWN;
    default:
        return SAO_RT_IO_OPERATOR_RESIDUE_GATE_DIRTY;
    }
}

uint32_t rt_io_operator_bit_count(uint32_t mask) noexcept {
    uint32_t count = 0u;
    while (mask != 0u) {
        count += mask & 1u;
        mask >>= 1u;
    }
    return count;
}

void rt_io_operator_copy_strict_response(const SaoRtIoProxyStrictChainRespV1& response,
                                         sao_launcher_rt_io_operator_report_t* report) noexcept {
    if (report == nullptr)
        return;

    const auto& state = response.wire.snapshot.state;
    const auto& projection = response.wire.snapshot.vt_projection;
    report->strict_policy = state.policy;
    report->strict_stage = state.stage;
    report->strict_transaction_state = state.transaction_state;
    report->strict_transaction_outcome = state.transaction_outcome;
    report->strict_transaction_id = state.transaction_id;
    report->strict_chain_generation = state.chain_generation;
    report->strict_required_mask = state.required_mask;
    report->strict_prepared_mask = state.prepared_mask;
    report->strict_committed_mask = state.committed_mask;
    report->strict_unknown_mask = state.unknown_mask;
    report->strict_rollback_attempted_mask = state.rollback_attempted_mask;
    report->strict_rollback_complete_mask = state.rollback_complete_mask;
    report->strict_category_required_mask = state.category_required_mask;
    report->strict_category_prepared_mask = state.category_prepared_mask;
    report->strict_category_committed_mask = state.category_committed_mask;
    report->strict_category_unknown_mask = state.category_unknown_mask;
    report->strict_category_rollback_attempted_mask = state.category_rollback_attempted_mask;
    report->strict_category_rollback_complete_mask = state.category_rollback_complete_mask;
    report->strict_category_count = state.category_count;
    for (uint32_t index = 0u; index < SAO_LAUNCHER_RT_IO_STRICT_CATEGORY_COUNT; ++index) {
        const auto& source = state.categories[index];
        auto& destination = report->strict_categories[index];
        destination.category = source.category;
        destination.outcome = source.outcome;
        destination.prepare_status = source.prepare_status;
        destination.apply_status = source.apply_status;
        destination.commit_status = source.commit_status;
        destination.rollback_status = source.rollback_status;
    }

    report->strict_vt_vendor = state.vendor;
    report->strict_vt_root_active =
        sao_rt_io_vt_stage_projects_loaded(projection.stage) != 0 ? 1u : 0u;
    report->strict_vt_control_status = projection.operation_status;
    report->strict_vt_session_id = projection.session_id;
    report->strict_vt_owner_generation = projection.owner_generation;
    report->strict_vt_requested_engine = projection.requested_engine;
    report->strict_vt_runtime_engine = projection.runtime_engine;
    report->strict_vt_load_path = projection.load_path;
    report->strict_vt_stage = projection.stage;
    report->strict_vt_capture_status = projection.capture_status;
    report->strict_vt_validation_status = projection.validation_status;
    report->strict_vt_cleanup_status = projection.cleanup_status;
    report->strict_vt_recovery_status = projection.recovery_status;
    report->strict_vt_terminal_reason = projection.terminal_reason;
    report->strict_final_residue_gate = state.residue_gate;
    report->strict_response_flags = response.wire.response_flags;
    report->strict_helper_system = response.identity.helper_system != 0u ? 1u : 0u;
    report->strict_helper_identity_authenticated =
        response.identity.identity_authenticated != 0u ? 1u : 0u;
    report->strict_helper_session_id =
        response.identity.helper_session_known != 0u ? response.identity.helper_session_id : 0u;
    std::memcpy(report->strict_helper_actual_image, response.identity.helper_actual_image_utf8,
                sizeof(report->strict_helper_actual_image));
    report->strict_helper_actual_image[sizeof(report->strict_helper_actual_image) - 1u] = '\0';
    std::memcpy(report->strict_helper_parent_image, response.identity.helper_parent_image_utf8,
                sizeof(report->strict_helper_parent_image));
    report->strict_helper_parent_image[sizeof(report->strict_helper_parent_image) - 1u] = '\0';

    report->state_observed = 1u;
    report->residue_gate = rt_io_operator_strict_residue_gate(state.residue_gate);
    report->unknown_count = rt_io_operator_bit_count(state.unknown_mask) +
                            rt_io_operator_bit_count(state.category_unknown_mask);
    report->residue_count = state.residue_gate == SAO_RT_IO_STRICT_CHAIN_RESIDUE_CLEAN
                                ? 0u
                                : (report->unknown_count != 0u ? report->unknown_count : 1u);
    report->last_failure_code = state.failure_code;
    report->last_failure_stage = state.failure_stage;
    report->selected_engine =
        projection.runtime_engine == SAO_RT_IO_ENGINE_HYPERVISOR ? SAO_RT_IO_ENGINE_HYPERVISOR : 0u;
}

bool rt_io_operator_strict_success(const sao_launcher_rt_io_operator_report_t& report,
                                   uint64_t provider_generation) noexcept {
    const bool chain_complete = report.strict_required_mask == SAO_RT_IO_STRICT_CHAIN_STAGE_ALL &&
                                report.strict_prepared_mask == report.strict_required_mask &&
                                report.strict_committed_mask == report.strict_required_mask &&
                                report.strict_unknown_mask == 0u &&
                                report.strict_rollback_attempted_mask == 0u &&
                                report.strict_rollback_complete_mask == 0u;
    const bool categories_complete =
        report.strict_category_required_mask == SAO_RT_IO_STRICT_CHAIN_CATEGORY_ALL &&
        report.strict_category_prepared_mask == report.strict_category_required_mask &&
        report.strict_category_committed_mask == report.strict_category_required_mask &&
        report.strict_category_unknown_mask == 0u &&
        report.strict_category_rollback_attempted_mask == 0u &&
        report.strict_category_rollback_complete_mask == 0u &&
        report.strict_category_count == SAO_LAUNCHER_RT_IO_STRICT_CATEGORY_COUNT;
    const bool active_projection_complete =
        report.strict_vt_session_id != 0u &&
        report.strict_vt_session_id == report.strict_transaction_id &&
        report.strict_vt_owner_generation != 0u &&
        report.strict_vt_owner_generation == provider_generation &&
        report.strict_vt_requested_engine == SAO_RT_IO_ENGINE_HYPERVISOR &&
        report.strict_vt_runtime_engine == SAO_RT_IO_ENGINE_HYPERVISOR &&
        report.strict_vt_load_path == SAO_RT_IO_VT_LOAD_PATH_HELPER_MANUAL_MAP &&
        report.strict_vt_stage == SAO_RT_IO_VT_STAGE_ACTIVE &&
        report.strict_vt_control_status == 0 && report.strict_vt_capture_status == 0 &&
        report.strict_vt_validation_status == 0 && report.strict_vt_cleanup_status == 0 &&
        report.strict_vt_recovery_status == 0 && report.strict_vt_terminal_reason == 0;
    return report.strict_policy == SAO_RT_IO_STRICT_CHAIN_POLICY_HYPERVISOR_MANDATORY &&
           report.strict_stage == SAO_RT_IO_STRICT_CHAIN_STAGE_VT_ACTIVE &&
           report.strict_transaction_state == SAO_RT_IO_STRICT_CHAIN_STATE_ACTIVE &&
           report.strict_transaction_outcome == SAO_RT_IO_STRICT_CHAIN_OUTCOME_COMMITTED &&
           report.strict_vt_root_active != 0u && report.strict_helper_system != 0u &&
           report.strict_helper_identity_authenticated != 0u &&
           report.strict_final_residue_gate == SAO_RT_IO_STRICT_CHAIN_RESIDUE_CLEAN &&
           active_projection_complete && chain_complete && categories_complete;
}

bool rt_io_operator_strict_recovery_required(
    const SaoRtIoProxyStrictChainRespV1& response) noexcept {
    const auto& state = response.wire.snapshot.state;
    return state.transaction_state == SAO_RT_IO_STRICT_CHAIN_STATE_RECOVERY_REQUIRED ||
           state.transaction_outcome == SAO_RT_IO_STRICT_CHAIN_OUTCOME_RECOVERY_REQUIRED ||
           (response.wire.response_flags &
            (SAO_RT_IO_STRICT_CHAIN_RESPONSE_FLAG_KEEP_RUNNING |
             SAO_RT_IO_STRICT_CHAIN_RESPONSE_FLAG_RETAINED_FOR_RECOVERY)) != 0u;
}

bool rt_io_operator_strict_terminal_clean(const SaoRtIoProxyStrictChainRespV1& response) noexcept {
    const auto& state = response.wire.snapshot.state;
    const uint32_t flags = response.wire.response_flags;
    return state.transaction_state == SAO_RT_IO_STRICT_CHAIN_STATE_TERMINAL &&
           state.transaction_outcome == SAO_RT_IO_STRICT_CHAIN_OUTCOME_ROLLED_BACK &&
           state.stage == SAO_RT_IO_STRICT_CHAIN_STAGE_TERMINAL &&
           state.residue_gate == SAO_RT_IO_STRICT_CHAIN_RESIDUE_CLEAN && state.unknown_mask == 0u &&
           state.category_unknown_mask == 0u &&
           (flags & SAO_RT_IO_STRICT_CHAIN_RESPONSE_FLAG_TERMINAL_CLEAN) != 0u &&
           (flags & (SAO_RT_IO_STRICT_CHAIN_RESPONSE_FLAG_KEEP_RUNNING |
                     SAO_RT_IO_STRICT_CHAIN_RESPONSE_FLAG_RETAINED_FOR_RECOVERY)) == 0u;
}

void rt_io_operator_cache_strict_chain(sao_platform_ctx* ctx,
                                       const SaoRtIoProxyStrictChainRespV1& response) noexcept {
    if (ctx == nullptr)
        return;
    const auto& state = response.wire.snapshot.state;
    if (state.transaction_id != 0u && state.chain_generation != 0u) {
        ctx->rt_io_strict_transaction_id = state.transaction_id;
        ctx->rt_io_strict_chain_generation = state.chain_generation;
    }
}

extern "C++" {

template <typename Request>
void rt_io_operator_initialize_strict_request(Request* request, const char magic[4]) noexcept {
    *request = Request{};
    std::memcpy(request->header.magic, magic, sizeof(request->header.magic));
    request->header.version = SAO_RT_IO_STRICT_CHAIN_BODY_VERSION;
    request->header.header_size = sizeof(SaoRtIoVersionedPayloadHeader);
    request->header.struct_size = sizeof(*request);
}

} // extern "C++"

bool rt_io_operator_call_complete(const sao_launcher_rt_io_operator_report_t& report) noexcept {
    return report.call_authenticated != 0u && report.call_transport_complete != 0u &&
           report.call_request_id_matched != 0u && report.call_committed != 0u;
}

bool rt_io_operator_state_has_no_unknown_or_restore(
    const SaoRtIoProductionStateWireV1& state,
    const sao_launcher_rt_io_operator_report_t& report) noexcept {
    return report.state_observed != 0u && rt_io_operator_production_enums_known(state) &&
           report.restore_mask == 0u;
}

bool rt_io_operator_state_clean(const SaoRtIoProductionStateWireV1& state,
                                const sao_launcher_rt_io_operator_report_t& report) noexcept {
    return rt_io_operator_state_has_no_unknown_or_restore(state, report) &&
           report.resources_absent != 0u && state.ci_mutation_active == 0u &&
           state.callbacks_suppressed == 0u && state.watchdog_started == 0u &&
           state.native_io_pending == 0u && state.auxiliary_handle_count == 0u &&
           state.r1_handle_open == 0u && state.r3_handle_open == 0u &&
           state.cached_write_state == 0u && state.cached_write_in_flight == 0u &&
           state.r3_map_admission_open == 0u && state.active_r3_maps == 0u &&
           state.r3_operations_inflight == 0u && report.admission_mask == 0u &&
           report.provider_retained == 0u;
}

uint32_t
rt_io_operator_live_options(const sao_launcher_rt_io_operator_options_t& options) noexcept {
    uint32_t flags = 0u;
    if (options.input_checks != 0u) {
        flags |= SAO_RT_IO_LIVE_VALIDATE_OPTION_MOUSE_ZERO_MOVE;
        flags |= SAO_RT_IO_LIVE_VALIDATE_OPTION_F24_DOWN_UP;
    }
    if (options.r5_check != 0u)
        flags |= SAO_RT_IO_LIVE_VALIDATE_OPTION_R5_FALLBACK_PROBE;
    if (options.mf_check != 0u)
        flags |= SAO_RT_IO_LIVE_VALIDATE_OPTION_MF_FALLBACK_DIAGNOSTIC;
    return flags;
}

// Auxiliary window generations registered by the tagWND chain must be revoked
// before the controller is destroyed; the tokens are per-HWND, so a failure on
// one does not invalidate the others.
sao_status_t revoke_window_rect_aux(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (!ctx->window_rect_aux_registered)
        return SAO_STATUS_OK;
    if (ctx->window_rect_controller == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    sao_status_t first_failure = SAO_STATUS_OK;
    const SaoRtIoWindowToken tokens[]{ctx->window_rect_control_token,
                                      ctx->window_rect_owner_token};
    for (const SaoRtIoWindowToken& token : tokens) {
        if (token.hwnd == 0u || token.generation == 0u)
            continue;
        const sao_status_t status = sao_rt_io_window_rect_revoke(ctx->window_rect_controller, &token);
        if (status != SAO_STATUS_OK && first_failure == SAO_STATUS_OK)
            first_failure = status;
    }
    ctx->window_rect_control_token = {};
    ctx->window_rect_owner_token = {};
    ctx->window_rect_aux_registered = false;
    return first_failure;
}

sao_status_t prepare_rt_io_operator_shutdown(sao_platform_ctx* ctx) noexcept {
    if (ctx == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    const sao_status_t shared_ui_status = retire_shared_ui_owners(ctx);
    if (shared_ui_status != SAO_STATUS_OK)
        return shared_ui_status;
#endif
    if (ctx->window_rect_registered) {
        const sao_status_t status =
            sao_rt_io_window_rect_revoke(ctx->window_rect_controller, &ctx->window_rect_token);
        if (status != SAO_STATUS_OK)
            return status;
        ctx->window_rect_registered = false;
        ctx->window_rect_token = {};
    }
    const sao_status_t aux_status = revoke_window_rect_aux(ctx);
    if (aux_status != SAO_STATUS_OK)
        return aux_status;
    if (ctx->window_rect_controller != nullptr) {
        sao_rt_io_window_rect_controller_destroy(ctx->window_rect_controller);
        ctx->window_rect_controller = nullptr;
    }
    return SAO_STATUS_OK;
}

sao_status_t
sao_platform_rt_io_operator_preflight(sao_platform_ctx* ctx,
                                      const sao_launcher_rt_io_operator_options_t* options,
                                      sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT, out_report);
    if (ctx == nullptr || ctx->rt_io_proxy == nullptr || !rtIoOperatorOptionsValid(options) ||
        out_report == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;

    SaoRtIoPreflightV2Resp response{};
    SaoRtIoCallResult call{};
    const sao_status_t status =
        sao_rt_io_proxy_preflight_v2(ctx->rt_io_proxy, options->timeout_ms, &response, &call);
    out_report->status = status;
    out_report->operation_status = response.operation_status;
    rt_io_operator_copy_call(call, out_report);
    rt_io_operator_copy_state(response.production, out_report);

    SaoRtIoProductionCapabilitiesV2Resp capabilities{};
    SaoRtIoCallResult capabilities_call{};
    const sao_status_t capabilities_status = sao_rt_io_proxy_production_capabilities_v2(
        ctx->rt_io_proxy, options->timeout_ms, &capabilities, &capabilities_call);
    const bool capabilities_response_valid =
        capabilities_call.authenticated != 0u && capabilities_call.transport_complete != 0u &&
        capabilities_call.request_id_matched != 0u &&
        capabilities_call.payload_bytes == sizeof(capabilities) &&
        std::memcmp(capabilities.header.magic, SAO_RT_IO_PRODUCTION_CAPABILITIES_V2_RESPONSE_MAGIC,
                    4u) == 0 &&
        capabilities.header.version == SAO_RT_IO_PRODUCTION_CAPABILITIES_V2_VERSION &&
        capabilities.header.header_size == sizeof(SaoRtIoVersionedPayloadHeader) &&
        capabilities.header.struct_size == sizeof(capabilities) &&
        capabilities.header.reserved == 0u &&
        capabilities.capabilities.abi_version ==
            SAO_RT_IO_PRODUCTION_HID_CAPABILITIES_ABI_VERSION_V2 &&
        capabilities.capabilities.struct_size == sizeof(capabilities.capabilities) &&
        capabilities.capabilities.reserved0 == 0u &&
        capabilities.capabilities.vt_readiness <= SAO_RT_IO_PRODUCTION_VT_READINESS_READY;
    const bool capabilities_operation_ok =
        capabilities_response_valid && capabilities.operation_status == SAO_STATUS_OK;

    out_report->residue_gate = response.residue_gate;
    out_report->residue_count = response.residue_count;
    out_report->unknown_count = response.unknown_count;
    out_report->is_admin = response.is_admin;
    out_report->is_elevated = response.is_elevated;
    out_report->load_driver_privilege_present = response.load_driver_privilege_present;
    out_report->load_driver_privilege_enabled = response.load_driver_privilege_enabled;
    out_report->hvci_enabled = response.hvci_enabled;
    out_report->vbs_enabled = response.vbs_enabled;
    out_report->provider_observable = response.provider_observable;

    if (capabilities_operation_ok &&
        capabilities.capabilities.vt_readiness == SAO_RT_IO_PRODUCTION_VT_READINESS_UNKNOWN)
        ++out_report->unknown_count;
    if (capabilities_operation_ok &&
        capabilities.capabilities.vt_readiness == SAO_RT_IO_PRODUCTION_VT_READINESS_READY)
        out_report->capability_mask |= SAO_LAUNCHER_RT_IO_CAP_VT_READY;

    const bool observations_complete = rtIoOperatorPreflightObservationsComplete(
        response.is_admin, response.is_elevated, response.load_driver_privilege_present,
        response.load_driver_privilege_enabled, response.hvci_enabled, response.vbs_enabled,
        response.provider_observable);
    const bool capability_call_failed = capabilities_status != SAO_STATUS_OK ||
                                        !capabilities_response_valid || !capabilities_operation_ok;
    if (capability_call_failed) {
        const sao_status_t exact_capability_status =
            capabilities_status != SAO_STATUS_OK
                ? capabilities_status
                : (capabilities_response_valid ? capabilities.operation_status
                                               : SAO_RT_IO_ERR_PAYLOAD_MALFORMED);
        out_report->status = exact_capability_status;
        out_report->operation_status = exact_capability_status;
        rt_io_operator_copy_call(capabilities_call, out_report);
        out_report->complete = 0u;
        out_report->success = 0u;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
        return exact_capability_status;
    }

    const bool complete =
        status == SAO_STATUS_OK && response.operation_status == SAO_STATUS_OK &&
        rt_io_operator_call_complete(*out_report) &&
        response.residue_gate == SAO_RT_IO_OPERATOR_RESIDUE_GATE_CLEAN &&
        response.residue_count == 0u && response.unknown_count == 0u &&
        response.r1_asset.status == SAO_STATUS_OK && response.r1_asset.valid != 0u &&
        response.r3_asset.status == SAO_STATUS_OK && response.r3_asset.valid != 0u &&
        observations_complete && rt_io_operator_state_clean(response.production, *out_report) &&
        rtIoOperatorPreflightFailureStateReady(response.production.last_failure_code,
                                               response.production.last_failure_stage);
    out_report->complete = complete ? 1u : 0u;
    out_report->success = out_report->complete;
    out_report->failure_classification =
        complete ? SAO_LAUNCHER_RT_IO_FAILURE_NONE
                 : (status != SAO_STATUS_OK ? SAO_LAUNCHER_RT_IO_FAILURE_CALL
                                            : SAO_LAUNCHER_RT_IO_FAILURE_PREFLIGHT_INCOMPLETE);
    return complete ? SAO_STATUS_OK : (status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL);
}

sao_status_t sao_platform_rt_io_operator_init(sao_platform_ctx* ctx,
                                              const sao_launcher_rt_io_operator_options_t* options,
                                              sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_INIT, out_report);
    if (ctx == nullptr || ctx->rt_io_proxy == nullptr || !rtIoOperatorOptionsValid(options) ||
        out_report == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (ctx->rt_io_strict_transaction_id == 0u)
        return SAO_STATUS_ERR_NOT_INITIALIZED;

    SaoRtIoStrictChainInitV1Req request{};
    rt_io_operator_initialize_strict_request(&request,
                                             SAO_RT_IO_STRICT_CHAIN_INIT_V1_REQUEST_MAGIC);
    request.policy = SAO_RT_IO_STRICT_CHAIN_POLICY_HYPERVISOR_MANDATORY;
    request.transaction_id = ctx->rt_io_strict_transaction_id;
    request.expected_chain_generation = 0u;
    request.required_stage_mask = SAO_RT_IO_STRICT_CHAIN_STAGE_ALL;
    request.required_category_mask = SAO_RT_IO_STRICT_CHAIN_CATEGORY_ALL;

    SaoRtIoProxyStrictChainRespV1 response{};
    SaoRtIoCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_strict_init(ctx->rt_io_proxy, &request,
                                                            options->timeout_ms, &response, &call);
    out_report->status = status;
    out_report->operation_status = response.wire.operation_status;
    rt_io_operator_copy_call(call, out_report);
    rt_io_operator_copy_strict_response(response, out_report);
    rt_io_operator_cache_strict_chain(ctx, response);
    out_report->driver_strategy = SAO_RT_IO_OPERATOR_DRIVER_STRATEGY_PHYSRW;
    out_report->loaded = out_report->strict_vt_root_active;
    out_report->strict_success =
        rt_io_operator_strict_success(*out_report, response.wire.snapshot.state.provider_generation)
            ? 1u
            : 0u;
    out_report->probe_passed = out_report->strict_success;
    const bool complete =
        status == SAO_STATUS_OK && response.wire.operation_status == SAO_STATUS_OK &&
        rt_io_operator_call_complete(*out_report) && out_report->strict_success != 0u;
    out_report->complete = complete ? 1u : 0u;
    out_report->success = out_report->complete;
    out_report->failure_classification =
        complete ? SAO_LAUNCHER_RT_IO_FAILURE_NONE
                 : (status != SAO_STATUS_OK ? SAO_LAUNCHER_RT_IO_FAILURE_CALL
                                            : SAO_LAUNCHER_RT_IO_FAILURE_INIT_INCOMPLETE);
    return complete ? SAO_STATUS_OK : (status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL);
}

sao_status_t
sao_platform_rt_io_operator_live_validate(sao_platform_ctx* ctx,
                                          const sao_launcher_rt_io_operator_options_t* options,
                                          sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_LIVE, out_report);
    if (ctx == nullptr || ctx->rt_io_proxy == nullptr || !rtIoOperatorOptionsValid(options) ||
        out_report == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;

    SaoRtIoLiveValidateV1Resp response{};
    SaoRtIoCallResult call{};
    const uint32_t live_options = rt_io_operator_live_options(*options);
    const sao_status_t status = sao_rt_io_proxy_live_validate_v1(
        ctx->rt_io_proxy, live_options, options->timeout_ms, &response, &call);
    out_report->status = status;
    out_report->operation_status = response.operation_status;
    rt_io_operator_copy_call(call, out_report);
    rt_io_operator_copy_state(response.final_state, out_report);
    out_report->attempted_step_mask = response.attempted_mask;
    out_report->passed_step_mask = response.passed_mask;
    out_report->unknown_step_mask = response.unknown_mask;
    out_report->partial_step_mask = response.partial_mask;
    const uint64_t requested_mask = out_report->requested_step_mask;
    constexpr uint32_t required_admission =
        SAO_LAUNCHER_RT_IO_ADMISSION_R3_MAP | SAO_LAUNCHER_RT_IO_ADMISSION_CACHED_WRITE |
        SAO_LAUNCHER_RT_IO_ADMISSION_HID_OWNER | SAO_LAUNCHER_RT_IO_ADMISSION_HID_PROBE;
    constexpr uint32_t required_capabilities =
        SAO_LAUNCHER_RT_IO_CAP_R3_SHARED | SAO_LAUNCHER_RT_IO_CAP_MOUSE_PROVENANCE |
        SAO_LAUNCHER_RT_IO_CAP_KEYBOARD_PROVENANCE | SAO_LAUNCHER_RT_IO_CAP_OB |
        SAO_LAUNCHER_RT_IO_CAP_WATCHDOG;
    const bool complete =
        status == SAO_STATUS_OK && response.operation_status == SAO_STATUS_OK &&
        rt_io_operator_call_complete(*out_report) && response.options == live_options &&
        response.terminal_step == UINT32_MAX &&
        response.step_count == SAO_RT_IO_LIVE_VALIDATE_STEP_COUNT &&
        response.attempted_mask == requested_mask && response.passed_mask == requested_mask &&
        response.unknown_mask == 0u && response.partial_mask == 0u &&
        response.active_r3_maps_before == 0u && response.active_r3_maps_after == 0u &&
        rt_io_operator_state_has_no_unknown_or_restore(response.final_state, *out_report) &&
        (out_report->admission_mask & required_admission) == required_admission &&
        (out_report->capability_mask & required_capabilities) == required_capabilities &&
        out_report->selected_backend != 0u &&
        response.final_state.last_failure_code == SAO_STATUS_OK &&
        response.final_state.last_failure_stage == SAO_RT_IO_FAILURE_STAGE_NONE;
    out_report->complete = complete ? 1u : 0u;
    out_report->success = out_report->complete;
    out_report->failure_classification =
        complete ? SAO_LAUNCHER_RT_IO_FAILURE_NONE
                 : (status != SAO_STATUS_OK ? SAO_LAUNCHER_RT_IO_FAILURE_CALL
                                            : SAO_LAUNCHER_RT_IO_FAILURE_LIVE_INCOMPLETE);
    return complete ? SAO_STATUS_OK : (status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL);
}

sao_status_t
sao_platform_rt_io_operator_status(sao_platform_ctx* ctx,
                                   const sao_launcher_rt_io_operator_options_t* options,
                                   sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_STATUS, out_report);
    if (ctx == nullptr || ctx->rt_io_proxy == nullptr || !rtIoOperatorOptionsValid(options) ||
        out_report == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;

    SaoRtIoStrictChainStatusV1Req request{};
    rt_io_operator_initialize_strict_request(&request,
                                             SAO_RT_IO_STRICT_CHAIN_STATUS_V1_REQUEST_MAGIC);
    if (ctx->rt_io_strict_transaction_id != 0u && ctx->rt_io_strict_chain_generation != 0u) {
        request.transaction_id = ctx->rt_io_strict_transaction_id;
        request.chain_generation = ctx->rt_io_strict_chain_generation;
    }

    SaoRtIoProxyStrictChainRespV1 response{};
    SaoRtIoCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_strict_status(
        ctx->rt_io_proxy, &request, options->timeout_ms, &response, &call);
    out_report->status = status;
    out_report->operation_status = response.wire.operation_status;
    rt_io_operator_copy_call(call, out_report);
    rt_io_operator_copy_strict_response(response, out_report);
    rt_io_operator_cache_strict_chain(ctx, response);
    out_report->driver_strategy = SAO_RT_IO_OPERATOR_DRIVER_STRATEGY_PHYSRW;
    out_report->backend_ready = out_report->strict_vt_root_active;
    out_report->strict_success =
        rt_io_operator_strict_success(*out_report, response.wire.snapshot.state.provider_generation)
            ? 1u
            : 0u;
    const bool complete =
        status == SAO_STATUS_OK && response.wire.operation_status == SAO_STATUS_OK &&
        rt_io_operator_call_complete(*out_report) && out_report->strict_success != 0u;
    out_report->complete = complete ? 1u : 0u;
    out_report->success = out_report->complete;
    out_report->failure_classification =
        complete ? SAO_LAUNCHER_RT_IO_FAILURE_NONE
                 : (status != SAO_STATUS_OK ? SAO_LAUNCHER_RT_IO_FAILURE_CALL
                                            : SAO_LAUNCHER_RT_IO_FAILURE_STATUS_INCONSISTENT);
    return complete ? SAO_STATUS_OK : (status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL);
}

sao_status_t
sao_platform_rt_io_operator_cleanup(sao_platform_ctx* ctx,
                                    const sao_launcher_rt_io_operator_options_t* options,
                                    sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_CLEANUP, out_report);
    if (ctx == nullptr || !rtIoOperatorOptionsValid(options) || out_report == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (ctx->rt_io_operator_shutdown) {
        *out_report = ctx->rt_io_cleanup_report;
        return out_report->success != 0u ? SAO_STATUS_OK : out_report->status;
    }
    if (ctx->rt_io_proxy == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;

    sao_status_t status = prepare_rt_io_operator_shutdown(ctx);
    if (status != SAO_STATUS_OK) {
        out_report->status = status;
        out_report->operation_status = status;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE;
        return status;
    }

    SaoRtIoStrictChainStatusV1Req status_request{};
    rt_io_operator_initialize_strict_request(&status_request,
                                             SAO_RT_IO_STRICT_CHAIN_STATUS_V1_REQUEST_MAGIC);
    if (ctx->rt_io_strict_transaction_id != 0u && ctx->rt_io_strict_chain_generation != 0u) {
        status_request.transaction_id = ctx->rt_io_strict_transaction_id;
        status_request.chain_generation = ctx->rt_io_strict_chain_generation;
    }

    SaoRtIoProxyStrictChainRespV1 status_response{};
    SaoRtIoCallResult status_call{};
    status = sao_rt_io_proxy_strict_status(ctx->rt_io_proxy, &status_request, options->timeout_ms,
                                           &status_response, &status_call);
    out_report->status = status;
    out_report->operation_status = status_response.wire.operation_status;
    rt_io_operator_copy_call(status_call, out_report);
    rt_io_operator_copy_strict_response(status_response, out_report);
    rt_io_operator_cache_strict_chain(ctx, status_response);
    const auto& status_state = status_response.wire.snapshot.state;
    const bool status_complete =
        status == SAO_STATUS_OK && status_response.wire.operation_status == SAO_STATUS_OK &&
        rt_io_operator_call_complete(*out_report) && status_state.transaction_id != 0u &&
        status_state.chain_generation != 0u;
    if (!status_complete) {
        out_report->failure_classification = status != SAO_STATUS_OK
                                                 ? SAO_LAUNCHER_RT_IO_FAILURE_CALL
                                                 : SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE;
        return status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL;
    }

    SaoRtIoStrictChainRecoverV1Req recover_request{};
    rt_io_operator_initialize_strict_request(&recover_request,
                                             SAO_RT_IO_STRICT_CHAIN_RECOVER_V1_REQUEST_MAGIC);
    recover_request.transaction_id = status_state.transaction_id;
    recover_request.chain_generation = status_state.chain_generation;

    SaoRtIoProxyStrictChainRespV1 recover_response{};
    SaoRtIoCallResult recover_call{};
    status = sao_rt_io_proxy_strict_recover(ctx->rt_io_proxy, &recover_request, options->timeout_ms,
                                            &recover_response, &recover_call);
    out_report->status = status;
    out_report->operation_status = recover_response.wire.operation_status;
    rt_io_operator_copy_call(recover_call, out_report);
    rt_io_operator_copy_strict_response(recover_response, out_report);
    rt_io_operator_cache_strict_chain(ctx, recover_response);
    out_report->driver_strategy = SAO_RT_IO_OPERATOR_DRIVER_STRATEGY_PHYSRW;
    out_report->cleanup_acknowledged = rt_io_operator_call_complete(*out_report) ? 1u : 0u;
    const bool recovery_required = rt_io_operator_strict_recovery_required(recover_response);
    const bool terminal_clean = rt_io_operator_strict_terminal_clean(recover_response);
    out_report->cleanup_clean = terminal_clean ? 1u : 0u;
    out_report->cleanup_keep_running = recovery_required ? 1u : 0u;
    out_report->provider_retained = recovery_required ? 1u : 0u;
    out_report->strict_success = 0u;
    if (recovery_required) {
        out_report->restore_mask |= SAO_LAUNCHER_RT_IO_RESTORE_PROVIDER_RETAINED;
        out_report->complete = 0u;
        out_report->success = 0u;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE;
        return status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL;
    }

    const bool complete = status == SAO_STATUS_OK &&
                          recover_response.wire.operation_status == SAO_STATUS_OK &&
                          rt_io_operator_call_complete(*out_report) && terminal_clean;
    out_report->complete = complete ? 1u : 0u;
    out_report->success = out_report->complete;
    out_report->failure_classification =
        complete ? SAO_LAUNCHER_RT_IO_FAILURE_NONE
                 : (status != SAO_STATUS_OK ? SAO_LAUNCHER_RT_IO_FAILURE_CALL
                                            : SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE);
    if (!complete)
        return status != SAO_STATUS_OK ? status : SAO_STATUS_INTERNAL;

    ctx->rt_io_operator_shutdown = true;
    ctx->rt_io_cleanup_report = *out_report;
    return SAO_STATUS_OK;
}

// Stage 1 — surfaces.  Settings/theme, the DC-mutation coordinator, the
// overlay host, the compositor and the SDK compositor binding.  Deliberately
// free of driver/helper/plugin work so the Link Start intro can be displayed
// as soon as this returns.
sao_status_t sao_platform_bringup_surface(const sao_platform_config* cfg,
                                          sao_platform_ctx** ctx_out) {
    if (!cfg || !ctx_out || !cfg->base_dir)
        return SAO_STATUS_INVALID_ARGUMENT;
    *ctx_out = nullptr;

    EnvironmentVariableRollback base_dir_environment(L"SAO_BASE_DIR");
    if (!base_dir_environment.valid() || !base_dir_environment.set(cfg->base_dir))
        return SAO_STATUS_ERR_OS_CALL_FAILED;

    auto* ctx = new (std::nothrow) sao_platform_ctx{};
    if (!ctx)
        return SAO_STATUS_INTERNAL;

    const auto delete_and_fail = [&](const char* stage, sao_status_t failure_status) noexcept {
        trace_platform_bringup_failure(stage, failure_status);
        clear_settings_bindings();
        delete ctx;
        return failure_status;
    };
    const auto rollback_and_fail = [&](const char* stage, sao_status_t failure_status) noexcept {
        return rollback_platform_bringup(ctx, ctx_out,
                                         trace_platform_bringup_failure(stage, failure_status));
    };

    sao_status_t status = create_ai_editor_owner(cfg->base_dir, ctx->ai_editor);
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("create_ai_editor_owner", status);
    }
    status = create_settings_owner(cfg->base_dir, ctx->settings_owner);
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("create_settings_owner", status);
    }
    sao::launcher::settings_owner::LoadInfo load_info{};
    status = ctx->settings_owner->load(load_info);
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("settings_owner_load", status);
    }
    status = sao::launcher::settings::settings_panel_bind_owner(ctx->settings_owner.get());
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("settings_panel_bind_owner", status);
    }
    status = sao::launcher::settings::settings_profiles_bind_owner(ctx->settings_owner.get());
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("settings_profiles_bind_owner", status);
    }
    status = sao_launcher_hotkey_set_settings_owner(ctx->settings_owner.get());
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("hotkey_settings_owner_bind", status);
    }
    status = ctx->settings_owner->get_truthy("nervgear_mode", true, ctx->nervgear_mode);
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("settings_owner_nervgear_mode", status);
    }
    bool persisted_streaming_mode = false;
    status = ctx->settings_owner->get_truthy("streaming_mode", true, persisted_streaming_mode);
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("settings_owner_streaming_mode", status);
    }
    status = ctx->settings_owner->get_truthy("sao_screencap_protection", true,
                                             ctx->screencap_protection);
    if (status != SAO_STATUS_OK) {
        return delete_and_fail("settings_owner_sao_screencap_protection", status);
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
    action_authority.license_activation = false;
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
        return rollback_and_fail("settings_theme_read_process_theme", status);
    }
    status = sao_ui_theme_get_active_id(&ctx->previous_theme);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_theme_get_active_id", status);
    }
    ctx->restore_theme_on_rollback = true;
    status = sao_ui_theme_set_active_id(runtime_theme_id(restored_theme));
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_theme_set_active_id", status);
    }

    const bool safe_mode = cfg->safe_mode != 0;

    SaoUiDcMutationProviderV3 dc_mutation_provider{};
    const SaoUiDcMutationProviderV3* selected_mutation_provider = nullptr;
    if (!safe_mode) {
        dc_mutation_provider.struct_size = sizeof(dc_mutation_provider);
        dc_mutation_provider.hide_window_rect = &hide_window_rect;
        dc_mutation_provider.hide_exstyle = &hide_exstyle;
        dc_mutation_provider.unlink_z_order = &unlink_z_order;
        dc_mutation_provider.user_data = ctx;
        selected_mutation_provider = &dc_mutation_provider;
    }
    status = sao_ui_dc_mutation_coordinator_create_ex_v3(selected_mutation_provider,
                                                         &ctx->dc_mutation_coordinator);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_dc_mutation_coordinator_create", status);
    }

    SaoOverlayHostConfig overlay_cfg{};
    overlay_cfg.dc_mutation_coordinator = ctx->dc_mutation_coordinator;
    overlay_cfg.sao_screencap_protection =
        ctx->screencap_protection && ctx->builtin_action_state.streaming_mode;
    overlay_cfg.protection_provider = &apply_overlay_protection_provider;
    overlay_cfg.protection_provider_user_data = ctx;
    status = sao_ui_overlay_host_create(&overlay_cfg, &ctx->overlay_host);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_overlay_host_create", status);
    }
    const auto render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    if (render_hwnd == nullptr) {
        return rollback_and_fail("ui_overlay_host_hwnd", SAO_STATUS_ERR_HANDLE_INVALID);
    }
    status = sao_ui_compositor_create(ctx->overlay_host, nullptr, &ctx->compositor);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_compositor_create", status);
    }
    status = sao_ui_overlay_host_set_size_fn(ctx->overlay_host, &resize_linkstart_for_host, ctx);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_linkstart_size_callback", status);
    }
    status =
        sao_ui_overlay_host_set_dpi_changed_fn(ctx->overlay_host, &resize_linkstart_for_dpi, ctx);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_linkstart_dpi_callback", status);
    }
    try {
        ctx->hotkey_owner = std::make_unique<sao::launcher::hotkey::Owner>(ctx->compositor);
    } catch (...) {
        return rollback_and_fail("hotkey_owner_create", SAO_STATUS_ERR_UNKNOWN);
    }
    status = ctx->hotkey_owner->set_owner_wake_window(render_hwnd);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("hotkey_owner_wake_window", status);
    }
    status = map_sdk_runtime_status(sao_sdk_platform_bind_ui_compositor(ctx->compositor));
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("sdk_platform_bind_ui_compositor", status);
    }
    ctx->sdk_compositor_bound = true;
    *ctx_out = ctx;
    base_dir_environment.commit();
    return SAO_STATUS_OK;
}

// Stage 2 — driver chain.  Opens the rt_io proxy (SCM/helper bootstrap plus the
// R1/R3/R5 driver stages) and its window-rect controller.  Touches no UI
// object, so the launcher may run it off the owner thread while the Link Start
// intro covers the wait; for the same reason it never rolls the platform back
// itself — a failure is returned and the owner-thread caller tears the context
// down.
sao_status_t sao_platform_bringup_drivers(const sao_platform_config* cfg, sao_platform_ctx* ctx) {
    if (!cfg || !ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (cfg->safe_mode != 0)
        return SAO_STATUS_OK;

    SaoRtIoProxyConfigV3 rt_io_cfg{};
    rt_io_cfg.struct_size = sizeof(rt_io_cfg);
    rt_io_cfg.abi_version = SAO_RT_IO_PROXY_CONFIG_V3_ABI_VERSION;
    rt_io_cfg.v2_config.struct_size = sizeof(rt_io_cfg.v2_config);
    rt_io_cfg.v2_config.abi_version = SAO_RT_IO_PROXY_CONFIG_ABI_VERSION;
    rt_io_cfg.v2_config.ready_policy = SAO_RT_IO_PROXY_READY_POLICY_STRICT_PRODUCTION;
    rt_io_cfg.v2_config.legacy_config.session_name_utf8 = "launcher";
    rt_io_cfg.v2_config.legacy_config.strict_bootstrap = 1;
    rt_io_cfg.v2_config.legacy_config.driver_strategy =
        cfg->rt_io_operator != 0 ? SAO_RT_IO_OPERATOR_DRIVER_STRATEGY_PHYSRW
                                 : SAO_RT_IO_OPERATOR_DRIVER_STRATEGY_DEFAULT;
    // Production always uses the LocalSystem SCM/HIDF bootstrap.  The
    // V2/LEGACY_CHILD API remains available to explicit compatibility
    // and test callers, but launcher defaults no longer bypass the
    // service identity contract.
    rt_io_cfg.bootstrap_mode = SAO_RT_IO_PROXY_BOOTSTRAP_MODE_SCM_STRICT;
    const NTSTATUS nonce_status =
        BCryptGenRandom(nullptr, rt_io_cfg.session_nonce,
                        static_cast<ULONG>(sizeof(rt_io_cfg.session_nonce)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    const NTSTATUS transaction_status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(&rt_io_cfg.transaction_id),
        static_cast<ULONG>(sizeof(rt_io_cfg.transaction_id)), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (nonce_status != 0 || transaction_status != 0) {
        return trace_platform_bringup_failure("rt_io_proxy_open_v3_rng", SAO_STATUS_INTERNAL);
    }
    bool nonce_nonzero = false;
    for (const uint8_t value : rt_io_cfg.session_nonce)
        nonce_nonzero = nonce_nonzero || value != 0u;
    if (!nonce_nonzero)
        rt_io_cfg.session_nonce[0] = 1u;
    rt_io_cfg.transaction_id |= 1ull;
    ctx->rt_io_strict_transaction_id = rt_io_cfg.transaction_id;
    ctx->rt_io_strict_chain_generation = 0u;
    if (cfg->rt_io_operator != 0) {
        rt_io_cfg.v2_config.legacy_config.dev_license_bypass = 0u;
    } else {
#if defined(SAO_LAUNCHER_ACTUAL_DEBUG) || defined(SAO_LAUNCHER_ACCEPTANCE_TESTING)
        rt_io_cfg.v2_config.legacy_config.dev_license_bypass =
            cfg->rt_io_dev_license_bypass != 0 ? 1u : 0u;
#else
        rt_io_cfg.v2_config.legacy_config.dev_license_bypass = 0u;
#endif
    }
    // Operator mode hides the F12 status page by default (stealth posture);
    // rt_io_force_status_page is an explicit opt-in override.
    rt_io_cfg.v2_config.legacy_config.disable_status_page =
        (cfg->rt_io_operator != 0 && cfg->rt_io_force_status_page == 0) ? 1u : 0u;
    sao_status_t status = sao_rt_io_proxy_open_v3(&rt_io_cfg, &ctx->rt_io_proxy);
    if (status != SAO_STATUS_OK) {
        return trace_platform_bringup_failure("rt_io_proxy_open_v3", status);
    }
    status =
        sao_rt_io_window_rect_controller_create(ctx->rt_io_proxy, &ctx->window_rect_controller);
    if (status != SAO_STATUS_OK) {
        return trace_platform_bringup_failure("rt_io_window_rect_controller_create", status);
    }
    return SAO_STATUS_OK;
}

// Stage 3 — engines and their UI surfaces.  Owner-thread only: panels, the
// window-rect registration, the streaming/screencap mode transaction, the
// entity shell and its provider publication all bind compositor objects.
sao_status_t sao_platform_bringup_engines(const sao_platform_config* cfg, sao_platform_ctx* ctx,
                                          sao_platform_ctx** ctx_out) {
    if (!cfg || !ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    const auto rollback_and_fail = [&](const char* stage, sao_status_t failure_status) noexcept {
        return rollback_platform_bringup(ctx, ctx_out,
                                         trace_platform_bringup_failure(stage, failure_status));
    };
    const bool safe_mode = cfg->safe_mode != 0;
    auto& action_authority = ctx->builtin_action_state.authority;
    sao_status_t status = SAO_STATUS_OK;
    HWND render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    if (render_hwnd == nullptr) {
        return rollback_and_fail("ui_overlay_host_hwnd", SAO_STATUS_ERR_HANDLE_INVALID);
    }
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    status = create_shared_ui_owners(cfg->base_dir, ctx, safe_mode);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("create_shared_ui_owners", status);
    }
    action_authority.workshop = ctx->workshop_panel != nullptr;
    action_authority.process_selector = ctx->process_selector_panel != nullptr;
    action_authority.license_activation = ctx->license_panel != nullptr;
    action_authority.fisheye_procedural = ctx->fisheye_backdrop != nullptr;
    action_authority.fisheye_live = ctx->fisheye_backdrop != nullptr;
#endif
    if (!safe_mode) {
        status = sao_rt_io_window_rect_register(
            ctx->window_rect_controller,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(render_hwnd)),
            &ctx->window_rect_token);
        if (status != SAO_STATUS_OK) {
            return rollback_and_fail("rt_io_window_rect_register", status);
        }
        ctx->window_rect_registered = true;
    }

    status = sao_streaming_flow_startup(2.0);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("streaming_flow_startup", status);
    }
    ctx->streaming_flow_started = true;

    status = apply_streaming_mode(ctx->builtin_action_state.streaming_mode, ctx);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("apply_streaming_mode", status);
    }
    SaoUiEntityShellConfig entity_cfg{};
    entity_cfg.action_fn = &entity_action;
    entity_cfg.action_user_data = ctx;
    status =
        sao_ui_entity_shell_create_on_compositor(ctx->compositor, &entity_cfg, &ctx->entity_shell);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_entity_shell_create_on_compositor", status);
    }
    status = sao_ui_entity_shell_set_nervgear_mode(ctx->entity_shell, ctx->nervgear_mode);
    if (status != SAO_STATUS_OK) {
        return rollback_and_fail("ui_entity_shell_set_nervgear_mode", status);
    }
    sao::launcher::hotkey::clear_callbacks();
    sao::launcher::hotkey::set_callback("toggle_sao_menu", [ctx] {
        if (sao_ui_entity_shell_home(ctx->entity_shell) == SAO_STATUS_OK) {
            (void)tick_shared_fisheye(ctx);
            (void)sao_ui_compositor_tick(ctx->compositor);
        }
    });
    sao::launcher::hotkey::set_callback("toggle_float_button", [ctx] {
        if (sao_ui_entity_shell_insert(ctx->entity_shell) == SAO_STATUS_OK) {
            (void)tick_shared_fisheye(ctx);
            (void)sao_ui_compositor_tick(ctx->compositor);
        }
    });
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    auto& authority = ctx->entity_provider_publication.builtin_authority;
    sync_entity_publication_authority(ctx);
    authority.topmost_status = sao::launcher::entity_provider_publication::
        TopmostPublicationStatus::degraded_authority_unavailable;
#endif
    ctx->restore_theme_on_rollback = false;
    ctx->settings_save_enabled = true;
    return SAO_STATUS_OK;
}

// Stage 4 — capture shield.  Runs the anti-screencap chain over every window
// this process owns: per-method availability, the syscall/stub affinity path,
// DWM-thumbnail denial, the process-wide registration sweep and the threat
// reaction.  Requires the overlay HWNDs, so it runs after the engine stage.
sao_status_t sao_platform_bringup_capture_sweep(sao_platform_ctx* ctx);

sao_status_t sao_platform_bringup_capture_shield(const sao_platform_config* cfg,
                                                 sao_platform_ctx* ctx) {
    if (!cfg || !ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (cfg->safe_mode != 0)
        return SAO_STATUS_OK;
    HWND render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    HWND control_hwnd = static_cast<HWND>(sao_ui_overlay_host_control_hwnd(ctx->overlay_host));
    if (render_hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
#if defined(SAO_LAUNCHER_HAS_ANTI_SCREENCAP_CHAIN)
    // Method availability first: the result decides which denial paths below
    // can be relied on, and is reported through the rail telemetry.
    ctx->capture_shield_methods =
        static_cast<uint32_t>(sao_security_anti_screencap_method_probe_all());
    if (sao_security_anti_screencap_syscall_affinity_available()) {
        (void)sao_security_anti_screencap_syscall_affinity_apply(render_hwnd, 1u);
        if (control_hwnd != nullptr)
            (void)sao_security_anti_screencap_syscall_affinity_apply(control_hwnd, 1u);
    }
    if (sao_security_anti_screencap_kernel_sprite_available() &&
        sao_security_anti_screencap_kernel_sprite_status() !=
            SAO_ASC_KERNEL_SPRITE_NO_PROVIDER) {
        (void)sao_security_anti_screencap_kernel_sprite_protect(render_hwnd, true);
        if (control_hwnd != nullptr)
            (void)sao_security_anti_screencap_kernel_sprite_protect(control_hwnd, true);
    }
    (void)sao_security_anti_screencap_dwm_thumbnail_deny(render_hwnd);
    if (control_hwnd != nullptr)
        (void)sao_security_anti_screencap_dwm_thumbnail_deny(control_hwnd);
    // Re-assert the dual-HWND affinity through the host: the host owns both
    // HWNDs and applies the pair symmetrically with rollback.
    if (ctx->screencap_protection) {
        const sao_status_t capture_status =
            sao_ui_overlay_host_set_capture_mode(ctx->overlay_host, true);
        if (capture_status != SAO_STATUS_OK)
            trace_platform_bringup_failure("capture_mode_apply", capture_status);
    }
    const sao_status_t sweep_status = sao_platform_bringup_capture_sweep(ctx);
    if (sweep_status != SAO_STATUS_OK)
        return sweep_status;
    (void)sao_security_anti_screencap_scan_all(render_hwnd, &ctx->capture_shield_threat_flags);
    // A dirty scan turns on the strict posture; a clean one restores the
    // normal posture instead of leaving the threat latch set.
    (void)sao_security_anti_screencap_react_to_capture_threat(
        ctx->capture_shield_threat_flags != 0u);
#endif
    ctx->capture_shield_active = true;
    return SAO_STATUS_OK;
}

// Process-wide registration sweep.  Safe to repeat: already-registered windows
// are skipped, and it closes the startup gap for windows created after the
// shield stage (guide host, AI editor, plugin-owned panels).
sao_status_t sao_platform_bringup_capture_sweep(sao_platform_ctx* ctx) {
    if (ctx == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
#if defined(SAO_LAUNCHER_HAS_ANTI_SCREENCAP_CHAIN)
    (void)sao_security_anti_screencap_register_process_windows();
    HWND render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    if (render_hwnd != nullptr) {
        (void)sao_security_anti_screencap_scan_all(render_hwnd, &ctx->capture_shield_threat_flags);
        if (ctx->capture_shield_active)
            (void)sao_security_anti_screencap_react_to_capture_threat(
                ctx->capture_shield_threat_flags != 0u);
    }
#else
    (void)ctx;
#endif
    return SAO_STATUS_OK;
}

// Stage 5 — tagWND chain.  Registers the auxiliary windows with the window-rect
// controller, then drives the ordered physical mutations the Python overlay
// performs at startup: rcWindow scrub to the 1x1 decoy and ExStyle scrub of
// OVERLAY_EXSTYLE_MASK (mem_probe/_dc.py L794).
sao_status_t sao_platform_bringup_wnd_scrub(const sao_platform_config* cfg, sao_platform_ctx* ctx) {
    if (!cfg || !ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (cfg->safe_mode != 0)
        return SAO_STATUS_OK;
    if (ctx->window_rect_controller == nullptr || ctx->dc_mutation_coordinator == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;

    const auto render_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(ctx->overlay_host));
    const auto control_hwnd = static_cast<HWND>(sao_ui_overlay_host_control_hwnd(ctx->overlay_host));
    const auto owner_hwnd = static_cast<HWND>(sao_ui_overlay_host_owner_hwnd(ctx->overlay_host));
    if (render_hwnd == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;

    // Auxiliary windows: rcWindow/ExStyle transactions need their own
    // generation tokens, and hControl needs a coordinator registration because
    // the host only registers hRender.
    if (!ctx->window_rect_aux_registered) {
        if (control_hwnd != nullptr) {
            const sao_status_t status = sao_rt_io_window_rect_register(
                ctx->window_rect_controller,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(control_hwnd)),
                &ctx->window_rect_control_token);
            if (status != SAO_STATUS_OK)
                return trace_platform_bringup_failure("window_rect_register_control", status);
        }
        if (owner_hwnd != nullptr) {
            const sao_status_t status = sao_rt_io_window_rect_register(
                ctx->window_rect_controller,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner_hwnd)),
                &ctx->window_rect_owner_token);
            if (status != SAO_STATUS_OK)
                return trace_platform_bringup_failure("window_rect_register_owner", status);
        }
        ctx->window_rect_aux_registered = true;
    }

    const uint32_t timeout_ms = kCaptureShieldScrubTimeoutMs;
    const uint32_t settle_ms = kCaptureShieldScrubSettleMs;
    sao_status_t status = sao_ui_dc_mutation_coordinator_submit_hide_window_rect(
        ctx->dc_mutation_coordinator, render_hwnd, &kCaptureShieldRectScrub, settle_ms,
        timeout_ms);
    if (status == SAO_STATUS_ERR_NOT_INITIALIZED) {
        // No rect-scrub provider installed: the chain degrades to USER32/DWM
        // geometry only, exactly like the host's own scrub path.
        status = SAO_STATUS_OK;
    }
    if (status != SAO_STATUS_OK)
        return trace_platform_bringup_failure("wnd_scrub_rect_render", status);

    if (control_hwnd != nullptr) {
        if (ctx->dc_mutation_control_token == nullptr) {
            void* token = nullptr;
            const sao_status_t register_status = sao_ui_dc_mutation_coordinator_register(
                ctx->dc_mutation_coordinator, control_hwnd, &token);
            if (register_status != SAO_STATUS_OK)
                return trace_platform_bringup_failure("dc_mutation_register_control",
                                                      register_status);
            ctx->dc_mutation_control_token = token;
        }
        const sao_status_t control_status = sao_ui_dc_mutation_coordinator_submit_hide_window_rect(
            ctx->dc_mutation_coordinator, control_hwnd, &kCaptureShieldRectScrub, settle_ms,
            timeout_ms);
        if (control_status != SAO_STATUS_OK && control_status != SAO_STATUS_ERR_NOT_INITIALIZED)
            return trace_platform_bringup_failure("wnd_scrub_rect_control", control_status);
    }

    // ExStyle scrub rides the generic JSON lane: operation "host-exstyle",
    // method "hide_exstyle", args {"mask":N}.
    char exstyle_args[64]{};
    (void)sprintf_s(exstyle_args, sizeof(exstyle_args), "{\"mask\":%u}",
                    kOverlayExstyleScrubMask);
    const auto submit_exstyle = [&](HWND hwnd, const char* stage) -> sao_status_t {
        if (hwnd == nullptr)
            return SAO_STATUS_OK;
        const sao_status_t exstyle_status = sao_ui_dc_mutation_coordinator_submit_dc(
            ctx->dc_mutation_coordinator, hwnd, "host-exstyle", "hide_exstyle",
            reinterpret_cast<const uint8_t*>(exstyle_args), std::strlen(exstyle_args));
        if (exstyle_status == SAO_STATUS_ERR_NOT_INITIALIZED)
            return SAO_STATUS_OK;
        return exstyle_status == SAO_STATUS_OK
                   ? SAO_STATUS_OK
                   : trace_platform_bringup_failure(stage, exstyle_status);
    };
    status = submit_exstyle(render_hwnd, "wnd_scrub_exstyle_render");
    if (status != SAO_STATUS_OK)
        return status;
    status = submit_exstyle(control_hwnd, "wnd_scrub_exstyle_control");
    if (status != SAO_STATUS_OK)
        return status;
    status = submit_exstyle(owner_hwnd, "wnd_scrub_exstyle_owner");
    if (status != SAO_STATUS_OK)
        return status;

    // The coordinator dispatches provider mutations on its worker; wait for the
    // lane to drain so the scrub is committed before the hold is released.
    for (uint32_t attempt = 0u; attempt < kCaptureShieldDrainAttempts; ++attempt) {
        SaoDcMutationStats stats{};
        const sao_status_t stats_status =
            sao_ui_dc_mutation_coordinator_stats(ctx->dc_mutation_coordinator, &stats);
        if (stats_status != SAO_STATUS_OK)
            break;
        if (stats.inflight_operations == 0u && stats.queued_operations == 0u)
            break;
        Sleep(kCaptureShieldDrainSleepMs);
    }
    ctx->wnd_scrub_applied = true;

    // hControl was registered with the coordinator for this stage only; the
    // host owns hRender's registration, so this one is invalidated here.
    if (ctx->dc_mutation_control_token != nullptr && control_hwnd != nullptr) {
        (void)sao_ui_dc_mutation_coordinator_invalidate(ctx->dc_mutation_coordinator, control_hwnd,
                                                        2.0);
        ctx->dc_mutation_control_token = nullptr;
    }
    return SAO_STATUS_OK;
}

// Combined bring-up in the historical single-shot order used by the headless
// pipeline and the smoke/operator paths.  App::run() drives the stages
// individually so the Link Start intro can cover the driver stages.
sao_status_t sao_platform_bringup(const sao_platform_config* cfg, sao_platform_ctx** ctx_out) {
    if (!cfg || !ctx_out)
        return SAO_STATUS_INVALID_ARGUMENT;
    sao_status_t status = sao_platform_bringup_surface(cfg, ctx_out);
    if (status != SAO_STATUS_OK)
        return status;
    sao_platform_ctx* ctx = *ctx_out;
    // Ownership is republished by the stage that needs to hand the context
    // back, so a clean rollback never leaves a destroyed context behind.
    *ctx_out = nullptr;
    status = sao_platform_bringup_drivers(cfg, ctx);
    if (status != SAO_STATUS_OK)
        return rollback_platform_bringup(ctx, ctx_out, status);
    status = sao_platform_bringup_engines(cfg, ctx, ctx_out);
    if (status != SAO_STATUS_OK)
        return status;
    *ctx_out = ctx;
    status = sao_platform_bringup_capture_shield(cfg, ctx);
    if (status != SAO_STATUS_OK)
        return rollback_platform_bringup(ctx, ctx_out, status);
    status = sao_platform_bringup_wnd_scrub(cfg, ctx);
    if (status != SAO_STATUS_OK)
        return rollback_platform_bringup(ctx, ctx_out, status);
    return SAO_STATUS_OK;
}

sao_status_t teardown_platform_context(sao_platform_ctx* ctx, bool save_settings) noexcept {
    if (!ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    // The tagWND chain's coordinator registration for hControl is stage-owned;
    // drop it before the overlay host retires its own windows.
    if (ctx->dc_mutation_control_token != nullptr && ctx->dc_mutation_coordinator != nullptr &&
        ctx->overlay_host != nullptr) {
        void* control_hwnd = sao_ui_overlay_host_control_hwnd(ctx->overlay_host);
        if (control_hwnd != nullptr)
            (void)sao_ui_dc_mutation_coordinator_invalidate(ctx->dc_mutation_coordinator,
                                                            control_hwnd, 2.0);
        ctx->dc_mutation_control_token = nullptr;
    }
    const sao_status_t aux_revoke_status = revoke_window_rect_aux(ctx);
    if (aux_revoke_status != SAO_STATUS_OK)
        return aux_revoke_status;
    if (ctx->overlay_host != nullptr) {
        const sao_status_t size_status =
            sao_ui_overlay_host_set_size_fn(ctx->overlay_host, nullptr, nullptr);
        if (size_status != SAO_STATUS_OK)
            return size_status;
        const sao_status_t dpi_status =
            sao_ui_overlay_host_set_dpi_changed_fn(ctx->overlay_host, nullptr, nullptr);
        if (dpi_status != SAO_STATUS_OK)
            return dpi_status;
    }
    drain_deferred_cleanup_for_owner();
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
    const sao_status_t hotkey_status = retire_hotkeys(ctx);
    if (hotkey_status != SAO_STATUS_OK)
        return hotkey_status;
    const sao_status_t settings_panel_status = retire_settings_panel();
    if (settings_panel_status != SAO_STATUS_OK)
        return settings_panel_status;
    drain_deferred_cleanup_for_owner();
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
    if (ctx->linkstart != nullptr) {
        (void)sao_ui_linkstart_dismiss_with_reason(ctx->linkstart,
                                                   SAO_UI_LINKSTART_COMPLETION_TEARDOWN);
        sao_ui_linkstart_destroy(ctx->linkstart);
        ctx->linkstart = nullptr;
        ctx->linkstart_pending_completion = false;
        ctx->linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    }
    if (ctx->keyboard_router != nullptr) {
        const sao_status_t router_status =
            sao_ui_input_router_deep_try_destroy(ctx->keyboard_router);
        if (router_status != SAO_STATUS_OK)
            return router_status;
        ctx->keyboard_router = nullptr;
    }
    const sao_status_t sound_status = sao_ui_sound_shutdown();
    if (sound_status != SAO_STATUS_OK)
        return sound_status;
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
        if (ctx->rt_io_operator_shutdown) {
            sao_rt_io_proxy_destroy(ctx->rt_io_proxy);
        } else {
            const sao_status_t proxy_status = sao_rt_io_proxy_close(ctx->rt_io_proxy);
            if (proxy_status != SAO_STATUS_OK) {
                return proxy_status;
            }
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
    // A teardown of a partially brought-up context reverts the process theme the
    // same way rollback_platform_bringup does; a completed bring-up clears the
    // flag, so normal shutdowns are unaffected.
    if (ctx->restore_theme_on_rollback) {
        ctx->restore_theme_on_rollback = false;
        (void)sao_ui_theme_set_active_id(ctx->previous_theme);
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
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    return bindPluginsWithAuthority(
        ctx->plugins_registry, registry,
        [](void* user_data) -> sao_status_t {
            return sync_plugin_runtime_authority(static_cast<sao_platform_ctx*>(user_data));
        },
        ctx);
#else
    ctx->plugins_registry = registry;
    return SAO_STATUS_OK;
#endif
}

// Concrete recorder for the platform composition provider. runPipeline's
// runtime installer step forwards outcomes through the file-scope
// function pointer sao::launcher::runtime_installer_glue::g_record_outcome;
// the static initializer below wires this recorder into that slot at
// translation-unit load time so runPipeline can call across the boundary
// without having a complete sao_platform_ctx type in scope.
void record_runtime_installer_outcome_impl(void* platform_ctx, bool ok) noexcept {
    if (platform_ctx == nullptr)
        return;
    auto* ctx = static_cast<sao_platform_ctx*>(platform_ctx);
    ctx->builtin_action_state.authority.runtime_installer = ok;
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
    sync_entity_publication_authority(ctx);
#endif
}

struct RuntimeInstallerRecorderRegistration {
    RuntimeInstallerRecorderRegistration() noexcept {
        sao::launcher::runtime_installer_glue::g_record_outcome =
            &record_runtime_installer_outcome_impl;
    }
};
RuntimeInstallerRecorderRegistration g_runtime_installer_recorder_registration;

// Creates and shows the Link Start intro on the compositor.  The intro is
// decorative, so a creation failure is recorded as a completion reason instead
// of being fatal; the returned status only tells the caller whether it ran.
sao_status_t start_linkstart_intro(sao_platform_ctx* ctx) {
    bool linkstart_started = false;
    ctx->linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    ctx->linkstart_pending_completion = false;
    SaoOverlayHostClientRect host_bounds{};
    if (sao_ui_overlay_host_get_client_rect(ctx->overlay_host, &host_bounds) == SAO_STATUS_OK &&
        host_bounds.width > 0 && host_bounds.height > 0) {
        SaoUiLinkStartConfig linkstart_config{};
        linkstart_config.struct_size = sizeof(linkstart_config);
        linkstart_config.width_px = static_cast<uint32_t>(host_bounds.width);
        linkstart_config.height_px = static_cast<uint32_t>(host_bounds.height);
        sao_ui_linkstart_handle_t linkstart = nullptr;
        const sao_status_t create_status =
            sao_ui_linkstart_create(ctx->compositor, nullptr, &linkstart_config, &linkstart);
        if (create_status == SAO_STATUS_OK && linkstart != nullptr) {
            const uint32_t dpi = sao_ui_overlay_host_current_dpi(ctx->overlay_host);
            sao_status_t linkstart_status =
                sao_ui_linkstart_resize(linkstart, static_cast<uint32_t>(host_bounds.width),
                                        static_cast<uint32_t>(host_bounds.height), dpi);
            if (linkstart_status == SAO_STATUS_OK)
                linkstart_status = sao_ui_linkstart_show(linkstart);
            if (linkstart_status == SAO_STATUS_OK) {
                ctx->linkstart = linkstart;
                ctx->linkstart_pending_completion = true;
                ctx->linkstart_last_tick = GetTickCount64();
                linkstart_started = true;
            } else {
                SaoUiLinkStartCompletionReason reason = SAO_UI_LINKSTART_COMPLETION_NONE;
                (void)sao_ui_linkstart_poll_completion(linkstart, &reason);
                ctx->linkstart_completion_reason =
                    reason == SAO_UI_LINKSTART_COMPLETION_NONE
                        ? (linkstart_status == SAO_STATUS_ERR_DEVICE_LOST
                               ? SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST
                               : SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED)
                        : reason;
                sao_ui_linkstart_destroy(linkstart);
            }
        } else if (create_status == SAO_STATUS_ERR_DEVICE_LOST) {
            ctx->linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST;
        }
    }
    if (!linkstart_started &&
        ctx->linkstart_completion_reason == SAO_UI_LINKSTART_COMPLETION_NONE)
        ctx->linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED;
    return linkstart_started ? SAO_STATUS_OK : SAO_STATUS_UI_ONLINE_FAIL;
}

sao_status_t sao_ui_bring_online(sao_platform_ctx* ctx) {
    if (!ctx || !ctx->overlay_host || !ctx->compositor || !ctx->entity_shell) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    sao_status_t status = SAO_STATUS_OK;
    if (ctx->keyboard_router == nullptr) {
        status = sao_ui_input_router_deep_create(ctx->compositor, &ctx->keyboard_router);
        if (status != SAO_STATUS_OK)
            return status;
    }
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
    sao::launcher::hotkey::load_or_default({
        {"toggle_sao_menu", "Home", VK_HOME, MOD_NOREPEAT},
        {"toggle_float_button", "Insert", VK_INSERT, MOD_NOREPEAT},
    });
    if (!sao::launcher::hotkey::register_all().empty()) {
#if defined(SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION)
        (void)sao::launcher::entity_provider_publication::clear(
            ctx->entity_shell, ctx->entity_action_routes, ctx->entity_provider_publication,
            ctx->nervgear_mode, &sao_ui_entity_shell_set_roots);
#endif
        (void)sao_ui_entity_shell_take_offline(ctx->entity_shell);
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    // Link Start 开场：UI 上线后自动播放。结束边沿由 sao_ui_tick 打标，
    // launcher 轮询 sao_ui_linkstart_poll_finished 后做衔接动作。
    if (ctx->linkstart == nullptr)
        (void)start_linkstart_intro(ctx);
    return SAO_STATUS_OK;
}

sao_status_t sao_ui_take_offline(sao_platform_ctx* ctx) {
    if (!ctx || !ctx->overlay_host || !ctx->entity_shell) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    if (ctx->linkstart != nullptr) {
        (void)sao_ui_linkstart_dismiss_with_reason(ctx->linkstart,
                                                   SAO_UI_LINKSTART_COMPLETION_OFFLINE);
        sao_ui_linkstart_destroy(ctx->linkstart);
        ctx->linkstart = nullptr;
    }
    ctx->linkstart_pending_completion = false;
    ctx->linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_OFFLINE;
    ctx->linkstart_last_tick = 0;
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
    const sao_status_t hotkey_status = retire_hotkeys(ctx);
    if (hotkey_status != SAO_STATUS_OK)
        return hotkey_status;
    const sao_status_t offline_status = sao_ui_entity_shell_take_offline(ctx->entity_shell);
    const sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
    if (status != SAO_STATUS_OK)
        return status;
    return offline_status == SAO_STATUS_OK ? compositor_status : offline_status;
}

// Advances the Link Start intro one frame and turns its end edge into a
// completion reason.  WM_TIMER is coalesced under load, so the delta comes from
// one monotonic clock rather than the nominal frame interval.
sao_status_t tick_linkstart(sao_platform_ctx* ctx) {
    if (!ctx || ctx->linkstart == nullptr)
        return SAO_STATUS_OK;
    const ULONGLONG now = GetTickCount64();
    const auto intro_delta = static_cast<int32_t>(
        std::min<ULONGLONG>(now - ctx->linkstart_last_tick, static_cast<ULONGLONG>(INT32_MAX)));
    ctx->linkstart_last_tick = now;
    const sao_status_t linkstart_status = sao_ui_linkstart_tick(ctx->linkstart, intro_delta);
    if (linkstart_status != SAO_STATUS_OK &&
        linkstart_status != SAO_STATUS_ERR_NOT_INITIALIZED) {
        const auto reason = linkstart_status == SAO_STATUS_ERR_DEVICE_LOST
                                ? SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST
                                : SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED;
        (void)sao_ui_linkstart_dismiss_with_reason(ctx->linkstart, reason);
    }
    bool now_active = false;
    (void)sao_ui_linkstart_is_active(ctx->linkstart, &now_active);
    if (ctx->linkstart_pending_completion && !now_active)
        capture_linkstart_completion(ctx, SAO_UI_LINKSTART_COMPLETION_NATURAL);
    return linkstart_status == SAO_STATUS_ERR_NOT_INITIALIZED ? SAO_STATUS_OK : linkstart_status;
}

// Shows the intro before the entity shell is online.  The overlay host window is
// otherwise published by entity-shell bring-online, which runs after the
// driver/engine bootstrap in the animation-covered order.
sao_status_t sao_ui_intro_show(sao_platform_ctx* ctx, int32_t hold_for_bootstrap) {
    if (!ctx || !ctx->overlay_host || !ctx->compositor)
        return SAO_STATUS_INVALID_ARGUMENT;
    sao_status_t status = sao_ui_overlay_host_set_visible(ctx->overlay_host, true);
    if (status == SAO_STATUS_OK)
        status = start_linkstart_intro(ctx);
    if (status != SAO_STATUS_OK) {
        // Nothing was painted, so do not leave a bare surface on screen.
        (void)sao_ui_overlay_host_set_visible(ctx->overlay_host, false);
        return status;
    }
    if (hold_for_bootstrap != 0 && ctx->linkstart != nullptr)
        status = sao_ui_linkstart_arm_bootstrap_hold(ctx->linkstart);
    return status;
}

sao_status_t sao_ui_intro_publish_bootstrap(sao_platform_ctx* ctx,
                                            const SaoUiLinkStartBootstrap* state) {
    if (!ctx || !state)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (ctx->linkstart == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    return sao_ui_linkstart_set_bootstrap(ctx->linkstart, state);
}

sao_status_t sao_ui_intro_release_bootstrap(sao_platform_ctx* ctx, int32_t failed) {
    if (!ctx)
        return SAO_STATUS_INVALID_ARGUMENT;
    if (ctx->linkstart == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    return sao_ui_linkstart_release_bootstrap_hold(ctx->linkstart, failed);
}

// Frame pump for callers that block on out-of-band work (the bootstrap worker)
// while the intro is on screen.  Only the intro and the compositor are driven;
// entity-shell/panel service needs an online shell and runs in sao_ui_tick.
sao_status_t sao_ui_intro_pump(sao_platform_ctx* ctx) {
    if (!ctx || !ctx->compositor)
        return SAO_STATUS_INVALID_ARGUMENT;
    drain_deferred_cleanup_for_owner();
    const sao_status_t intro_status = tick_linkstart(ctx);
    sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
    if (compositor_status == SAO_STATUS_ERR_DEVICE_LOST)
        compositor_status = SAO_STATUS_OK;
    return intro_status == SAO_STATUS_OK ? compositor_status : intro_status;
}

sao_status_t sao_ui_tick(sao_platform_ctx* ctx, uint32_t elapsed_ms) {
    if (!ctx || !ctx->entity_shell || !ctx->compositor)
        return SAO_STATUS_INVALID_ARGUMENT;
    drain_deferred_cleanup_for_owner();
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
        if (ai_editor_status != SAO_STATUS_OK && ai_editor_status != SAO_STATUS_ERR_CANCELLED) {
            (void)sao_core_logf(SAO_LOG_WARN, "launcher.ai_editor",
                                "owner-thread UI service deferred: status=%d", ai_editor_status);
        }
#endif
        if (status == SAO_STATUS_OK && ai_editor_status != SAO_STATUS_ERR_CANCELLED) {
            status = ai_editor_status;
        }
    }
#if defined(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    if (ctx->plugin_manager_panel) {
        const sao_status_t plugin_manager_status = ctx->plugin_manager_panel->service_ui();
        if (status == SAO_STATUS_OK && plugin_manager_status != SAO_STATUS_OK)
            status = plugin_manager_status;
    }
    if (ctx->process_selector_panel) {
        const sao_status_t process_selector_status = ctx->process_selector_panel->service_ui();
        if (status == SAO_STATUS_OK && process_selector_status != SAO_STATUS_OK)
            status = process_selector_status;
    }
    if (ctx->workshop_panel) {
        const sao_status_t workshop_status = ctx->workshop_panel->service_ui();
        if (status == SAO_STATUS_OK && workshop_status != SAO_STATUS_OK)
            status = workshop_status;
    }
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
    if (ctx->license_panel) {
        const sao_status_t license_status = ctx->license_panel->service_ui();
        if (status == SAO_STATUS_OK && license_status != SAO_STATUS_OK)
            status = license_status;
    }
#endif
#endif
    const sao_status_t fisheye_status = tick_shared_fisheye(ctx);
    if (status == SAO_STATUS_OK && fisheye_status != SAO_STATUS_OK)
        status = fisheye_status;
    // Link Start 开场：驱动完成边沿。 show() 已由 sao_ui_bring_online 开头调用；
    // tick 内部自己推进到 total_duration 后转入 inactive 并隐藏图层。
    (void)tick_linkstart(ctx);
    sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
    if (compositor_status != SAO_STATUS_OK && ctx->linkstart_pending_completion) {
        const auto reason = compositor_status == SAO_STATUS_ERR_DEVICE_LOST
                                ? SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST
                                : SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED;
        (void)sao_ui_linkstart_dismiss_with_reason(ctx->linkstart, reason);
        capture_linkstart_completion(ctx, reason);
        compositor_status = sao_ui_compositor_tick(ctx->compositor);
    }
    // The compositor owns device recreation and retries it on the next frame.
    if (compositor_status == SAO_STATUS_ERR_DEVICE_LOST)
        compositor_status = SAO_STATUS_OK;
    return status == SAO_STATUS_OK ? compositor_status : status;
}

sao_status_t sao_ui_linkstart_poll_finished(sao_platform_ctx* ctx, int32_t* out_just_finished) {
    int32_t reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    return sao_ui_linkstart_poll_finished_ex(ctx, out_just_finished, &reason);
}

sao_status_t sao_ui_linkstart_poll_finished_ex(sao_platform_ctx* ctx, int32_t* out_just_finished,
                                               int32_t* out_completion_reason) {
    if (out_just_finished == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_completion_reason == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_just_finished = 0;
    *out_completion_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    if (ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (ctx->linkstart_completion_reason != SAO_UI_LINKSTART_COMPLETION_NONE) {
        *out_just_finished = 1;
        *out_completion_reason = static_cast<int32_t>(ctx->linkstart_completion_reason);
        ctx->linkstart_completion_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    }
    return SAO_STATUS_OK;
}

sao_status_t sao_ui_handle_message(sao_platform_ctx* ctx, uint32_t message, uintptr_t w_param,
                                   intptr_t l_param, int32_t* out_handled) {
    if (!ctx || !ctx->entity_shell || !out_handled) {
        return SAO_STATUS_INVALID_ARGUMENT;
    }
    *out_handled = 0;
    if (message == WM_TIMER && w_param == kUiFrameTimerId) {
        *out_handled = 1;
        return sao_ui_tick(ctx, kUiFrameIntervalMs);
    }
    if (message == sao::launcher::hotkey::kCaptureCompletionMessage) {
        *out_handled = 1;
        return ctx->hotkey_owner ? ctx->hotkey_owner->drain_capture_for_owner() : SAO_STATUS_OK;
    }
    if (ctx->linkstart != nullptr &&
        (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
         message == WM_HOTKEY || message == SAO_UI_NATIVE_TEXT_TAB_MESSAGE)) {
        bool intro_active = false;
        const sao_status_t intro_status = sao_ui_linkstart_is_active(ctx->linkstart, &intro_active);
        if (intro_status != SAO_STATUS_OK)
            return intro_status;
        if (intro_active) {
            *out_handled = 1;
            return SAO_STATUS_OK;
        }
    }
    if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
         message == SAO_UI_NATIVE_TEXT_TAB_MESSAGE) &&
        ctx->keyboard_router != nullptr && ctx->compositor != nullptr) {
        const HWND host = static_cast<HWND>(sao_ui_compositor_host_hwnd(ctx->compositor));
        const HWND focus = GetFocus();
        if (host != nullptr && focus == host) {
            bool consumed = false;
            const sao_status_t route_status = sao_ui_input_router_feed_raw_win32(
                ctx->keyboard_router, message, static_cast<uint64_t>(w_param),
                static_cast<int64_t>(l_param), &consumed);
            if (route_status != SAO_STATUS_OK && route_status != SAO_STATUS_ERR_NOT_FOUND)
                return route_status;
            *out_handled = consumed ? 1 : 0;
            return SAO_STATUS_OK;
        }
    }
    if (message != WM_HOTKEY)
        return SAO_STATUS_OK;
    if (!sao::launcher::hotkey::dispatch_by_native_id(static_cast<int>(w_param)))
        return SAO_STATUS_OK;
    *out_handled = 1;
    const sao_status_t fisheye_status = tick_shared_fisheye(ctx);
    const sao_status_t compositor_status = sao_ui_compositor_tick(ctx->compositor);
    return fisheye_status == SAO_STATUS_OK ? compositor_status : fisheye_status;
}

sao_status_t sao_platform_bind_user_menu(sao_platform_ctx* ctx, void* user_menu) {
    if (ctx == nullptr || user_menu == nullptr || ctx->hotkey_owner == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    static_cast<sao::launcher::UserMenu*>(user_menu)->bind_hotkey_owner(ctx->hotkey_owner.get());
    ctx->user_menu = user_menu;
    return SAO_STATUS_OK;
}
sao_status_t sao_platform_unbind_user_menu(sao_platform_ctx* ctx, void* user_menu) {
    if (ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (user_menu != nullptr && ctx->user_menu != user_menu)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (ctx->user_menu != nullptr) {
        static_cast<sao::launcher::UserMenu*>(ctx->user_menu)
            ->unbind_hotkey_owner(ctx->hotkey_owner.get());
        ctx->user_menu = nullptr;
    }
    return SAO_STATUS_OK;
}
sao_status_t sao_platform_user_guide_presented(sao_platform_ctx* ctx, int32_t* out_presented) {
    if (ctx == nullptr || out_presented == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_presented = 0;
    if (!ctx->settings_owner)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    bool presented = false;
    const sao_status_t status =
        ctx->settings_owner->get_truthy("user_guide_presented", false, presented);
    if (status == SAO_STATUS_OK)
        *out_presented = presented ? 1 : 0;
    return status;
}
sao_status_t sao_platform_mark_user_guide_presented(sao_platform_ctx* ctx) {
    if (ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return ctx->settings_owner
               ? ctx->settings_owner->set_value_and_save("user_guide_presented", true)
               : SAO_STATUS_ERR_NOT_INITIALIZED;
}
#else
sao_status_t sao_platform_bringup(const sao_platform_config*, sao_platform_ctx**) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_bringup_surface(const sao_platform_config*, sao_platform_ctx**) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_bringup_drivers(const sao_platform_config*, sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_bringup_engines(const sao_platform_config*, sao_platform_ctx*,
                                          sao_platform_ctx**) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}

sao_status_t sao_platform_bringup_capture_shield(const sao_platform_config*, sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}

sao_status_t sao_platform_bringup_capture_sweep(sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}

sao_status_t sao_platform_bringup_wnd_scrub(const sao_platform_config*, sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_intro_show(sao_platform_ctx*, int32_t) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_intro_publish_bootstrap(sao_platform_ctx*, const SaoUiLinkStartBootstrap*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_intro_release_bootstrap(sao_platform_ctx*, int32_t) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_ui_intro_pump(sao_platform_ctx*) {
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
sao_status_t sao_ui_linkstart_poll_finished(sao_platform_ctx*, int32_t* out_just_finished) {
    int32_t reason = 0;
    return sao_ui_linkstart_poll_finished_ex(nullptr, out_just_finished, &reason);
}
sao_status_t sao_ui_linkstart_poll_finished_ex(sao_platform_ctx*, int32_t* out_just_finished,
                                               int32_t* out_completion_reason) {
    if (out_just_finished == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (out_completion_reason == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_just_finished = 0;
    *out_completion_reason = 0;
    return SAO_STATUS_OK;
}
sao_status_t sao_ui_handle_message(sao_platform_ctx*, uint32_t, uintptr_t, intptr_t,
                                   int32_t* out_handled) {
    if (out_handled)
        *out_handled = 0;
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_bind_user_menu(sao_platform_ctx*, void*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_unbind_user_menu(sao_platform_ctx*, void*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_user_guide_presented(sao_platform_ctx*, int32_t* out_presented) {
    if (out_presented == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_presented = 1;
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_mark_user_guide_presented(sao_platform_ctx*) {
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t
sao_platform_rt_io_operator_preflight(sao_platform_ctx*,
                                      const sao_launcher_rt_io_operator_options_t* options,
                                      sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT, out_report);
    if (out_report != nullptr) {
        out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t sao_platform_rt_io_operator_init(sao_platform_ctx*,
                                              const sao_launcher_rt_io_operator_options_t* options,
                                              sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_INIT, out_report);
    if (out_report != nullptr) {
        out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t
sao_platform_rt_io_operator_live_validate(sao_platform_ctx*,
                                          const sao_launcher_rt_io_operator_options_t* options,
                                          sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_LIVE, out_report);
    if (out_report != nullptr) {
        out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t
sao_platform_rt_io_operator_status(sao_platform_ctx*,
                                   const sao_launcher_rt_io_operator_options_t* options,
                                   sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_STATUS, out_report);
    if (out_report != nullptr) {
        out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
    }
    return SAO_STATUS_NOT_IMPLEMENTED;
}
sao_status_t
sao_platform_rt_io_operator_cleanup(sao_platform_ctx*,
                                    const sao_launcher_rt_io_operator_options_t* options,
                                    sao_launcher_rt_io_operator_report_t* out_report) {
    initializeRtIoOperatorReport(options, SAO_LAUNCHER_RT_IO_STAGE_CLEANUP, out_report);
    if (out_report != nullptr) {
        out_report->status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->operation_status = SAO_STATUS_NOT_IMPLEMENTED;
        out_report->failure_classification = SAO_LAUNCHER_RT_IO_FAILURE_CALL;
    }
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
    stop_anti_debug_worker();
    try {
        if (cfg->enable_anti_debug) {
            SaoSecurityAntiDebugEvidence evidence{};
            evidence.struct_size = SAO_SECURITY_ANTI_DEBUG_EVIDENCE_SIZE;
            evidence.abi_version = SAO_SECURITY_ANTI_DEBUG_EVIDENCE_ABI_VERSION;
            const sao_status_t status = sao_security_anti_debug_evaluate(
                SAO_SECURITY_ANTI_DEBUG_POLICY_FAIL_STRONG, &evidence);
            log_anti_debug_result("startup", status, evidence);
            if (status != SAO_STATUS_OK)
                return status;
            if (evidence.verdict == SAO_SECURITY_ANTI_DEBUG_VERDICT_BLOCK)
                return SAO_STATUS_PLATFORM_INIT_FAIL;
        }
#if defined(SAO_LAUNCHER_USER_EVASION_PROVIDER)
        if (cfg->enable_user_evasion && !evaluate_user_evasion(cfg->strict_user_evasion != 0))
            return SAO_STATUS_PLATFORM_INIT_FAIL;
#else
        if (cfg->enable_user_evasion) {
#if defined(SAO_LAUNCHER_CORE_LOG_PROVIDER)
            (void)sao_core_log(SAO_LOG_WARN, "launcher.security.user_evasion",
                               "provider unavailable; evidence collection skipped");
#endif
            if (cfg->strict_user_evasion)
                return SAO_STATUS_NOT_IMPLEMENTED;
        }
#endif
#if defined(SAO_LAUNCHER_ANTI_DUMP_PROVIDER)
        if (cfg->enable_anti_dump) {
            // Keep the snapshot watcher active without erasing the launcher's
            // complete PE headers. Platform, plugin, and UI startup still create
            // CRT threads after security initialization, and zeroing the headers
            // makes that thread bootstrap fail in hardened builds.
            (void)sao_security_anti_dump_snapshot_watch_start(1000u);
        }
#endif
        if (cfg->enable_anti_debug) {
            const sao_status_t worker_status =
                start_anti_debug_worker(cfg->anti_debug_poll_interval_seconds);
            if (worker_status != SAO_STATUS_OK) {
#if defined(SAO_LAUNCHER_ANTI_DUMP_PROVIDER)
                (void)sao_security_anti_dump_snapshot_watch_stop();
#endif
                return worker_status;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        stop_anti_debug_worker();
#if defined(SAO_LAUNCHER_ANTI_DUMP_PROVIDER)
        (void)sao_security_anti_dump_snapshot_watch_stop();
#endif
        return SAO_STATUS_INTERNAL;
    }
}
sao_status_t sao_security_shutdown(void) {
    stop_anti_debug_worker();
#if defined(SAO_LAUNCHER_ANTI_DUMP_PROVIDER)
    (void)sao_security_anti_dump_snapshot_watch_stop();
#endif
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
