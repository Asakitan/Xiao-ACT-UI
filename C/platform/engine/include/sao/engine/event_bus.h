// SAO Auto — declarative event bus.
//
// Game-agnostic port of the platform event bus (`act_platform/event_bus.py`
// in the Python source tree).
//
// A topic-scoped publish/subscribe bus with:
//   * synchronous delivery (deterministic replay tests)
//   * callback isolation (a failing callback never breaks another)
//   * ring-buffered recent history for late subscribers
//   * "ephemeral" topics that skip the retention ring — useful for bulk
//     snapshot topics whose payloads would otherwise dominate the ring
//
// Payloads cross the ABI as UTF-8 JSON bytes.  The bus does not parse
// them — it just fans them out.  Consumers who need typed access do
// their own JSON parse.
//
// The bus knows nothing about DPS, boss state, encounters, mechanics
// or any other game concept — those live inside plugins and cross
// this bus as opaque JSON payloads on plugin-owned topic names.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_engine_event_bus_s* sao_engine_event_bus_handle_t;
typedef uint64_t sao_engine_subscription_t;

// Envelope of one published event.  See Python `events.py::make_event`.
//
// producer_id_hash / source_id_hash are opaque FNV-1a tags supplied by
// the publisher; the bus never inspects them and they carry no built-in
// semantics.  Consumers who care may use them for provenance display.
struct SaoEngineEventHeader {
    uint64_t sequence_id;      // monotonically increasing per bus
    uint64_t ts_unix_ns;
    uint64_t producer_id_hash; // FNV-1a, publisher-defined identity
    uint64_t source_id_hash;   // FNV-1a, publisher-defined data source
    float    confidence;
    uint32_t _pad;
};

// Callback signature — topic UTF-8, JSON UTF-8, header struct.
typedef void (SAO_ENGINE_CALL* sao_engine_event_callback_t)(
    const char* topic_utf8,
    const uint8_t* json_payload_utf8,
    size_t payload_len,
    const SaoEngineEventHeader* header,
    void* user_data);

// ---------------------------------------------------------------------------
// Wave 17c note — base ABI classification.
//
// PLAN §1.5 lists the base subscribe/publish/recent/stats/mark_ephemeral
// entry points as "legacy stubs".  On audit those symbols turned out to be
// live: the shipping implementation lives in ``src/event_bus_adapter.cpp``
// and is exercised by ``sao::runtime`` at startup.  The historical
// ``src/event_bus.cpp`` translation unit has seven NOT_IMPLEMENTED return
// statements in its dead base-ABI section and is never compiled into the
// engine DLL.  That source count is audit inventory, not a runtime
// completeness measure.  We therefore do NOT mark these declarations
// ``[[deprecated]]`` — doing so would spam warnings on live consumers.
// New code can bind against the ``_wave5`` API when it needs the priority
// and cancellation semantics layered on top of the same production bus.
// ---------------------------------------------------------------------------

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create(
    uint32_t max_recent,           // ring buffer size; 0 = default 200
    float slow_callback_ms,        // threshold for the slow-callback counter
    sao_engine_event_bus_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_event_bus_destroy(
    sao_engine_event_bus_handle_t handle);

// Mark a topic ephemeral — delivered but not retained.  Idempotent.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_mark_ephemeral(
    sao_engine_event_bus_handle_t handle, const char* topic_utf8);

// Subscribe.  "*" matches any topic.  owner_id_utf8 is an opaque tag
// used by the plugin manager to sweep all subs owned by a plugin at
// unload time.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_subscribe(
    sao_engine_event_bus_handle_t handle,
    const char* topic_utf8,
    const char* owner_id_utf8,
    sao_engine_event_callback_t callback,
    void* user_data,
    sao_engine_subscription_t* out_subscription);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_unsubscribe(
    sao_engine_event_bus_handle_t handle,
    sao_engine_subscription_t subscription);

// Sweep every subscription with matching owner_id_utf8.  Returns the
// count that was removed.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_unsubscribe_owner(
    sao_engine_event_bus_handle_t handle,
    const char* owner_id_utf8,
    uint32_t* out_removed_count);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish(
    sao_engine_event_bus_handle_t handle,
    const char* topic_utf8,
    const uint8_t* json_payload_utf8,
    size_t payload_len,
    const SaoEngineEventHeader* header);

// Snapshot the ring buffer for late subscribers.  When out_events is
// null, *out_event_count is set to how many events are available.
struct SaoEngineRingEntry {
    // topic and json are indices into the concatenated buffers below.
    uint32_t topic_offset;
    uint32_t topic_length;
    uint32_t json_offset;
    uint32_t json_length;
    SaoEngineEventHeader header;
};

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_recent(
    sao_engine_event_bus_handle_t handle,
    uint32_t max_events,
    SaoEngineRingEntry* out_events,
    size_t out_events_capacity,
    char* out_topics_utf8,
    size_t topics_capacity,
    uint8_t* out_jsons_utf8,
    size_t jsons_capacity,
    uint32_t* out_event_count,
    size_t* out_topics_used,
    size_t* out_jsons_used);

// Aggregate counters — matches the Python `snapshot()` return.
struct SaoEngineBusStats {
    uint64_t published;
    uint64_t retained;
    uint64_t callback_failures;
    uint64_t slow_callbacks;
    uint32_t topic_count;
    uint32_t active_subscription_count;
};

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_stats(
    sao_engine_event_bus_handle_t handle, SaoEngineBusStats* out_stats);

// ---------------------------------------------------------------------------
// Wave 5 / Phase 1 — priority + cancel API.
//
// Extends the base subscribe/publish above with per-subscriber priority
// ordering and a cancellation return code.  The two APIs share the same
// underlying bus (create/destroy/unsubscribe are unchanged); the Wave 5
// entry points are named ``_wave5`` so the base API stays byte-stable
// for consumers that already bound against it.
//
// Delivery order for a single publish:
//   1. subscribers with higher ``priority`` fire before lower.
//   2. within the same priority, insertion order (FIFO).
//   3. once any callback returns ``SAO_ENGINE_EVENT_CANCELLED``, the
//      remaining subscribers are skipped.
//
// A subscriber that mutates the bus (unsubscribe / publish another
// topic) from inside a callback is supported — see
// ``event_bus_unsubscribe_mid_fire`` in the Wave 5 tests.
// ---------------------------------------------------------------------------

// Callback return codes for the wave5 API.
#define SAO_ENGINE_EVENT_CONTINUE   0
#define SAO_ENGINE_EVENT_CANCELLED  1

typedef int (SAO_ENGINE_CALL* sao_engine_event_wave5_callback_t)(
    const char* topic_utf8,
    const uint8_t* data_ptr,
    size_t data_size,
    void* user_data);

struct SaoEngineWave5PublishOptions {
    // Non-zero when the caller wants asynchronous semantics.  Currently
    // implemented as synchronous dispatch that still guarantees no
    // subscribe/unsubscribe deadlock when a callback mutates the bus —
    // a follow-up slice will move async publish onto a work queue.
    uint32_t async;
};

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create_wave5(
    sao_engine_event_bus_handle_t* out_handle);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_subscribe_wave5(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    int32_t priority,
    sao_engine_event_wave5_callback_t callback,
    void* user_data,
    sao_engine_subscription_t* out_handle);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish_wave5(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size);

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish_ex_wave5(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size,
    const SaoEngineWave5PublishOptions* options);

#ifdef __cplusplus
}  // extern "C"
#endif
