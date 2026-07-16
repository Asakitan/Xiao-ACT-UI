// SAO Auto — generic state bridge.
//
// Corresponds to Python `mem_state_bridge.py` / `unified_data_source.py`:
// a game-agnostic key/value snapshot with source metadata (TCP vs MEM
// vs FALLBACK) so consumers can display "why did this number appear?".
//
// State is scalar-only.  Complex plugin-owned aggregates (any of the
// game-specific rollups that live in `plugins/<game>_plugin/`) are
// held by the plugin itself and communicate outwards via the event
// bus, not through this handle.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sao_engine_state_source_e : int32_t {
    SAO_ENGINE_SOURCE_UNKNOWN  = 0,
    SAO_ENGINE_SOURCE_TCP      = 1,
    SAO_ENGINE_SOURCE_MEMORY   = 2,
    SAO_ENGINE_SOURCE_ESTIMATE = 3,
    SAO_ENGINE_SOURCE_FALLBACK = 4,
};

enum sao_engine_state_type_e : int32_t {
    SAO_ENGINE_STATE_NULL    = 0,
    SAO_ENGINE_STATE_BOOL    = 1,
    SAO_ENGINE_STATE_INT64   = 2,
    SAO_ENGINE_STATE_DOUBLE  = 3,
    SAO_ENGINE_STATE_STRING  = 4,
};

struct SaoEngineStateValue {
    int32_t  value_type;      // sao_engine_state_type_e
    int32_t  source;          // sao_engine_state_source_e
    uint64_t timestamp_ns;    // set/refresh time from state clock
    int64_t  as_int64;        // valid when value_type == INT64
    double   as_double;       // valid when value_type == DOUBLE
    // string is stored at *out_string in a separate buffer supplied by
    // the caller of the get() function; string_len is the length
    // *including* null terminator.
    uint32_t string_len;
    uint32_t _pad;
};

typedef struct sao_engine_state_s* sao_engine_state_handle_t;
typedef uint64_t sao_engine_state_subscription_t;

struct SaoEngineStateSnapshotEntry {
    uint32_t key_offset;
    uint32_t key_length;
    uint32_t string_offset;
    uint32_t string_length;
    SaoEngineStateValue value;
};

typedef void (SAO_ENGINE_CALL* sao_engine_state_change_callback_t)(
    const char* key_utf8,
    const SaoEngineStateValue* value,
    const char* string_value_utf8,
    uint32_t erased,
    void* user_data);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_create(
    sao_engine_state_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_state_destroy(
    sao_engine_state_handle_t handle);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_set_int64(
    sao_engine_state_handle_t handle,
    const char* key_utf8,
    int64_t value,
    int32_t source);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_set_double(
    sao_engine_state_handle_t handle,
    const char* key_utf8,
    double value,
    int32_t source);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_set_string(
    sao_engine_state_handle_t handle,
    const char* key_utf8,
    const char* value_utf8,
    int32_t source);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_get(
    sao_engine_state_handle_t handle,
    const char* key_utf8,
    SaoEngineStateValue* out_value,
    char* out_string_utf8,      // may be null if value_type != STRING
    size_t string_capacity);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_erase(
    sao_engine_state_handle_t handle, const char* key_utf8);

// Snapshot all entries in lexical key order. Pass out_entries == null to
// query the required entry and buffer sizes. Keys and string values refer
// into out_buffer by offset and length; lengths exclude null terminators.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_snapshot(
    sao_engine_state_handle_t handle,
    SaoEngineStateSnapshotEntry* out_entries,
    size_t entries_capacity,
    char* out_buffer,
    size_t buffer_capacity,
    uint32_t* out_entry_count,
    size_t* out_buffer_used);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_subscribe(
    sao_engine_state_handle_t handle,
    sao_engine_state_change_callback_t callback,
    void* user_data,
    sao_engine_state_subscription_t* out_subscription);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_unsubscribe(
    sao_engine_state_handle_t handle,
    sao_engine_state_subscription_t subscription);

// ---------------------------------------------------------------------------
// Wave 6 / Phase 5 — generic finite state machine.
//
// Ports the platform-generic ``StateMachine`` primitive. Config is a JSON
// blob supplied by the caller so the engine layer stays completely
// game-agnostic — no boss/encounter/DPS names live in this header.
//
// Config JSON schema (validated by state_machine.cpp):
//   {
//     "initial": "<name>",
//     "states":  ["<name>", ...],
//     "transitions": [
//       {"from": "<name>", "to": "<name>", "on": ["<event>", ...]},
//       ...
//     ]
//   }
//
// Event types are arbitrary UTF-8 tokens supplied by the caller — the
// state machine only matches them against the ``on`` list.  A single
// dispatch may trigger at most one transition (the first matching edge
// wins under insertion order).
//
// SAO_STATUS_ERR_INVALID_TRANSITION indicates the event was well-formed
// but did not match any outgoing edge from the current state.  Callers
// treat this as a soft failure — the engine returns the *unchanged*
// current state via the out param when the caller supplied one.
// ---------------------------------------------------------------------------

#define SAO_STATUS_ERR_INVALID_TRANSITION ((sao_status_t)-82)

typedef struct sao_engine_state_machine_s* sao_engine_state_machine_handle_t;

// One recorded transition.  ``timestamp_ns`` is a monotonic value
// supplied by the dispatcher (0 = "unspecified", allowed for tests).
struct SaoEngineStateTransition {
    // Names refer into the accompanying ``out_buffer`` — offset + length,
    // like the event bus ring API.
    uint32_t from_offset;
    uint32_t from_length;
    uint32_t to_offset;
    uint32_t to_length;
    uint32_t event_offset;
    uint32_t event_length;
    uint64_t timestamp_ns;
};

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_create(
    const char* config_json_utf8,
    sao_engine_state_machine_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_state_machine_destroy(
    sao_engine_state_machine_handle_t handle);

// Feed one event.  ``event_utf8`` is opaque to the engine; only the
// config-registered ``on`` names are meaningful.  ``timestamp_ns`` is
// stored verbatim into the history ring for provenance.  On success
// writes the new state name (nul-terminated) into ``out_state_utf8`` up
// to ``state_capacity`` bytes; SAO_STATUS_ERR_BUFFER_TOO_SMALL when the
// buffer cannot hold the name.
//
// SAO_STATUS_ERR_INVALID_TRANSITION -> event did not match any outgoing
// edge; the state is left unchanged, but the *current* state is still
// written into ``out_state_utf8`` when supplied.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_dispatch(
    sao_engine_state_machine_handle_t handle,
    const char* event_utf8,
    uint64_t timestamp_ns,
    char* out_state_utf8,
    size_t state_capacity);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_get_current(
    sao_engine_state_machine_handle_t handle,
    char* out_state_utf8,
    size_t state_capacity);

// Read the last N transitions (oldest first when ``max_entries`` >= total).
// Pass out_entries == null with any capacity to just get the count.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_get_history(
    sao_engine_state_machine_handle_t handle,
    uint32_t max_entries,
    SaoEngineStateTransition* out_entries,
    size_t entries_capacity,
    char* out_buffer,
    size_t buffer_capacity,
    uint32_t* out_entry_count,
    size_t* out_buffer_used);

// Reset to the initial state and clear the transition history.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_state_machine_reset(
    sao_engine_state_machine_handle_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
