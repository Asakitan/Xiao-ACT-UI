// SAO Auto -- runtime installer public C ABI.
//
// The launcher and each plugin host consult this API at startup to guarantee
// that the language / interpreter runtime it needs is present on disk before
// the host DLL is loaded.  When the runtime is missing the installer probes
// a signed manifest, downloads the payload over WinHTTP, verifies its hash
// against the manifest entry (SHA-256 mandatory, BLAKE3 optional), extracts
// it into %LOCALAPPDATA%/SaoAuto/runtimes/<opaque_id>/<version>/ and only
// then reports the runtime as usable.
//
// The API is intentionally minimal.  Higher-level orchestration (parallel
// downloads, UI progress, mirror fallback) can be composed on top by the
// launcher; the ABI itself only exposes single-shot verbs.
//
// Ordering guarantees:
//   * probe()   never touches the network -- disk + registry only.
//   * ensure()  is fail-closed: on any error the runtime tree is left in
//               the same state as before the call (staging directory is
//               removed on failure).
//   * verify()  re-checks integrity of the installed tree without any
//               network I/O.  Callers may run it every launch.
//   * uninstall() forcibly removes the runtime tree.  It never touches
//               unrelated tenants.
//
// Threading:
//   All exported functions may be called from any thread.  Concurrent
//   ensure() calls for the same runtime kind serialize internally on a
//   per-runtime named mutex (LOCAL\\SaoAuto.runtime_installer.<kind>).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// ABI export macros -- runtime_installer ships SHARED so the launcher and
// plugin hosts can dlopen it without pulling in core.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  if defined(SAO_RUNTIME_INSTALLER_BUILDING_DLL)
#    define SAO_RUNTIME_INSTALLER_API __declspec(dllexport)
#  elif defined(SAO_RUNTIME_INSTALLER_USING_DLL)
#    define SAO_RUNTIME_INSTALLER_API __declspec(dllimport)
#  else
#    define SAO_RUNTIME_INSTALLER_API
#  endif
#else
#  define SAO_RUNTIME_INSTALLER_API
#endif
#define SAO_RUNTIME_INSTALLER_CALL __cdecl

// ABI version.  Every new field on a public struct bumps the minor.  Layout
// changes bump the major.  Consumers must ignore trailing fields they do
// not recognise (struct_size guard below).
#define SAO_RUNTIME_INSTALLER_ABI_VERSION_MAJOR 1u
#define SAO_RUNTIME_INSTALLER_ABI_VERSION_MINOR 0u
#define SAO_RUNTIME_INSTALLER_ABI_VERSION \
    ((SAO_RUNTIME_INSTALLER_ABI_VERSION_MAJOR << 16) | \
     SAO_RUNTIME_INSTALLER_ABI_VERSION_MINOR)

SAO_RUNTIME_INSTALLER_API uint32_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_abi_version(void);

// ---------------------------------------------------------------------------
// Runtime kinds.
//
// Numeric values are ABI: never renumber, only append.  When new kinds land
// they must also grow the manifest schema and the internal descriptor table
// in src/runtime_installer.cpp.
// ---------------------------------------------------------------------------
typedef int32_t sao_runtime_kind_t;
enum sao_runtime_kind_e : int32_t {
    SAO_RUNTIME_KIND_INVALID          = 0,
    SAO_RUNTIME_KIND_PYTHON3_EMBED    = 1,   // CPython 3.11 embeddable zip
    SAO_RUNTIME_KIND_DOTNET_RUNTIME   = 2,   // .NET 8 runtime win-x64
    SAO_RUNTIME_KIND_LUA54_LIB        = 3,   // Lua 5.4 shared library
    SAO_RUNTIME_KIND_ANGELSCRIPT_LIB  = 4,   // AngelScript shared library

    // Keep last -- iterators use this as the exclusive upper bound.
    SAO_RUNTIME_KIND_COUNT_
};

// ---------------------------------------------------------------------------
// Progress callback.  Invoked from the download thread; must not block
// indefinitely and must not call back into the installer API.  A total of
// zero means the payload length is unknown (WinHTTP chunked encoding).
// ---------------------------------------------------------------------------
typedef void (SAO_RUNTIME_INSTALLER_CALL* sao_runtime_installer_progress_cb_t)(
    uint64_t bytes_transferred,
    uint64_t bytes_total,
    void*    user_data);

// ---------------------------------------------------------------------------
// Manifest handle (opaque).  Owned by the installer; caller destroys via
// sao_runtime_installer_manifest_release().
// ---------------------------------------------------------------------------
typedef struct sao_runtime_manifest_s* sao_runtime_manifest_handle_t;

// ---------------------------------------------------------------------------
// Verbs
// ---------------------------------------------------------------------------

// Probe: check whether a runtime is present on disk and report the absolute
// UTF-8 path to its root (%LOCALAPPDATA%\SaoAuto\runtimes\<opaque>\<ver>\).
//
// path_capacity is measured in bytes and includes the trailing NUL.  When
// the buffer is too small out_required receives the exact byte count that
// would satisfy the request; the caller can allocate and retry.  Passing
// out_path_utf8=nullptr with path_capacity=0 is a sizing query.
//
// out_present must never be NULL -- it is written on every success, even
// when the buffer is too small.
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_probe(
    sao_runtime_kind_t kind,
    bool*   out_present,
    char*   out_path_utf8,
    size_t  path_capacity,
    size_t* out_required);

// Ensure: probe first; if missing, download the payload described by the
// currently loaded manifest, verify integrity, extract into the target
// directory, and only then flip the on-disk marker to indicate the runtime
// is usable.  Progress callback is optional.
//
// Fail-closed: on any error the staging directory is removed and no
// partial installation is left behind.  When the runtime was already
// present the function returns SAO_STATUS_OK without touching the disk.
//
// Consumers that do not want to block on the network path (e.g. background
// tasks) should probe first and only ensure() when they have UI to show
// progress.
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_ensure(
    sao_runtime_kind_t                    kind,
    sao_runtime_installer_progress_cb_t   progress_cb,
    void*                                 user_data);

// Verify: recompute the integrity hash of the installed tree (or, for
// zip-archive kinds, the extracted marker) and compare against the manifest.
// No network I/O.  Returns SAO_STATUS_ERR_NOT_FOUND when the runtime is
// absent; SAO_STATUS_ERR_UNKNOWN when it is present but tampered.
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_verify_integrity(sao_runtime_kind_t kind);

// Uninstall: remove the runtime tree.  Returns SAO_STATUS_OK even when the
// runtime is absent (idempotent).
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_uninstall(sao_runtime_kind_t kind);

// ---------------------------------------------------------------------------
// Aggregate ensure surface.  Invoked once per launch by the launcher's
// init pipeline; walks every runtime kind referenced by the currently bound
// manifest and calls ensure() on each.  The progress callback carries the
// runtime's opaque directory id so the compositor progress overlay can show
// which runtime is being fetched.
//
// The base_dir parameter is accepted for future use (redirect installs to
// a portable base) but is currently ignored -- installs always land under
// %LOCALAPPDATA%\SaoAuto\runtimes.  A NULL base_dir is legal and means
// "use the default".
// ---------------------------------------------------------------------------
typedef void (SAO_RUNTIME_INSTALLER_CALL* sao_runtime_installer_progress_cb_ex_t)(
    const char* kind_opaque_id_utf8,
    uint64_t    bytes_done,
    uint64_t    bytes_total,
    void*       user_data);

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_ensure_all(
    const wchar_t* base_dir,
    sao_runtime_installer_progress_cb_ex_t progress_cb,
    void* user_data);

// ---------------------------------------------------------------------------
// Manifest management
//
// A manifest describes URLs, hashes, versions and per-kind extraction hints.
// It is a compact JSON object; the schema lives in manifest.h.  The blob
// itself is expected to be signed and delivered through the license
// channel; the installer never fetches manifests over the network -- it
// receives them from the launcher.
// ---------------------------------------------------------------------------
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_load_manifest(
    const char*                       manifest_json_utf8,
    size_t                            manifest_json_length,
    sao_runtime_manifest_handle_t*    out_handle);

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_release(sao_runtime_manifest_handle_t handle);

// Bind a loaded manifest to the process-wide installer.  Ownership stays
// with the caller; the installer keeps a strong reference until unbound
// or another manifest is bound.  Passing NULL clears the binding, which
// makes ensure() fail with SAO_STATUS_ERR_NOT_INITIALIZED.
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_bind_manifest(sao_runtime_manifest_handle_t handle);

// Look up whether a manifest declares a given runtime kind.  Useful for
// launcher UI to grey-out unavailable runtimes without attempting a probe.
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_has_kind(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    bool*                         out_present);

// ---------------------------------------------------------------------------
// Introspection helpers -- read-only.  These are exposed so the launcher
// can render diagnostics without duplicating the descriptor table.
// ---------------------------------------------------------------------------

// Copy the opaque directory id for a runtime kind into the caller buffer.
// The id is a short lowercase hex tag (12 chars) derived from the compiled
// SAO_ENC_STR'd internal name; the plaintext product name never appears on
// disk, matching the "no plaintext product names in downloaded runtime dir
// names" constraint.
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_kind_opaque_id(
    sao_runtime_kind_t kind,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required);

// Return the currently bound manifest version string (semver-ish).  Copies
// into the caller buffer identically to probe().
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_version(
    sao_runtime_manifest_handle_t handle,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required);

#ifdef __cplusplus
}  // extern "C"
#endif
