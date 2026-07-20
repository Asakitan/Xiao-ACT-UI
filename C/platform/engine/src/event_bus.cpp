// SAO Auto — platform/engine/src/event_bus.cpp
//
// Historical priority-API translation unit.  This file is not compiled
// into sao_platform_engine; the shipping implementation of both the base
// ABI and the ``_priority`` API is ``event_bus_adapter.cpp``.
//
// The source remains as an audit artifact.  Its base-ABI section has
// seven NOT_IMPLEMENTED return statements, followed by an older concrete
// priority implementation.  Neither section is a production runtime path.
//
// Threading: an ``std::shared_mutex`` protects the subscriber table.
// Callbacks fire without the lock held so a subscriber that mutates
// the bus (unsubscribes another subscriber, or publishes another
// event) doesn't deadlock.  This mirrors ``event_bus.py``'s comment
// about ``deepcopy`` happening outside the lock.

#include "sao/engine/event_bus.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Base ABI classification note — this translation unit is DEAD CODE.
//
// PLAN §1.5 lists the base API as "legacy stubs".  On audit the real
// production implementation lives in ``src/event_bus_adapter.cpp`` and
// exports the same entry points; the engine DLL's CMakeLists only lists
// the adapter, so this file (``src/event_bus.cpp``) is never compiled
// into the shipping DLL.  It is preserved so downstream diff tools can
// still inventory the seven dead NOT_IMPLEMENTED returns.  That static
// count says nothing about runtime completeness.  Do NOT wire this file
// back into the build without first removing ``event_bus_adapter.cpp`` or
// symbol duplication will land.
// ---------------------------------------------------------------------------

// Forward-declare the handle so the base stubs and the priority impl can
// share it.
struct sao_engine_event_bus_s;

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create(
    uint32_t, float, sao_engine_event_bus_handle_t* out_handle) {
    if (out_handle != nullptr) *out_handle = nullptr;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

// destroy is defined below because it needs the full struct definition.

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_mark_ephemeral(
    sao_engine_event_bus_handle_t, const char*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_subscribe(
    sao_engine_event_bus_handle_t, const char*, const char*,
    sao_engine_event_callback_t, void*, sao_engine_subscription_t* out_sub) {
    if (out_sub != nullptr) *out_sub = 0;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_unsubscribe_owner(
    sao_engine_event_bus_handle_t, const char*, uint32_t* out_removed) {
    if (out_removed != nullptr) *out_removed = 0;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish(
    sao_engine_event_bus_handle_t, const char*, const uint8_t*, size_t,
    const SaoEngineEventHeader*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_recent(
    sao_engine_event_bus_handle_t, uint32_t, SaoEngineRingEntry*, size_t,
    char*, size_t, uint8_t*, size_t,
    uint32_t* out_count, size_t* out_topics_used, size_t* out_jsons_used) {
    if (out_count != nullptr) *out_count = 0;
    if (out_topics_used != nullptr) *out_topics_used = 0;
    if (out_jsons_used != nullptr) *out_jsons_used = 0;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_stats(
    sao_engine_event_bus_handle_t, SaoEngineBusStats*) {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// Priority API implementation (historical — superseded by adapter)
// ---------------------------------------------------------------------------

struct SubscriberEntry {
    sao_engine_subscription_t token = 0;
    std::string event_type;
    int32_t priority = 0;
    // Insertion sequence for FIFO tie-break at equal priority.  A plain
    // counter is fine — we never reset it during a bus's lifetime and
    // uint64_t won't wrap in any realistic run.
    uint64_t insertion_seq = 0;
    sao_engine_event_priority_callback_t callback = nullptr;
    void* user_data = nullptr;
    // Torn-down subscribers are tombstoned rather than removed so
    // in-flight iteration in publish() doesn't have to worry about
    // vector invalidation.  A subsequent publish sweeps the graveyard.
    bool active = true;
};

struct sao_engine_event_bus_s {
    mutable std::shared_mutex mtx;

    // event_type -> ordered list of subscribers.
    // Kept unsorted internally; publish() sorts a snapshot copy so
    // insertion cost stays O(1).
    std::unordered_map<std::string, std::vector<SubscriberEntry>> subscribers;

    // Reverse index: token -> (event_type, index) so unsubscribe is
    // O(1) without scanning every bucket.
    std::unordered_map<sao_engine_subscription_t, std::string> token_to_type;

    std::atomic<sao_engine_subscription_t> next_token{1};
    std::atomic<uint64_t> next_insertion_seq{1};

    // Recursion depth guard so the tombstone sweep can safely defer
    // compaction while we're mid-fire.
    std::atomic<uint32_t> publish_depth{0};
};

// Destroy has to see the full struct definition, so it lives here.
extern "C" void SAO_ENGINE_CALL sao_engine_event_bus_destroy(
    sao_engine_event_bus_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_unsubscribe(
    sao_engine_event_bus_handle_t handle,
    sao_engine_subscription_t subscription) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::unique_lock lk(handle->mtx);
    auto it = handle->token_to_type.find(subscription);
    if (it == handle->token_to_type.end()) {
        return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
    }
    auto bucket_it = handle->subscribers.find(it->second);
    if (bucket_it != handle->subscribers.end()) {
        for (auto& entry : bucket_it->second) {
            if (entry.token == subscription) {
                entry.active = false;
                break;
            }
        }
        // Only compact when we're not inside a publish call — otherwise
        // the mid-fire snapshot would be walking a freed vector slot.
        if (handle->publish_depth.load(std::memory_order_relaxed) == 0) {
            auto& vec = bucket_it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [](const SubscriberEntry& e) {
                                         return !e.active;
                                     }),
                      vec.end());
        }
    }
    handle->token_to_type.erase(it);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create_priority(
    sao_engine_event_bus_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        *out_handle = new sao_engine_event_bus_s{};
    } catch (const std::bad_alloc&) {
        *out_handle = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_subscribe_priority(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    int32_t priority,
    sao_engine_event_priority_callback_t callback,
    void* user_data,
    sao_engine_subscription_t* out_handle) {
    if (handle == nullptr || event_type_utf8 == nullptr || callback == nullptr
        || out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    SubscriberEntry entry;
    entry.token          = handle->next_token.fetch_add(1, std::memory_order_relaxed);
    entry.event_type     = event_type_utf8;
    entry.priority       = priority;
    entry.insertion_seq  = handle->next_insertion_seq.fetch_add(
        1, std::memory_order_relaxed);
    entry.callback       = callback;
    entry.user_data      = user_data;
    entry.active         = true;

    {
        std::unique_lock lk(handle->mtx);
        auto& bucket = handle->subscribers[entry.event_type];
        bucket.push_back(entry);
        handle->token_to_type[entry.token] = entry.event_type;
    }

    *out_handle = entry.token;
    return SAO_STATUS_OK;
}

namespace {

// Snapshot the subscriber list for a topic under the read lock, sorted
// by priority desc / insertion_seq asc.  Returned by value so publish
// can iterate without holding the mutex — a callback that mutates the
// bus during dispatch doesn't touch this snapshot.
std::vector<SubscriberEntry> snapshotForTopic(
    sao_engine_event_bus_handle_t handle, const std::string& event_type) {
    std::shared_lock lk(handle->mtx);
    auto it = handle->subscribers.find(event_type);
    if (it == handle->subscribers.end()) return {};
    std::vector<SubscriberEntry> snap;
    snap.reserve(it->second.size());
    for (const auto& e : it->second) {
        if (e.active) snap.push_back(e);
    }
    std::sort(snap.begin(), snap.end(),
              [](const SubscriberEntry& a, const SubscriberEntry& b) {
                  if (a.priority != b.priority) return a.priority > b.priority;
                  return a.insertion_seq < b.insertion_seq;
              });
    return snap;
}

sao_status_t doPublish(sao_engine_event_bus_handle_t handle,
                      const char* event_type_utf8,
                      const uint8_t* data_ptr,
                      size_t data_size) {
    if (handle == nullptr || event_type_utf8 == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    const std::string event_type = event_type_utf8;
    auto snap = snapshotForTopic(handle, event_type);

    // A subscribers-list may legitimately be empty — publishing to a
    // topic no one listens to is not an error.
    if (snap.empty()) return SAO_STATUS_OK;

    handle->publish_depth.fetch_add(1, std::memory_order_acq_rel);

    for (const auto& entry : snap) {
        // Snapshot may include a subscriber that was unsubscribed
        // between the snapshot and here.  Re-check active state under
        // the read lock so we skip torn-down subscribers cleanly.
        bool still_active = false;
        {
            std::shared_lock lk(handle->mtx);
            auto bucket_it = handle->subscribers.find(event_type);
            if (bucket_it != handle->subscribers.end()) {
                for (const auto& e : bucket_it->second) {
                    if (e.token == entry.token) {
                        still_active = e.active;
                        break;
                    }
                }
            }
        }
        if (!still_active) continue;

        int rc = entry.callback(event_type_utf8, data_ptr, data_size,
                                entry.user_data);
        if (rc == SAO_ENGINE_EVENT_CANCELLED) break;
    }

    // Sweep tombstones if we're now the outermost publisher.
    if (handle->publish_depth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::unique_lock lk(handle->mtx);
        for (auto& [type, vec] : handle->subscribers) {
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [](const SubscriberEntry& e) {
                                         return !e.active;
                                     }),
                      vec.end());
        }
    }

    return SAO_STATUS_OK;
}

}  // namespace

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish_priority(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size) {
    return doPublish(handle, event_type_utf8, data_ptr, data_size);
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish_ex_priority(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size,
    const SaoEnginePriorityPublishOptions* /*options*/) {
    // Async is currently synchronous but retains the outer contract so
    // callers can migrate to a future work-queue-backed impl without
    // touching call sites.  ``options`` is intentionally unused today.
    return doPublish(handle, event_type_utf8, data_ptr, data_size);
}
