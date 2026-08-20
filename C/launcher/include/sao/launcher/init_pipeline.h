// SAO Auto — launcher/init_pipeline.h
//
// C ABI declarations of every subsystem entry point the launcher calls into.
// This file is the single place cross-subsystem boundaries are described,
// so an ABI break in any subsystem is visible here at link time.
//
// Contract:
//   * All entry points are extern "C".
//   * All parameters and return values are POD.
//   * Ownership of handles is bumped on the caller (subsystem creates,
//     launcher calls the corresponding shutdown).
//   * Every function returns a status code from sao_status_t.

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// Universal status.  Every subsystem uses the same code space so the
// launcher can log them uniformly.
typedef int32_t sao_status_t;

#define SAO_STATUS_OK 0
#define SAO_STATUS_NOT_IMPLEMENTED -1
#define SAO_STATUS_INTERNAL -2
#define SAO_STATUS_INVALID_ARGUMENT -3
#define SAO_STATUS_LICENSE_INVALID -100
#define SAO_STATUS_LICENSE_EXPIRED -101
#define SAO_STATUS_LICENSE_HWID_MISMATCH -102
#define SAO_STATUS_SHELL_TAMPERED -200
#define SAO_STATUS_PLATFORM_INIT_FAIL -300
#define SAO_STATUS_PLUGIN_LOAD_FAIL -400
#define SAO_STATUS_UI_ONLINE_FAIL -500

// ---------------------------------------------------------------------------
// Platform (implemented by platform/core/)
// ---------------------------------------------------------------------------
typedef struct sao_platform_ctx sao_platform_ctx;
typedef struct sao_plugins_registry sao_plugins_registry;

typedef struct sao_platform_config {
    const wchar_t* base_dir;    // BASE_DIR (see working_dir.h)
    const wchar_t* config_path; // may be NULL to use default
    const char* log_level;      // "trace" .. "critical"; NULL == "info"
    int32_t safe_mode;          // non-zero disables non-essential threads
    int32_t streaming_entitled; // verified paid tier or dev bypass
    int32_t rt_io_operator;     // non-zero selects the strict HYPERVISOR chain
    int32_t rt_io_dev_license_bypass; // explicit actual-Debug --no-license propagation
    int32_t rt_io_force_status_page; // non-zero keeps the helper F12 status page
                                      // enabled even when rt_io_operator would
                                      // otherwise disable it
} sao_platform_config;

sao_status_t sao_platform_bringup(const sao_platform_config* cfg, sao_platform_ctx** ctx_out);
sao_status_t sao_platform_bind_plugins(sao_platform_ctx* ctx, sao_plugins_registry* reg);
sao_status_t sao_platform_teardown(sao_platform_ctx* ctx);

// ---------------------------------------------------------------------------
// Security (implemented by security/)
// ---------------------------------------------------------------------------
typedef struct sao_security_config {
    int32_t enable_anti_debug;
    int32_t enable_anti_dump;
    int32_t enable_anti_screencap;
    int32_t enable_obfuscation_runtime;
    int32_t enable_user_evasion;
    int32_t strict_user_evasion;
    uint32_t anti_debug_poll_interval_seconds;
} sao_security_config;

sao_status_t sao_security_init(const sao_security_config* cfg);
sao_status_t sao_security_shutdown(void);

// ---------------------------------------------------------------------------
// Shell (implemented by shell/stub/)
// ---------------------------------------------------------------------------
typedef struct sao_shell_verify_result {
    int32_t tampered; // 0 = OK, non-zero = evidence of tamper
    char reason[256]; // human-readable diagnosis if tampered
} sao_shell_verify_result;

sao_status_t sao_shell_verify_integrity(sao_shell_verify_result* result);
sao_status_t sao_shell_shutdown(void);

// ---------------------------------------------------------------------------
// License (implemented by license/client/)
// ---------------------------------------------------------------------------
typedef struct sao_license_result {
    int32_t valid;
    int64_t expires_utc; // Unix time; 0 for perpetual
    char tier[32];       // "free" | "pro" | "team"
    char hwid_hash[65];  // hex sha256
    char error_msg[256];
} sao_license_result;

sao_status_t sao_license_verify(sao_license_result* out);
sao_status_t sao_license_shutdown(void);

// ---------------------------------------------------------------------------
// Plugins (implemented by plugins/loader/)
// ---------------------------------------------------------------------------
sao_status_t sao_plugins_discover(sao_platform_ctx* ctx, sao_plugins_registry** reg_out);
sao_status_t sao_plugins_activate_autostart(sao_plugins_registry* reg);
sao_status_t sao_plugins_reload_all(sao_plugins_registry* reg);

enum sao_plugins_python_runtime_status_e : int32_t {
    SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE = 0,
    SAO_PLUGINS_PYTHON_RUNTIME_UNCONFIGURED = 1,
    SAO_PLUGINS_PYTHON_RUNTIME_UNAVAILABLE = 2,
    SAO_PLUGINS_PYTHON_RUNTIME_READY = 3,
};

enum sao_plugins_python_launch_strategy_e : int32_t {
    SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED = 0,
    SAO_PLUGINS_PYTHON_LAUNCH_IN_PROCESS = 1,
};

enum sao_plugins_operational_status_e : int32_t {
    SAO_PLUGINS_OPERATIONAL_READY = 0,
    SAO_PLUGINS_OPERATIONAL_DEGRADED = 1,
    SAO_PLUGINS_OPERATIONAL_SHUTDOWN = 2,
};

typedef struct sao_plugins_status_snapshot {
    uint32_t struct_size;
    int32_t python_runtime_status;
    int32_t python_launch_strategy;
    int32_t operational_status;
    sao_status_t last_operation_status;
    sao_status_t last_rollback_status;
    uint32_t rollback_attempted;
    uint32_t rollback_succeeded;
    uint32_t discovered_count;
    uint32_t loaded_count;
    uint32_t enabled_count;
    uint32_t deferred_count;
    uint32_t active_call_count;
} sao_plugins_status_snapshot_t;

sao_status_t sao_plugins_status_snapshot(sao_plugins_registry* reg,
                                         sao_plugins_status_snapshot_t* out_status);
sao_status_t sao_plugins_shutdown(sao_plugins_registry* reg);

// ---------------------------------------------------------------------------
// UI online — the last step before the message loop.  Implemented by
// platform/ui/, but exposed here because the launcher only calls into it
// through the launcher init pipeline.
// ---------------------------------------------------------------------------
sao_status_t sao_ui_bring_online(sao_platform_ctx* ctx);
sao_status_t sao_ui_take_offline(sao_platform_ctx* ctx);
sao_status_t sao_ui_tick(sao_platform_ctx* ctx, uint32_t elapsed_ms);
sao_status_t sao_ui_handle_message(sao_platform_ctx* ctx, uint32_t message, uintptr_t w_param,
                                   intptr_t l_param, int32_t* out_handled);
sao_status_t sao_platform_bind_user_menu(sao_platform_ctx* ctx, void* user_menu);
sao_status_t sao_platform_unbind_user_menu(sao_platform_ctx* ctx, void* user_menu);

// ---------------------------------------------------------------------------
// RT I/O operator flow.
//
// The launcher-facing structs deliberately contain only sanitized scalar
// observations.  The typed rt_io response PODs and proxy handle remain owned
// by the platform composition provider and never cross this boundary.
// ---------------------------------------------------------------------------
enum sao_launcher_rt_io_operator_stage_e : uint32_t {
    SAO_LAUNCHER_RT_IO_STAGE_PREFLIGHT = 1u,
    SAO_LAUNCHER_RT_IO_STAGE_INIT = 2u,
    SAO_LAUNCHER_RT_IO_STAGE_LIVE = 3u,
    SAO_LAUNCHER_RT_IO_STAGE_STATUS = 4u,
    SAO_LAUNCHER_RT_IO_STAGE_CLEANUP = 5u,
};

enum sao_launcher_rt_io_operator_failure_e : uint32_t {
    SAO_LAUNCHER_RT_IO_FAILURE_NONE = 0u,
    SAO_LAUNCHER_RT_IO_FAILURE_NOT_SUBMITTED = 1u,
    SAO_LAUNCHER_RT_IO_FAILURE_CALL = 2u,
    SAO_LAUNCHER_RT_IO_FAILURE_PREFLIGHT_INCOMPLETE = 3u,
    SAO_LAUNCHER_RT_IO_FAILURE_INIT_INCOMPLETE = 4u,
    SAO_LAUNCHER_RT_IO_FAILURE_LIVE_INCOMPLETE = 5u,
    SAO_LAUNCHER_RT_IO_FAILURE_STATUS_INCONSISTENT = 6u,
    SAO_LAUNCHER_RT_IO_FAILURE_CLEANUP_INCOMPLETE = 7u,
};

enum sao_launcher_rt_io_operator_admission_bit_e : uint32_t {
    SAO_LAUNCHER_RT_IO_ADMISSION_R3_MAP = 1u << 0u,
    SAO_LAUNCHER_RT_IO_ADMISSION_CACHED_WRITE = 1u << 1u,
    SAO_LAUNCHER_RT_IO_ADMISSION_HID_OWNER = 1u << 2u,
    SAO_LAUNCHER_RT_IO_ADMISSION_HID_PROBE = 1u << 3u,
};

enum sao_launcher_rt_io_operator_capability_bit_e : uint32_t {
    SAO_LAUNCHER_RT_IO_CAP_R3_SHARED = 1u << 0u,
    SAO_LAUNCHER_RT_IO_CAP_R5_DIRECT = 1u << 1u,
    SAO_LAUNCHER_RT_IO_CAP_MF = 1u << 2u,
    SAO_LAUNCHER_RT_IO_CAP_MOUSE_PROVENANCE = 1u << 3u,
    SAO_LAUNCHER_RT_IO_CAP_KEYBOARD_PROVENANCE = 1u << 4u,
    SAO_LAUNCHER_RT_IO_CAP_OB = 1u << 5u,
    SAO_LAUNCHER_RT_IO_CAP_WATCHDOG = 1u << 6u,
};

enum sao_launcher_rt_io_operator_restore_bit_e : uint32_t {
    SAO_LAUNCHER_RT_IO_RESTORE_PROVIDER_RETAINED = 1u << 0u,
    SAO_LAUNCHER_RT_IO_RESTORE_HANDLE = 1u << 1u,
    SAO_LAUNCHER_RT_IO_RESTORE_MF = 1u << 2u,
    SAO_LAUNCHER_RT_IO_RESTORE_OB = 1u << 3u,
    SAO_LAUNCHER_RT_IO_RESTORE_OB_RECOVERY = 1u << 4u,
    SAO_LAUNCHER_RT_IO_RESTORE_HID_SHARED = 1u << 5u,
    SAO_LAUNCHER_RT_IO_RESTORE_NATIVE_IO = 1u << 6u,
    SAO_LAUNCHER_RT_IO_RESTORE_R3_ACTIVITY = 1u << 7u,
    SAO_LAUNCHER_RT_IO_RESTORE_CACHED_WRITE = 1u << 8u,
    SAO_LAUNCHER_RT_IO_RESTORE_RESOURCE_UNKNOWN = 1u << 9u,
};

typedef struct sao_launcher_rt_io_operator_options {
    uint32_t struct_size;
    uint32_t preflight_only;
    uint32_t input_checks;
    uint32_t r5_check;
    uint32_t mf_check;
    uint32_t exit_after_validation;
    uint32_t timeout_ms;
    uint32_t reserved;
} sao_launcher_rt_io_operator_options_t;

#define SAO_LAUNCHER_RT_IO_STRICT_CATEGORY_COUNT 5u
#define SAO_LAUNCHER_RT_IO_STRICT_HELPER_IMAGE_CAPACITY 1024u

typedef struct sao_launcher_rt_io_operator_strict_category_report {
    uint32_t category;
    uint32_t outcome;
    sao_status_t prepare_status;
    sao_status_t apply_status;
    sao_status_t commit_status;
    sao_status_t rollback_status;
} sao_launcher_rt_io_operator_strict_category_report_t;

typedef struct sao_launcher_rt_io_operator_report {
    uint32_t struct_size;
    uint32_t stage;
    sao_status_t status;
    sao_status_t operation_status;
    uint32_t failure_classification;
    uint32_t success;
    uint32_t complete;
    uint32_t preflight_only;
    uint32_t input_checks;
    uint32_t r5_check;
    uint32_t mf_check;
    uint32_t exit_after_validation;
    uint64_t requested_step_mask;
    uint64_t attempted_step_mask;
    uint64_t passed_step_mask;
    uint64_t unknown_step_mask;
    uint64_t partial_step_mask;
    uint32_t selected_engine;
    uint32_t runtime_tier;
    uint32_t backend;
    uint32_t selected_backend;
    uint32_t driver_strategy;
    uint32_t residue_gate;
    uint32_t residue_count;
    uint32_t unknown_count;
    uint32_t is_admin;
    uint32_t is_elevated;
    uint32_t load_driver_privilege_present;
    uint32_t load_driver_privilege_enabled;
    uint32_t hvci_enabled;
    uint32_t vbs_enabled;
    uint32_t provider_observable;
    uint32_t admission_mask;
    uint32_t capability_mask;
    uint32_t restore_mask;
    uint32_t state_observed;
    uint32_t resources_absent;
    uint32_t loaded;
    uint32_t probe_passed;
    uint32_t backend_ready;
    uint32_t call_authenticated;
    uint32_t call_transport_complete;
    uint32_t call_request_id_matched;
    uint32_t call_committed;
    uint32_t cleanup_acknowledged;
    uint32_t cleanup_clean;
    uint32_t cleanup_keep_running;
    uint32_t provider_retained;
    uint32_t wiper_joined;
    uint32_t engine_cleanup_confirmed;
    uint32_t etw_restore_confirmed;
    int32_t last_failure_code;
    uint32_t last_failure_stage;
    uint32_t r3_uc_patch_failure_reason;
    uint32_t hid_fallback_reason;
    uint32_t strict_policy;
    uint32_t strict_stage;
    uint32_t strict_transaction_state;
    uint32_t strict_transaction_outcome;
    uint64_t strict_transaction_id;
    uint64_t strict_chain_generation;
    uint32_t strict_required_mask;
    uint32_t strict_prepared_mask;
    uint32_t strict_committed_mask;
    uint32_t strict_unknown_mask;
    uint32_t strict_rollback_attempted_mask;
    uint32_t strict_rollback_complete_mask;
    uint32_t strict_category_required_mask;
    uint32_t strict_category_prepared_mask;
    uint32_t strict_category_committed_mask;
    uint32_t strict_category_unknown_mask;
    uint32_t strict_category_rollback_attempted_mask;
    uint32_t strict_category_rollback_complete_mask;
    uint32_t strict_category_count;
    sao_launcher_rt_io_operator_strict_category_report_t
        strict_categories[SAO_LAUNCHER_RT_IO_STRICT_CATEGORY_COUNT];
    uint32_t strict_vt_vendor;
    uint32_t strict_vt_root_active;
    sao_status_t strict_vt_control_status;
    uint64_t strict_vt_session_id;
    uint64_t strict_vt_owner_generation;
    int32_t strict_vt_requested_engine;
    int32_t strict_vt_runtime_engine;
    int32_t strict_vt_load_path;
    int32_t strict_vt_stage;
    sao_status_t strict_vt_capture_status;
    sao_status_t strict_vt_validation_status;
    sao_status_t strict_vt_cleanup_status;
    sao_status_t strict_vt_recovery_status;
    int32_t strict_vt_terminal_reason;
    uint32_t strict_final_residue_gate;
    uint32_t strict_response_flags;
    uint32_t strict_helper_system;
    uint32_t strict_helper_identity_authenticated;
    uint32_t strict_helper_session_id;
    char strict_helper_actual_image[SAO_LAUNCHER_RT_IO_STRICT_HELPER_IMAGE_CAPACITY];
    char strict_helper_parent_image[SAO_LAUNCHER_RT_IO_STRICT_HELPER_IMAGE_CAPACITY];
    uint32_t strict_success;
} sao_launcher_rt_io_operator_report_t;

typedef void (*sao_launcher_rt_io_operator_output_fn)(const char* line_utf8,
                                                       void* user_data);

sao_status_t sao_platform_rt_io_operator_preflight(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_report_t* out_report);
sao_status_t sao_platform_rt_io_operator_init(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_report_t* out_report);
sao_status_t sao_platform_rt_io_operator_live_validate(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_report_t* out_report);
sao_status_t sao_platform_rt_io_operator_status(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_report_t* out_report);
sao_status_t sao_platform_rt_io_operator_cleanup(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_report_t* out_report);

sao_status_t sao_launcher_rt_io_operator_format_json(
    const sao_launcher_rt_io_operator_report_t* report, char* out_utf8,
    size_t out_capacity, size_t* out_bytes_written);

sao_status_t sao_launcher_rt_io_operator_run(
    sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
    sao_launcher_rt_io_operator_output_fn output, void* output_user_data,
    int32_t* out_ready);

// Test-only composition seams.  Production providers call the stable rt_io,
// UI, and security ABIs directly; tests install these hooks to verify launch
// ordering without launching a helper or creating a real HWND.
struct sao_launcher_composition_test_hooks_t {
    sao_status_t (*shell_verify)(sao_shell_verify_result* result, void* user_data);
    sao_status_t (*shell_shutdown)(void* user_data);
    sao_status_t (*license_verify)(sao_license_result* result, void* user_data);
    sao_status_t (*license_shutdown)(void* user_data);
    sao_status_t (*plugins_discover)(sao_platform_ctx* ctx, sao_plugins_registry** registry_out,
                                     void* user_data);
    sao_status_t (*plugins_activate_autostart)(sao_plugins_registry* registry, void* user_data);
    sao_status_t (*plugins_shutdown)(sao_plugins_registry* registry, void* user_data);
    sao_status_t (*security_init)(const sao_security_config* cfg, void* user_data);
    void (*security_shutdown)(void* user_data);
    sao_status_t (*platform_bringup)(const sao_platform_config* cfg, sao_platform_ctx** ctx_out,
                                     void* user_data);
    sao_status_t (*platform_teardown)(sao_platform_ctx* ctx, void* user_data);
    sao_status_t (*ui_bring_online)(sao_platform_ctx* ctx, void* user_data);
    sao_status_t (*ui_take_offline)(sao_platform_ctx* ctx, void* user_data);
    sao_status_t (*ui_tick)(sao_platform_ctx* ctx, uint32_t elapsed_ms, void* user_data);
    sao_status_t (*ui_handle_message)(sao_platform_ctx* ctx, uint32_t message, uintptr_t w_param,
                                      intptr_t l_param, int32_t* out_handled, void* user_data);
    sao_status_t (*rt_io_operator_preflight)(
        sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
        sao_launcher_rt_io_operator_report_t* out_report, void* user_data);
    sao_status_t (*rt_io_operator_init)(
        sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
        sao_launcher_rt_io_operator_report_t* out_report, void* user_data);
    sao_status_t (*rt_io_operator_live_validate)(
        sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
        sao_launcher_rt_io_operator_report_t* out_report, void* user_data);
    sao_status_t (*rt_io_operator_status)(
        sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
        sao_launcher_rt_io_operator_report_t* out_report, void* user_data);
    sao_status_t (*rt_io_operator_cleanup)(
        sao_platform_ctx* ctx, const sao_launcher_rt_io_operator_options_t* options,
        sao_launcher_rt_io_operator_report_t* out_report, void* user_data);
    void* user_data;
};

void sao_launcher_set_composition_test_hooks(const sao_launcher_composition_test_hooks_t* hooks);

// ---------------------------------------------------------------------------
// Headless init pipeline entry point.
//
// A thin, unit-testable wrapper around the same steps ``App::run`` walks,
// but with:
//   * no message loop (blocks in a caller-owned wait instead)
//   * every step is fail-closed except the crash handler
//   * teardown runs in reverse init order and NEVER throws
//
// ``argv`` mirrors argc/argv from ``main``.  ``exit_code_out`` receives
// the same SaoLauncherExitCode value ``App::run`` would emit.
//
// The function returns SAO_STATUS_OK when the pipeline reached UI online
// and exited cleanly.  Any earlier failure returns the propagating
// sao_status_t code and populates ``exit_code_out``.
//
// Test-only hooks
// ---------------
// Setting the global ``sao_launcher_test_hooks_t::pump_once`` to a
// non-null callback makes the run() variant return after that hook
// completes instead of blocking on a real WM_QUIT.  The pipeline tests
// use this to observe teardown order.
struct sao_launcher_init_hooks_t {
    // Called immediately before the reverse-order teardown starts.
    // Returning non-zero aborts the message loop early.  If null,
    // ``run`` blocks on GetMessage until WM_QUIT is posted.
    int (*poll_should_exit)(void* user_data);
    // Called at every teardown step for observability.  ``step_name``
    // points into static storage; do not free.
    void (*on_teardown_step)(const char* step_name, void* user_data);
    void* user_data;
};

// Base directory the launcher resolved (equivalent to Python's
// ``config.BASE_DIR``).  Set by the pipeline right after
// ``resolveWorkingDir`` completes; NOT thread-safe to read before then.
extern wchar_t SaoLauncherBaseDir[260];

sao_status_t sao_launcher_init_pipeline_run(int argc, wchar_t** argv,
                                            const sao_launcher_init_hooks_t* hooks,
                                            int* exit_code_out);

// Retries ownership-preserving cleanup left pending by a headless run.
// Returns SAO_STATUS_OK when no cleanup remains or the retry completes.
sao_status_t sao_launcher_init_pipeline_retry_pending_cleanup(void);

} // extern "C"

#ifdef __cplusplus
namespace sao::launcher {

struct AppState;

// Build the platform ABI config used by both the GUI App and the headless
// pipeline.  log_level_storage must remain alive while config is consumed.
bool buildPlatformConfig(const AppState& state, sao_platform_config& config,
                         char* log_level_storage, std::size_t log_level_capacity) noexcept;

bool isPaidLicenseTier(const char* tier) noexcept;

} // namespace sao::launcher
#endif
