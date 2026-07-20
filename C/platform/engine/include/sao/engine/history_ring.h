// SAO Auto — generic history ring.
//
// A fixed-capacity time-stamped ring buffer used by
// plugins that need to keep a sliding window of arbitrary payloads.
//
// Ports the platform-generic ``HistoryRing`` primitive: capacity is
// fixed at construction, ``push`` appends and overwrites the oldest
// entry when full, ``get`` and ``range`` read back entries indexed from
// newest.
//
// The ring is entry-size-agnostic — the caller supplies ``entry_size``
// bytes per entry.  The engine does not interpret payload contents; a
// plugin may store its own POD struct, a JSON blob, a raw byte buffer,
// etc.  Timestamps are 64-bit millisecond ticks supplied by the
// caller — the engine does not read any clock.
//
// Threading: the ring uses a mutex, so concurrent push/get from
// multiple plugin threads is safe.  Callers who need lock-free access
// should stage into their own local buffer and push once per tick.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/engine/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_engine_history_ring_s* sao_engine_history_ring_handle_t;

SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_create(
    uint32_t capacity,       // maximum number of retained entries; must be > 0
    uint32_t entry_size,     // bytes per entry; must be > 0
    sao_engine_history_ring_handle_t* out_handle);

SAO_ENGINE_API void SAO_ENGINE_CALL sao_engine_history_ring_destroy(
    sao_engine_history_ring_handle_t handle);

// Append one entry to the ring.  When the ring is at capacity the
// oldest entry is overwritten.  ``entry_ptr`` must point to exactly
// ``entry_size`` bytes (as configured at create).
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_push(
    sao_engine_history_ring_handle_t handle,
    const void* entry_ptr,
    uint64_t timestamp_ms);

// Read one entry.  ``index`` is signed to allow negative indices; the
// mapping is:
//   0        -> newest entry
//   1        -> second newest
//   count-1  -> oldest
//   -1       -> oldest
//   -2       -> second oldest
//   -count   -> newest
// ``out_entry`` receives ``entry_size`` bytes; ``out_timestamp`` receives
// the timestamp supplied at push time.  Both may be null when the caller
// only wants presence.  SAO_STATUS_ERR_NOT_FOUND when the ring is empty
// or the index is out of range.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_get(
    sao_engine_history_ring_handle_t handle,
    int32_t index,
    void* out_entry,
    uint64_t* out_timestamp);

// Fetch every entry whose timestamp lies inside [start_ms, end_ms]
// (inclusive).  Entries are written oldest-first to make chronological
// consumption trivial for plots and per-tick analysers.
//
// ``out_entries`` must be a caller-owned buffer of at least
// ``capacity * entry_size`` bytes; ``out_timestamps`` may be null when
// the caller doesn't need per-entry ts.  ``capacity`` is the number of
// slots the caller allocated (in *entries*, not bytes).
//
// SAO_STATUS_ERR_BUFFER_TOO_SMALL is returned when more entries match
// than fit; ``*out_count`` still reports how many *would* have been
// written so callers can retry with a larger buffer.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_range(
    sao_engine_history_ring_handle_t handle,
    uint64_t start_ms,
    uint64_t end_ms,
    void* out_entries,
    uint64_t* out_timestamps,   // may be null
    uint32_t capacity,
    uint32_t* out_count);

// Drop every retained entry.  Capacity and entry_size are preserved.
SAO_ENGINE_API sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_clear(
    sao_engine_history_ring_handle_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
