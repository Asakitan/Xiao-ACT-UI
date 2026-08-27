// SAO Auto — launcher/dual_run.h
//
// Dual-run cutover mode.
//
// The Python platform (sao_auto/python/main.py) and the native C++ platform
// (SaoAuto.exe) can coexist on a user's machine.  While cutover is in flight
// we keep both alive so users can pick which one they trust.  This header
// exposes the C ABI the launcher's init pipeline uses to decide, on every
// startup, which platform should drive the current process — and to spawn
// the "other side" as a helper subprocess when the user picks a fallback or
// side-by-side mode.
//
// Contract
// --------
// * All entry points are extern "C".
// * All parameters and return values are POD.
// * No Python.h / pybind11 anywhere in the launcher.  The launcher only
//   invokes ``python.exe`` via CreateProcessW, treating it as an opaque
//   subprocess.
// * The mutex ``Local\SaoAutoLauncherDualRun`` guards ``spawn_python`` and
//   ``spawn_cpp_side_by_side`` so a single dual-run driver never races
//   with itself.  Two independent SaoAuto.exe processes can still race
//   against the driver, but each of those will fail the guard.
//
// Config file
// -----------
// The launcher reads ``%APPDATA%\SaoAuto\dual_run.json`` at startup.  The
// file is optional; when absent the launcher falls back to
// ``CPP_PREFERRED_PYTHON_FALLBACK`` (safest during cutover — try
// the new platform first, silently fall back to Python if it faults).
//
// {
//   "mode": "cpp_only" | "python_only"
//         | "cpp_preferred_python_fallback"
//         | "python_preferred_cpp_fallback"
//         | "dual_side_by_side",
//   "python_exe_path": "E:\\Py\\python.exe",       // optional
//   "python_main_py_path": ".../python/main.py",    // optional
//   "cpp_exe_path": ".../SaoAuto.exe",              // optional; defaults
//                                                   // to GetModuleFileNameW
//   "env_overrides": { "SAO_LOG_LEVEL": "debug" }   // optional
// }

#pragma once

#include "sao/launcher/init_pipeline.h"

#include <cstdint>
#include <windows.h>

extern "C" {

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
typedef int32_t sao_launcher_dual_run_mode_t;

#define SAO_DUAL_RUN_MODE_CPP_ONLY                        0
#define SAO_DUAL_RUN_MODE_PYTHON_ONLY                     1
#define SAO_DUAL_RUN_MODE_CPP_PREFERRED_PYTHON_FALLBACK   2
#define SAO_DUAL_RUN_MODE_PYTHON_PREFERRED_CPP_FALLBACK   3
#define SAO_DUAL_RUN_MODE_DUAL_SIDE_BY_SIDE               4

// ---------------------------------------------------------------------------
// Status codes specific to dual-run (in addition to the sao_status_t codes
// declared in init_pipeline.h).  Kept in the sao_status_t space so callers
// can treat them uniformly.
// ---------------------------------------------------------------------------
#define SAO_LAUNCHER_ALREADY_RUNNING       -600
#define SAO_LAUNCHER_PYTHON_UNAVAILABLE    -601
#define SAO_LAUNCHER_SPAWN_FAILED          -602
#define SAO_LAUNCHER_CONFIG_PARSE_FAILED   -603
#define SAO_LAUNCHER_CONFIG_WRITE_FAILED   -604
#define SAO_LAUNCHER_HANDOFF_TO_PYTHON     -605  // reported by step_zero to
                                                 // signal the pipeline that
                                                 // the current process is
                                                 // NOT the one driving this
                                                 // run — caller should exit.

// Special exit code the launcher returns from wWinMain when
// ``sao_launcher_dual_run_step_zero`` handed off to Python.  The value also
// lives in the ``SaoLauncherExitCode`` enum declared in ``app.h`` — that
// header is the ABI source of truth.  This ``#define`` is retained so
// TU's that only include dual_run.h (for example dual_run.cpp itself, which
// intentionally has no dependency on the App singleton) still see the same
// integer.  When both headers land in one TU, ``app.h`` ``#undef``s this
// macro before declaring the enum entry so the two views do not collide.
#define SAO_EXIT_HANDOFF_TO_PYTHON  100

// ---------------------------------------------------------------------------
// Env-var name every spawned child inherits so it knows which side it is.
// Values: "cpp" | "python" | "cpp_fallback" | "python_fallback".
// ---------------------------------------------------------------------------
#define SAO_DUAL_RUN_ENV_VAR_NAME L"SAO_DUAL_RUN_ROLE"
#define SAO_DUAL_RUN_HANDOFF_RESULT_ENV_VAR_NAME L"SAO_DUAL_RUN_HANDOFF_RESULT"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// One env override entry.  ``key`` and ``value`` are UTF-16 for direct pass
// through CreateEnvironmentBlock.
typedef struct sao_dual_run_env_override {
    wchar_t key[64];
    wchar_t value[512];
} sao_dual_run_env_override;

typedef struct sao_dual_run_config {
    sao_launcher_dual_run_mode_t mode;

    // If the string is empty, the launcher probes PATH.
    wchar_t python_exe_path[260];
    wchar_t python_main_py_path[260];
    wchar_t cpp_exe_path[260];       // if empty, GetModuleFileNameW()

    // Up to 16 environment overrides passed to every spawned child.
    sao_dual_run_env_override env_overrides[16];
    int32_t env_overrides_count;
} sao_dual_run_config;

#define SAO_DUAL_RUN_CONFIG_ABI_VERSION_2 2u
#define SAO_DUAL_RUN_CONFIG_V2_PATH_CAPACITY 32768u
typedef struct sao_dual_run_config_v2 {
    sao_dual_run_config legacy;
    uint32_t struct_size;
    uint32_t abi_version;
    wchar_t python_exe_path[SAO_DUAL_RUN_CONFIG_V2_PATH_CAPACITY];
    wchar_t python_main_py_path[SAO_DUAL_RUN_CONFIG_V2_PATH_CAPACITY];
    wchar_t cpp_exe_path[SAO_DUAL_RUN_CONFIG_V2_PATH_CAPACITY];
} sao_dual_run_config_v2;

void sao_launcher_dual_run_config_v2_default(sao_dual_run_config_v2* cfg);
sao_status_t sao_launcher_dual_run_config_v2_load(sao_dual_run_config_v2* cfg_out);
sao_status_t sao_launcher_dual_run_config_v2_load_from_path(
    const wchar_t* path, sao_dual_run_config_v2* cfg_out);
sao_status_t sao_launcher_dual_run_config_v2_save(const sao_dual_run_config_v2* cfg);
sao_status_t sao_launcher_dual_run_config_v2_save_to_path(
    const wchar_t* path, const sao_dual_run_config_v2* cfg);
sao_status_t sao_launcher_dual_run_config_v2_copy_python_exe_path(
    const sao_dual_run_config_v2* cfg, wchar_t* out_path,
    uint32_t* inout_char_count);
sao_status_t sao_launcher_dual_run_config_v2_copy_python_main_py_path(
    const sao_dual_run_config_v2* cfg, wchar_t* out_path,
    uint32_t* inout_char_count);
sao_status_t sao_launcher_dual_run_config_v2_copy_cpp_exe_path(
    const sao_dual_run_config_v2* cfg, wchar_t* out_path,
    uint32_t* inout_char_count);

sao_status_t sao_launcher_dual_run_copy_python_exe_path(
    const sao_dual_run_config* cfg, wchar_t* out_path, uint32_t* inout_char_count);
sao_status_t sao_launcher_dual_run_copy_python_main_py_path(
    const sao_dual_run_config* cfg, wchar_t* out_path, uint32_t* inout_char_count);
sao_status_t sao_launcher_dual_run_copy_cpp_exe_path(
    const sao_dual_run_config* cfg, wchar_t* out_path, uint32_t* inout_char_count);

// Populate a config with the default mode (CPP_PREFERRED_PYTHON_FALLBACK),
// empty string fields, and zero env overrides.
void sao_launcher_dual_run_config_default(sao_dual_run_config* cfg);

// Load ``%APPDATA%\SaoAuto\dual_run.json``.  On success returns SAO_STATUS_OK
// and populates ``cfg_out``.  On a missing file returns SAO_STATUS_OK with a
// default-initialised config (safe to treat "no file" as "default").  On a
// parse failure returns SAO_LAUNCHER_CONFIG_PARSE_FAILED.
sao_status_t sao_launcher_dual_run_config_load(sao_dual_run_config* cfg_out);

// Same, but read from an explicit path.  Used by tests to point the loader
// at a temp file without polluting %APPDATA%.
sao_status_t sao_launcher_dual_run_config_load_from_path(
    const wchar_t* path, sao_dual_run_config* cfg_out);

// Persist ``cfg`` back to ``%APPDATA%\SaoAuto\dual_run.json`` (creating the
// parent directory if needed).  Returns SAO_LAUNCHER_CONFIG_WRITE_FAILED on
// I/O errors.
sao_status_t sao_launcher_dual_run_config_save(const sao_dual_run_config* cfg);
sao_status_t sao_launcher_dual_run_config_save_to_path(
    const wchar_t* path, const sao_dual_run_config* cfg);

// Convenience: read the mode field directly (calls _load, extracts .mode).
sao_launcher_dual_run_mode_t sao_launcher_dual_run_mode(void);

// ---------------------------------------------------------------------------
// Python probe
// ---------------------------------------------------------------------------
typedef struct sao_dual_run_python_probe {
    int32_t available;              // 1 = Python 3.11+ found
    wchar_t path[260];              // resolved absolute path
    wchar_t version[32];            // "3.11.0" etc.
    int32_t major;
    int32_t minor;
    int32_t patch;
} sao_dual_run_python_probe;

#define SAO_DUAL_RUN_PYTHON_PROBE_ABI_VERSION_2 2u
#define SAO_DUAL_RUN_PYTHON_PROBE_V2_PATH_CAPACITY 32768u
typedef struct sao_dual_run_python_probe_v2 {
    sao_dual_run_python_probe legacy;
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t available;
    wchar_t path[SAO_DUAL_RUN_PYTHON_PROBE_V2_PATH_CAPACITY];
    wchar_t version[32];
    int32_t major;
    int32_t minor;
    int32_t patch;
} sao_dual_run_python_probe_v2;

void sao_launcher_dual_run_python_probe_v2_default(
    sao_dual_run_python_probe_v2* probe);
sao_status_t sao_launcher_dual_run_probe_python_v2(
    const sao_dual_run_config_v2* cfg,
    sao_dual_run_python_probe_v2* probe_out);
sao_status_t sao_launcher_dual_run_copy_probe_path_v2(
    const sao_dual_run_python_probe_v2* probe, wchar_t* out_path,
    uint32_t* inout_char_count);

sao_status_t sao_launcher_dual_run_copy_probe_path(
    const sao_dual_run_python_probe* probe, wchar_t* out_path,
    uint32_t* inout_char_count);

// Probe for a usable Python.  Search order:
//   1. ``cfg.python_exe_path`` if non-empty and File exists.
//   2. Registered PATH via ``where python``.
//   3. Standard fallbacks: ``E:\Py\python.exe``, ``C:\Python311\python.exe``.
// A hit must respond to ``python.exe --version`` with a parsable "Python X.Y.Z"
// string; only versions with ``major==3 && minor>=11`` are accepted.
sao_status_t sao_launcher_dual_run_probe_python(
    const sao_dual_run_config* cfg,
    sao_dual_run_python_probe* probe_out);

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
typedef struct sao_dual_run_spawn_result {
    DWORD   pid;
    HANDLE  process;                 // caller must CloseHandle
    HANDLE  thread;                  // caller must CloseHandle
    int64_t start_time_qpc;          // QueryPerformanceCounter reading at spawn
} sao_dual_run_spawn_result;

// Spawn the Python-side subprocess:
//   ``<python_exe_path>`` ``<python_main_py_path>``
// Injects SAO_DUAL_RUN_ROLE=<role> and any env_overrides into the child.
// ``role`` is a UTF-16 label ("python" or "python_fallback").
//
// The caller owns the returned handles. Handoff paths complete the file
// handshake without waiting on the child; side-by-side paths retain the
// process handle in the status registry for non-blocking observation.
sao_status_t sao_launcher_dual_run_spawn_python(
    const sao_dual_run_config* cfg,
    const wchar_t* role,
    sao_dual_run_spawn_result* result_out);

// Spawn another SaoAuto.exe as a peer subprocess (used by side-by-side and
// by the cpp_fallback branch).  The child receives the same argv the parent
// received, minus a --safe-mode toggle the parent may inject to prevent
// infinite re-fork loops.
sao_status_t sao_launcher_dual_run_spawn_cpp(
    const sao_dual_run_config* cfg,
    const wchar_t* role,
    sao_dual_run_spawn_result* result_out);

// Build the exact CreateProcessW command line used by spawn_cpp.  argv[0]
// is replaced by cpp_exe_path while argv[1..] are preserved byte-for-byte
// after CommandLineToArgvW round-trip.  --safe-mode is appended only when it
// is not already present.  Pass out_command_line=NULL to query characters.
sao_status_t sao_launcher_dual_run_build_cpp_command_line(
    const wchar_t* cpp_exe_path,
    int argc,
    const wchar_t* const* argv,
    wchar_t* out_command_line,
    uint32_t* inout_char_count);

// ---------------------------------------------------------------------------
// Status snapshot — the dual-run driver's view of running children.
// ---------------------------------------------------------------------------
#define SAO_DUAL_RUN_STATUS_MAX_CHILDREN  4

typedef struct sao_dual_run_child_status {
    DWORD    pid;
    wchar_t  role[32];               // matches SAO_DUAL_RUN_ROLE
    int64_t  start_time_qpc;
    int32_t  is_dead;                // 1 if the process has exited
    int32_t  exit_code;              // only meaningful when is_dead == 1
} sao_dual_run_child_status;

typedef struct sao_dual_run_status {
    sao_launcher_dual_run_mode_t mode;
    int32_t  running_children_count;
    sao_dual_run_child_status running_children[SAO_DUAL_RUN_STATUS_MAX_CHILDREN];

    // If step_zero chose a fallback, why?
    wchar_t  fallback_reason[256];
} sao_dual_run_status;

// Read the current dual-run driver snapshot.  Thread-safe; internal state
// is protected by a critical section.
void sao_launcher_dual_run_status(sao_dual_run_status* out);

// Push a child into the status registry so future ``_status`` reads see it.
// Called by tests and by the pipeline when it spawns something.
void sao_launcher_dual_run_register_child(DWORD pid,
                                          const wchar_t* role,
                                          int64_t start_time_qpc,
                                          HANDLE process_handle_owned);

// Record the reason a fallback fired.  Copied into internal storage.
void sao_launcher_dual_run_record_fallback_reason(const wchar_t* reason);

// Wipe the internal state — used by tests between cases so mutex state, the
// child list, and the fallback reason don't leak across cases.
void sao_launcher_dual_run_reset_for_test(void);

// ---------------------------------------------------------------------------
// Mutex — dual-run driver's single-instance guard.
// ---------------------------------------------------------------------------
// The launcher's per-exe mutex (``Global\SaoAuto.Instance.*``) doesn't cover
// the dual-run case: side-by-side must run TWO processes (one CPP, one
// Python) at the same time.  So we add a separate, thinner guard —
// ``Local\SaoAutoLauncherDualRun`` — that only forbids TWO dual-run drivers
// from running side by side.  A single dual-run driver is free to spawn
// unlimited children.
sao_status_t sao_launcher_dual_run_acquire_driver_mutex(HANDLE* mutex_out);
void         sao_launcher_dual_run_release_driver_mutex(HANDLE mutex);

enum sao_dual_run_handoff_state_e {
    SAO_DUAL_RUN_HANDOFF_NONE = 0,
    SAO_DUAL_RUN_HANDOFF_PENDING = 1,
    SAO_DUAL_RUN_HANDOFF_SUCCEEDED = 2,
    SAO_DUAL_RUN_HANDOFF_FAILED = 3,
};

typedef struct sao_dual_run_handoff_snapshot {
    DWORD pid;
    int64_t start_time_qpc;
    uint64_t generation;
    int32_t state;
    int32_t exit_code;
} sao_dual_run_handoff_snapshot;

sao_status_t sao_launcher_dual_run_take_handoff_result(
    sao_dual_run_handoff_snapshot* out);

// ---------------------------------------------------------------------------
// Init pipeline step-zero.
//
// Called BEFORE the existing 10 steps in init_pipeline.cpp.  Reads the
// config, decides the mode, spawns whatever needs spawning, and tells the
// caller (via ``continue_out``) whether to run the rest of the CPP pipeline
// or hand off to the child that was just started.
//
// Returns:
//   SAO_STATUS_OK + *continue_out=1  — current process is the CPP driver;
//                                       proceed with the 10-step pipeline.
//   SAO_STATUS_OK + *continue_out=0  — current process should exit now
//                                       (Python was handed off to).
//                                       ``exit_code_out`` is populated.
//   SAO_LAUNCHER_ALREADY_RUNNING     — driver mutex is held elsewhere.
//   SAO_LAUNCHER_PYTHON_UNAVAILABLE  — python_only requested but no Python
//                                       is installed.
//   SAO_LAUNCHER_SPAWN_FAILED        — CreateProcess call returned false.
// ---------------------------------------------------------------------------
sao_status_t sao_launcher_dual_run_step_zero(
    const sao_dual_run_config* cfg,
    int32_t* continue_out,
    int32_t* exit_code_out);
sao_status_t sao_launcher_dual_run_step_zero_v2(
    const sao_dual_run_config_v2* cfg,
    int32_t* continue_out,
    int32_t* exit_code_out);

// Hook the init_pipeline consults when it enters ``cpp_preferred_python_fallback``
// mode and any of the 10 CPP steps fails.  The pipeline calls this with the
// failing step's exit code, and the hook decides whether to spawn Python as
// a fallback.  Returns 1 if it did (and populated ``exit_code_out``), 0 if
// the caller should keep propagating the original failure.
int32_t sao_launcher_dual_run_maybe_fallback_to_python(
    const sao_dual_run_config* cfg,
    int cpp_step_exit_code,
    const wchar_t* failing_step_name,
    int32_t* exit_code_out);
int32_t sao_launcher_dual_run_maybe_fallback_to_python_v2(
    const sao_dual_run_config_v2* cfg,
    int cpp_step_exit_code,
    const wchar_t* failing_step_name,
    int32_t* exit_code_out);

// ---------------------------------------------------------------------------
// Test-only hook: when non-null, ``spawn_python`` and ``spawn_cpp`` return
// SAO_STATUS_OK with a fake pid/handle without touching CreateProcessW.
// Set by unit tests that don't want real subprocesses.
// ---------------------------------------------------------------------------
typedef int (*sao_dual_run_test_spawn_hook_t)(
    const wchar_t* exe_path,
    const wchar_t* command_line,
    const wchar_t* role,
    sao_dual_run_spawn_result* result_out);

void sao_launcher_dual_run_set_test_spawn_hook(
    sao_dual_run_test_spawn_hook_t hook);

// Test-only hook: when non-null, ``probe_python`` returns immediately with
// this pre-fabricated result.  Lets a test simulate "no python installed"
// even on a machine where Python is on PATH.
typedef void (*sao_dual_run_test_probe_hook_t)(
    sao_dual_run_python_probe* out);

void sao_launcher_dual_run_set_test_probe_hook(
    sao_dual_run_test_probe_hook_t hook);

// Test-only hook: when non-null, ``sao_launcher_dual_run_step_zero`` will
// call this AFTER the CPP path decides it's the driver but BEFORE returning
// to the caller.  Returning a non-zero value forces step_zero to report a
// cpp init failure and take the CPP_PREFERRED_PYTHON_FALLBACK branch.
typedef int (*sao_dual_run_test_force_cpp_fail_hook_t)(void);

void sao_launcher_dual_run_set_test_force_cpp_fail_hook(
    sao_dual_run_test_force_cpp_fail_hook_t hook);

// Override the parent argv snapshot consumed by spawn_cpp.  Passing argc=0
// clears the override and restores GetCommandLineW/CommandLineToArgvW.
sao_status_t sao_launcher_dual_run_set_test_parent_argv(
    int argc, const wchar_t* const* argv);

} // extern "C"
