// SAO Auto — generic trigger engine.
//
// A game-agnostic condition/action dispatcher.
//
// The engine only knows six built-in condition families, and every one
// of them is expressed via opaque JSON parameters that the trigger
// engine itself does not interpret beyond the schemas documented
// below.  Any actual "boss buff appeared" / "DPS threshold crossed"
// semantics live entirely inside the plugin that registered the
// trigger — the plugin uses the built-in families as generic pattern
// matchers.
//
// Built-in condition types (SaoEngineTriggerConditionType):
//
//   event_match       — Fire when a published event matches a topic
//                       name and (optionally) contains substrings in its
//                       payload.  Params:
//                         { "event_type": "<utf-8>",           // required
//                           "payload_contains": ["<utf-8>", ...] // optional
//                         }
//
//   state_enter       — Fire when the state machine last dispatched a
//                       transition entering ``state``.  Params:
//                         { "state": "<utf-8>" }
//
//   state_leave       — Symmetric to ``state_enter``: matches events
//                       whose ``from`` == params.state.
//
//   history_pattern   — Fire when a comma-separated sequence of event
//                       types has been seen inside a rolling window.
//                       Params:
//                         { "sequence": ["<utf-8>", ...],  // ordered
//                           "window_ms": 3000 }
//
//   timer             — Periodic tick.  Params:
//                         { "interval_ms": 1000, "one_shot": false }
//
//   combo             — All-of composition of other conditions declared
//                       inline.  Params:
//                         { "conditions": [ <spec>, <spec>, ... ] }
//                       Each nested ``spec`` mirrors ``SaoEngineTriggerSpec``
//                       and is stored recursively.
//
// The trigger engine returns matched ``action_id`` tokens as raw
// uint64_t handles supplied at register time — the plugin owns the
// mapping to actual UI / TTS / plugin function callbacks.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_engine_trigger_engine_s* sao_engine_trigger_engine_handle_t;

enum sao_engine_trigger_condition_type_e : int32_t {
    SAO_ENGINE_TRIGGER_EVENT_MATCH     = 1,
    SAO_ENGINE_TRIGGER_STATE_ENTER     = 2,
    SAO_ENGINE_TRIGGER_STATE_LEAVE     = 3,
    SAO_ENGINE_TRIGGER_HISTORY_PATTERN = 4,
    SAO_ENGINE_TRIGGER_TIMER           = 5,
    SAO_ENGINE_TRIGGER_COMBO           = 6,
};

struct SaoEngineTriggerSpec {
    int32_t     condition_type;         // sao_engine_trigger_condition_type_e
    const char* condition_params_json;  // UTF-8; engine treats as opaque
    uint64_t    action_id;              // opaque token returned on match
};

struct SaoEngineTriggerEvent {
    const char* event_type_utf8;
    const char* payload_utf8;   // may be null when empty
    uint64_t    timestamp_ms;
    // Optional state machine transition context — set to null when the
    // event was not produced by a state machine dispatch.
    const char* state_from_utf8;
    const char* state_to_utf8;
};

typedef uint64_t sao_engine_trigger_handle_t;

// Optional callback signature.  When a callback is registered alongside
// the spec, the engine invokes it in addition to reporting the
// ``action_id`` through the evaluate/tick out parameters.  ``user_data``
// is exactly the value passed at register time.
typedef void (SAO_ENGINE_CALL* sao_engine_trigger_callback_t)(
    uint64_t action_id,
    const struct SaoEngineTriggerEvent* event,
    void* user_data);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_trigger_create(
    sao_engine_trigger_engine_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_trigger_destroy(
    sao_engine_trigger_engine_handle_t handle);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_trigger_register(
    sao_engine_trigger_engine_handle_t handle,
    const struct SaoEngineTriggerSpec* spec,
    sao_engine_trigger_callback_t callback,   // may be null
    void* user_data,
    sao_engine_trigger_handle_t* out_trigger);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_trigger_unregister(
    sao_engine_trigger_engine_handle_t handle,
    sao_engine_trigger_handle_t trigger);

// Evaluate one event.  ``out_action_ids`` receives every matched
// action_id in registration order.  ``capacity`` is the caller-owned
// slot count; on SAO_STATUS_ERR_BUFFER_TOO_SMALL the count still
// reports what would have been written.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_trigger_evaluate(
    sao_engine_trigger_engine_handle_t handle,
    const struct SaoEngineTriggerEvent* event,
    uint64_t* out_action_ids,
    uint32_t capacity,
    uint32_t* out_count);

// Advance timer triggers by ``dt_ms`` milliseconds.  Every timer that
// crosses its interval boundary fires — the returned action_ids are the
// timers that matched during this tick.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_trigger_tick(
    sao_engine_trigger_engine_handle_t handle,
    uint64_t dt_ms,
    uint64_t* out_action_ids,
    uint32_t capacity,
    uint32_t* out_count);

#ifdef __cplusplus
}  // extern "C"
#endif
