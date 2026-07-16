// SAO Auto — platform/engine/src/history_ring.cpp
//
// Wave 6 / Phase 5 — game-agnostic history ring buffer.
//
// The ring is a plain fixed-capacity vector with a head cursor and a
// filled flag.  Push overwrites the oldest entry when full; get maps
// index 0 -> newest.  Range walks the buffer in chronological order and
// copies matching entries out to a caller-owned linear buffer.
//
// No knowledge of what a "entry" is beyond its byte size — DPS rows,
// boss buff records, telemetry samples, whatever.  Timestamps are ms.
//
// Threading: a plain mutex, same policy as state_machine.  Contention
// is expected to be low; plugins that need very high push rates should
// stage into their own local buffer and push once per tick.

#include "sao/engine/history_ring.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct Slot {
    uint64_t timestamp_ms = 0;
    // The payload lives in a separate contiguous buffer indexed by slot
    // number; a slot only carries the timestamp so we can walk timestamp-
    // ordered filters without touching payload memory.
};

}  // namespace

struct sao_engine_history_ring_s {
    mutable std::mutex mtx;

    uint32_t capacity   = 0;
    uint32_t entry_size = 0;
    uint32_t head       = 0;   // next write slot
    uint32_t count      = 0;   // filled slot count (0..capacity)

    std::vector<uint8_t> storage;   // capacity * entry_size bytes
    std::vector<Slot>    slots;     // capacity slots
};

namespace {

// Map "logical index (0 = oldest)" to "physical slot index".
size_t physicalIndex(const sao_engine_history_ring_s& ring, size_t logical) {
    // Oldest is at (head - count) mod capacity when full, or at 0 when
    // the ring is still filling up.
    if (ring.count < ring.capacity) {
        // Ring hasn't wrapped yet — slots [0, count) hold data in
        // insertion order, oldest at 0.
        return logical;
    }
    return (static_cast<size_t>(ring.head) + logical) % ring.capacity;
}

// Convert the caller's signed index (0 = newest, -1 = oldest,
// negative counts from oldest) to a physical slot.  Returns
// SIZE_MAX for out-of-range indices.
size_t resolveIndex(const sao_engine_history_ring_s& ring, int32_t index) {
    if (ring.count == 0) return static_cast<size_t>(-1);
    int64_t signed_idx = static_cast<int64_t>(index);
    // Python-style negative indexing: -1 = oldest, -count = newest.
    if (signed_idx < 0) signed_idx += static_cast<int64_t>(ring.count);
    if (signed_idx < 0 || signed_idx >= static_cast<int64_t>(ring.count)) {
        return static_cast<size_t>(-1);
    }
    // ``signed_idx`` now runs 0=newest .. count-1=oldest; flip to
    // 0=oldest so physicalIndex works from the chronological start.
    const size_t logical_oldest_first =
        static_cast<size_t>(ring.count) - 1 - static_cast<size_t>(signed_idx);
    return physicalIndex(ring, logical_oldest_first);
}

}  // namespace

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_create(
    uint32_t capacity,
    uint32_t entry_size,
    sao_engine_history_ring_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (capacity == 0 || entry_size == 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    auto ring = std::make_unique<sao_engine_history_ring_s>();
    ring->capacity   = capacity;
    ring->entry_size = entry_size;
    ring->head       = 0;
    ring->count      = 0;
    try {
        ring->storage.resize(static_cast<size_t>(capacity) * entry_size, 0);
        ring->slots.resize(capacity);
    } catch (const std::bad_alloc&) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_handle = ring.release();
    return SAO_STATUS_OK;
}

extern "C" void SAO_ENGINE_CALL sao_engine_history_ring_destroy(
    sao_engine_history_ring_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_push(
    sao_engine_history_ring_handle_t handle,
    const void* entry_ptr,
    uint64_t timestamp_ms) {
    if (handle == nullptr || entry_ptr == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(handle->mtx);

    const size_t offset = static_cast<size_t>(handle->head) * handle->entry_size;
    std::memcpy(handle->storage.data() + offset, entry_ptr, handle->entry_size);
    handle->slots[handle->head].timestamp_ms = timestamp_ms;

    handle->head = (handle->head + 1) % handle->capacity;
    if (handle->count < handle->capacity) handle->count += 1;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_get(
    sao_engine_history_ring_handle_t handle,
    int32_t index,
    void* out_entry,
    uint64_t* out_timestamp) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mtx);

    const size_t phys = resolveIndex(*handle, index);
    if (phys == static_cast<size_t>(-1)) return SAO_STATUS_ERR_NOT_FOUND;

    if (out_entry != nullptr) {
        std::memcpy(out_entry,
                    handle->storage.data() + phys * handle->entry_size,
                    handle->entry_size);
    }
    if (out_timestamp != nullptr) {
        *out_timestamp = handle->slots[phys].timestamp_ms;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_range(
    sao_engine_history_ring_handle_t handle,
    uint64_t start_ms,
    uint64_t end_ms,
    void* out_entries,
    uint64_t* out_timestamps,
    uint32_t capacity,
    uint32_t* out_count) {
    if (handle == nullptr || out_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (start_ms > end_ms) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lk(handle->mtx);

    // Walk oldest -> newest so the caller receives entries in
    // chronological order — perfect for plots and delta computations.
    uint32_t matched = 0;
    uint32_t written = 0;
    const size_t total = handle->count;
    auto* out_bytes = static_cast<uint8_t*>(out_entries);

    for (size_t logical = 0; logical < total; ++logical) {
        const size_t phys = physicalIndex(*handle, logical);
        const uint64_t ts = handle->slots[phys].timestamp_ms;
        if (ts < start_ms || ts > end_ms) continue;
        matched += 1;
        if (out_bytes != nullptr && written < capacity) {
            std::memcpy(out_bytes + static_cast<size_t>(written) * handle->entry_size,
                        handle->storage.data() + phys * handle->entry_size,
                        handle->entry_size);
            if (out_timestamps != nullptr) {
                out_timestamps[written] = ts;
            }
            written += 1;
        }
    }

    *out_count = matched;

    // Buffer-too-small takes precedence over OK when the caller
    // supplied a payload buffer but couldn't fit every match.
    if (out_entries != nullptr && matched > capacity) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_history_ring_clear(
    sao_engine_history_ring_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(handle->mtx);
    handle->head  = 0;
    handle->count = 0;
    // Keep storage/slots allocated so subsequent pushes don't re-alloc.
    std::fill(handle->storage.begin(), handle->storage.end(),
              static_cast<uint8_t>(0));
    for (auto& s : handle->slots) s.timestamp_ms = 0;
    return SAO_STATUS_OK;
}
