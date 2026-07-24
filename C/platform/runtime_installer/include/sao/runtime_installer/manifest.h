// SAO Auto -- runtime installer manifest schema (public).
//
// The manifest is JSON delivered as one blob per launcher session.  It
// describes the download endpoints, integrity hashes and extraction hints
// for every runtime kind the launcher may need.  The schema below is the
// wire-level contract; the installer never fetches this blob itself -- it
// only parses whatever the launcher hands to load_manifest().
//
// Wire format (JSON, UTF-8):
//
//   {
//     "schema": 1,
//     "version": "2026.07.24",
//     "entries": [
//       {
//         "kind":         "python3_embed",
//         "version":      "3.11.8",
//         "url":          "https://cdn.example/py-3.11.8-embed-amd64.zip",
//         "url_mirrors":  [ "https://mirror1/..." ],
//         "sha256_hex":   "e0d9...",
//         "blake3_hex":   "8f47...",              // optional
//         "size_bytes":   10485760,
//         "archive_type": "zip",                  // "zip" | "raw" | "nupkg"
//         "install_hint": "extract_top_level"     // see kInstallHint*
//       },
//       ...
//     ]
//   }
//
// Every field named above is required except "url_mirrors" and
// "blake3_hex".  Unknown fields are ignored (forward compatibility).
//
// The parsed schema is exposed as opaque handles; this header only declares
// enums and length limits that the C ABI header (runtime_installer.h)
// references.  Everything else stays private to src/manifest.cpp.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/runtime_installer/runtime_installer.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Schema versions.  Manifests declaring a higher schema value than the
// installer supports are rejected with SAO_STATUS_ERR_ABI_MISMATCH.
// ---------------------------------------------------------------------------
#define SAO_RUNTIME_INSTALLER_MANIFEST_SCHEMA_MIN 1u
#define SAO_RUNTIME_INSTALLER_MANIFEST_SCHEMA_MAX 1u

// ---------------------------------------------------------------------------
// Archive types.
// ---------------------------------------------------------------------------
typedef int32_t sao_runtime_archive_t;
enum sao_runtime_archive_e : int32_t {
    SAO_RUNTIME_ARCHIVE_INVALID = 0,
    SAO_RUNTIME_ARCHIVE_ZIP     = 1,   // plain ZIP (python-embed, lua bundle)
    SAO_RUNTIME_ARCHIVE_RAW     = 2,   // single blob written as-is (e.g. lua54.dll)
    SAO_RUNTIME_ARCHIVE_NUPKG   = 3,   // NuGet package (.NET runtime)
};

// ---------------------------------------------------------------------------
// Install hints.  The installer post-processes an extracted archive with
// one of these strategies to normalise the final on-disk layout.
// ---------------------------------------------------------------------------
typedef int32_t sao_runtime_install_hint_t;
enum sao_runtime_install_hint_e : int32_t {
    SAO_RUNTIME_INSTALL_HINT_NONE               = 0,
    SAO_RUNTIME_INSTALL_HINT_EXTRACT_TOP_LEVEL  = 1,   // python-embed: flat
    SAO_RUNTIME_INSTALL_HINT_NUPKG_RUNTIME_TREE = 2,   // .NET: runtimes/win-x64
    SAO_RUNTIME_INSTALL_HINT_SINGLE_BLOB        = 3,   // just move the file
};

// ---------------------------------------------------------------------------
// Length caps.  The manifest parser enforces these; oversized fields are
// rejected with SAO_STATUS_ERR_INVALID_ARGUMENT.
// ---------------------------------------------------------------------------
#define SAO_RUNTIME_MANIFEST_MAX_JSON_BYTES        (1u << 20)   // 1 MiB
#define SAO_RUNTIME_MANIFEST_MAX_ENTRIES           64u
#define SAO_RUNTIME_MANIFEST_MAX_URL_BYTES         2048u
#define SAO_RUNTIME_MANIFEST_MAX_MIRROR_COUNT      8u
#define SAO_RUNTIME_MANIFEST_MAX_VERSION_BYTES     64u
#define SAO_RUNTIME_MANIFEST_MAX_HEX_BYTES         129u          // 128 hex chars + NUL
#define SAO_RUNTIME_MANIFEST_MAX_PAYLOAD_BYTES     (uint64_t)(1ull << 30)   // 1 GiB

// ---------------------------------------------------------------------------
// Query helpers.  These read individual fields off a loaded manifest.  All
// returns follow the caller-allocates buffer convention.
// ---------------------------------------------------------------------------
SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_url(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required);

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_version(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required);

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_sha256_hex(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    char*   out_utf8,
    size_t  capacity,
    size_t* out_required);

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_archive(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    sao_runtime_archive_t*        out_archive);

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_install_hint(
    sao_runtime_manifest_handle_t   handle,
    sao_runtime_kind_t              kind,
    sao_runtime_install_hint_t*     out_hint);

SAO_RUNTIME_INSTALLER_API sao_status_t SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_manifest_entry_size(
    sao_runtime_manifest_handle_t handle,
    sao_runtime_kind_t            kind,
    uint64_t*                     out_bytes);

// ---------------------------------------------------------------------------
// Test hooks.  Compiled in only when SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS
// is defined.  The mocked-network unit tests inject a synthetic transport
// so they can exercise the download+verify pipeline offline without
// touching WinHTTP.
// ---------------------------------------------------------------------------
#if defined(SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)
typedef sao_status_t (SAO_RUNTIME_INSTALLER_CALL* sao_runtime_installer_test_transport_t)(
    const char*     url_utf8,
    const uint8_t** out_body,
    size_t*         out_body_bytes,
    void*           user_data);

SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_transport(
    sao_runtime_installer_test_transport_t transport,
    void*                                  user_data);

// Redirect the on-disk root away from %LOCALAPPDATA%.  Passing NULL
// restores the default.
SAO_RUNTIME_INSTALLER_API void SAO_RUNTIME_INSTALLER_CALL
sao_runtime_installer_test_set_root(const char* utf8_path_or_null);
#endif  // SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS

#ifdef __cplusplus
}  // extern "C"
#endif
