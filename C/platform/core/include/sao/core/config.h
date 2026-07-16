// SAO Auto — configuration read/write facade.
//
// Modelled after Python's `sao_auto/python/config.py`: a flat key/value
// store with typed getters.  The concrete backing store is JSON on disk
// (chosen by the launcher), but the ABI never leaks JSON to callers.
//
// Keys are dotted lowercase paths (e.g. "overlay.topmost.mode",
// "act.aggregate.top_n").  Values are typed at read time — mismatch
// returns SAO_STATUS_ERR_INVALID_ARGUMENT rather than doing silent
// coercion.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_core_config_s* sao_core_config_handle_t;

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_open(
    const char* backing_path_utf8, sao_core_config_handle_t* out_handle);

SAO_CORE_API void SAO_CORE_CALL sao_core_config_close(
    sao_core_config_handle_t handle);

// Persist any in-memory dirty state.  Safe to call on a clean handle.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_flush(
    sao_core_config_handle_t handle);

// Typed getters — return SAO_STATUS_ERR_NOT_FOUND if key absent,
// SAO_STATUS_ERR_INVALID_ARGUMENT if key present with a wrong type.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_get_bool(
    sao_core_config_handle_t handle, const char* key_utf8, bool* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_get_int(
    sao_core_config_handle_t handle, const char* key_utf8, int64_t* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_get_double(
    sao_core_config_handle_t handle, const char* key_utf8, double* out_value);

// String getter — writes up to buffer_len bytes (UTF-8, null-terminated).
// If buffer_len is too small, returns SAO_STATUS_ERR_BUFFER_TOO_SMALL and
// *out_bytes_needed contains the required size *including* the null.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_get_string(
    sao_core_config_handle_t handle,
    const char* key_utf8,
    char* out_buffer,
    size_t buffer_len,
    size_t* out_bytes_needed);

// Setters — creates key if missing.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_set_bool(
    sao_core_config_handle_t handle, const char* key_utf8, bool value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_set_int(
    sao_core_config_handle_t handle, const char* key_utf8, int64_t value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_set_double(
    sao_core_config_handle_t handle, const char* key_utf8, double value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_set_string(
    sao_core_config_handle_t handle, const char* key_utf8, const char* value_utf8);

// Remove a key (idempotent — missing key is not an error).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_config_erase(
    sao_core_config_handle_t handle, const char* key_utf8);

// ---------------------------------------------------------------------------
// Wave 5 / Phase 1 — Settings envelope API.
//
// A pure in-memory dict-of-variant that serialises to/from a JSON file on
// disk.  Mirrors Python's ``json.dumps(indent=2, sort_keys=True)`` output
// exactly, so C-written and Python-written settings.json files interop.
//
// Ownership: sao_core_settings_load / _create hand back an owning handle;
// _save reads from it; _free tears it down.  Handles are NOT thread-safe —
// callers must serialise access externally.
//
// The old ``sao_core_config_*`` API above is a lower-level facade over the
// same settings store and persistence implementation.
// ---------------------------------------------------------------------------
typedef struct sao_core_settings_s sao_core_settings_t;

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_create(
    sao_core_settings_t** out_settings);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_load(
    const char* path_utf8, sao_core_settings_t** out_settings);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_save(
    const char* path_utf8, const sao_core_settings_t* settings);

SAO_CORE_API void SAO_CORE_CALL sao_core_settings_free(
    sao_core_settings_t* settings);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_get_int(
    const sao_core_settings_t* settings, const char* key_utf8,
    int64_t default_value, int64_t* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_get_bool(
    const sao_core_settings_t* settings, const char* key_utf8,
    bool default_value, bool* out_value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_get_float(
    const sao_core_settings_t* settings, const char* key_utf8,
    double default_value, double* out_value);

// Copies the string value into ``out_buffer`` (UTF-8, null-terminated).
// If the buffer is too small, ``out_size_needed`` receives the required
// length *including* the terminator and SAO_STATUS_ERR_BUFFER_TOO_SMALL
// is returned.  Missing keys return the default via the same path.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_get_string(
    const sao_core_settings_t* settings, const char* key_utf8,
    const char* default_value_utf8,
    char* out_buffer, size_t buffer_len, size_t* out_size_needed);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_set_int(
    sao_core_settings_t* settings, const char* key_utf8, int64_t value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_set_bool(
    sao_core_settings_t* settings, const char* key_utf8, bool value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_set_float(
    sao_core_settings_t* settings, const char* key_utf8, double value);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_set_string(
    sao_core_settings_t* settings, const char* key_utf8,
    const char* value_utf8);

// Introspection — returns how many keys are in the envelope.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_key_count(
    const sao_core_settings_t* settings, size_t* out_count);

// ---------------------------------------------------------------------------
// Wave 9 / Agent e — settings fixture parity helpers.
//
// The docs/fixtures/settings/*.json freeze pins the output of Python's
// ``json.dumps(data, ensure_ascii=False)`` (compact) and
// ``json.dumps(data, ensure_ascii=False, indent=2)`` (pretty) on the
// settings dict as it lives in ``config.py`` on disk.  The old settings
// envelope API above only handles flat primitive dicts, but the fixtures
// carry nested objects/arrays that need Python-parity byte output.
//
// The helpers below take a UTF-8 JSON blob (as the fixtures pin their
// ``input`` field) and produce the four canonical projections the
// fixtures require:
//   1. plaintext_utf8              -- compact dumps(ensure_ascii=False)
//   2. pretty_utf8                 -- indent=2 dumps(ensure_ascii=False)
//   3. normalized_panel_themes     -- normalize_panel_themes(input.panel_themes)
//   4. merged_hotkeys              -- DEFAULT_HOTKEYS overlaid with input.hotkeys
//   5. after_legacy_strip_plaintext_utf8 -- compact dump after
//                                         _LEGACY_KEYS have been popped
//
// All outputs are UTF-8, no BOM, no trailing newline; the writer preserves
// the input dict's insertion order (Python 3.7+ dict semantics) so a hop
// from Python to C stays byte-identical.
// ---------------------------------------------------------------------------

// Compact serialise: matches json.dumps(data, ensure_ascii=False).
// input_json_utf8/input_size is the source blob (must parse as an object).
// out_buf/out_capacity receive the serialised form; if buf is null or too
// small, out_size_needed is populated and SAO_STATUS_ERR_BUFFER_TOO_SMALL
// is returned (no null terminator).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_dump_compact(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed);

// Pretty serialise: matches json.dumps(data, ensure_ascii=False, indent=2).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_dump_pretty(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed);

// Normalise panel_themes: returns compact-dumped
// normalize_panel_themes(input.get("panel_themes", DEFAULT_SETTINGS["panel_themes"])).
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_normalize_panel_themes(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed);

// Merge hotkeys: returns compact-dumped
// {**DEFAULT_HOTKEYS, **(input.get("hotkeys") or {})}.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_merge_hotkeys(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed);

// Strip legacy keys and dump compact:
// json.dumps({k:v for k,v in input.items() if k not in _LEGACY_KEYS},
//            ensure_ascii=False)
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_settings_strip_legacy_dump(
    const uint8_t* input_json_utf8,
    size_t         input_size,
    uint8_t*       out_buf,
    size_t         out_capacity,
    size_t*        out_size_needed);

#ifdef __cplusplus
}  // extern "C"
#endif
