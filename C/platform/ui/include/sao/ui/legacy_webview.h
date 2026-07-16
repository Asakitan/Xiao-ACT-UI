// Legacy WebView compatibility surface (Phase 9 — frozen).
//
// The C++ platform does NOT re-implement `sao_webview.py`.  This header
// only exposes three probe entrypoints so operators, tests, and the
// launcher can prove the surface is intentionally frozen and locate the
// manifest listing all Python-side APIs.
//
// See `docs/legacy_webview_manifest.md` for the authoritative snapshot
// of the 220-method Python bridge and 45 exposed pywebview.api.* calls.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/ui/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Availability probe.  Always returns false for the frozen surface.
// If a future release ever re-implements the native WebView bridge,
// flip this to true and update the manifest.
SAO_UI_API bool SAO_UI_CALL sao_ui_legacy_webview_available(void);

// Enumerate Python-side references to `sao_webview.py` (and related
// modules) that still live in the repo.  `python_side_path` is the
// absolute path to `sao_auto/python/` on the caller machine; the probe
// walks known files and returns a NUL-terminated report describing
// which callers still route through the frozen surface.
//
// `report_out` must be a caller-owned buffer.  `report_capacity` is the
// buffer size in bytes.  On success the writer NUL-terminates.  If the
// buffer is too small, SAO_ERR_BUFFER_TOO_SMALL is returned and
// `report_out` is left with a truncated best-effort string plus a
// terminating NUL when possible.
SAO_UI_API int32_t SAO_UI_CALL sao_ui_legacy_webview_probe(
    const char* python_side_path_utf8,
    char*       report_out,
    size_t      report_capacity);

// Return the absolute path to `docs/legacy_webview_manifest.md`
// (relative to the calling executable's own directory tree, using the
// standard `docs/` layout of this repo).  This lets a runtime report or
// operator dialog cite the manifest without hard-coding the path.
//
// `path_out` must be a caller-owned buffer.  On success the writer
// NUL-terminates.  If the buffer is too small, SAO_ERR_BUFFER_TOO_SMALL
// is returned and `path_out` is left untouched.
SAO_UI_API int32_t SAO_UI_CALL sao_ui_legacy_webview_manifest_path(
    char*  path_out,
    size_t path_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif
