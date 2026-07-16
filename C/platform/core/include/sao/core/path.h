// SAO Auto — filesystem path helpers.
//
// The platform mirrors the Python `config.BASE_DIR` concept: everything
// persistent (settings, cached class index, plugin manifests) is stored
// relative to a single, deterministically-resolved base directory.
//
// The rules from Python survive here:
//   * When running as an unpacked binary, BASE_DIR = <exe_dir> if that
//     directory contains a `plugins/` sub-tree; otherwise the parent.
//   * When running from `bin/`, BASE_DIR = <exe_dir>/.. .
//   * A launcher may override the base dir via env `SAO_BASE_DIR`.
//   * Plugin resource paths are always resolved *relative to the plugin
//     manifest*, never relative to BASE_DIR.  This matches the Nuitka
//     packaging memory note (project_packaging_4_0_0_onedir).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolve BASE_DIR once per process.  Subsequent calls return the cached
// value.  UTF-8 result, no trailing slash.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_path_base_dir(
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed);

// Path join with automatic separator normalisation.  segments are
// concatenated with `\` on Windows; leading segments may be absolute.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_path_join(
    const char* const* segments_utf8,
    size_t segment_count,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed);

SAO_CORE_API bool SAO_CORE_CALL sao_core_path_exists(const char* path_utf8);
SAO_CORE_API bool SAO_CORE_CALL sao_core_path_is_directory(const char* path_utf8);
SAO_CORE_API bool SAO_CORE_CALL sao_core_path_is_file(const char* path_utf8);

// Create a directory (mkdir -p semantics — parents auto-created).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_path_make_dirs(
    const char* path_utf8);

// Canonicalise: resolve `.`, `..`, symlinks.  Result written to out_utf8.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_path_canonicalize(
    const char* path_utf8,
    char* out_utf8,
    size_t capacity,
    size_t* out_bytes_needed);

#ifdef __cplusplus
}  // extern "C"
#endif
