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
