// Shipping EventBus implementation for sao_platform_engine.
//
// CMake compiles this translation unit, not the historical
// ``event_bus.cpp`` file.  Both the base C ABI and the ``_priority`` entry
// points below therefore use this production state, dispatch, retention,
// subscription, and statistics implementation.  The ``_wave5`` symbols are
// kept as backward-compatibility aliases for existing consumers.

#include "sao/engine/event_bus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kDefaultRecentCapacity = 200;

struct SubscriberEntry {
    sao_engine_subscription_t token = 0;
    std::string event_type;
    std::string owner_id;
    int32_t priority = 0;
    uint64_t insertion_seq = 0;
    sao_engine_event_callback_t callback = nullptr;
    sao_engine_event_priority_callback_t priority_callback = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> active{true};
};

struct RetainedEvent {
    std::string topic;
    std::vector<uint8_t> payload;
    SaoEngineEventHeader header{};
};

uint64_t unix_now_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool valid_bytes(const uint8_t* data, size_t size) noexcept {
    return size == 0 || data != nullptr;
}

}  // namespace

struct sao_engine_event_bus_s {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<SubscriberEntry>>>
        subscribers;
    std::unordered_map<sao_engine_subscription_t,
                       std::shared_ptr<SubscriberEntry>>
        subscriptions_by_token;
    std::unordered_set<std::string> ephemeral_topics;
    std::deque<RetainedEvent> recent;
    uint32_t max_recent = kDefaultRecentCapacity;
    float slow_callback_ms = 0.0F;
    std::atomic<sao_engine_subscription_t> next_token{1};
    std::atomic<uint64_t> next_insertion_seq{1};
    std::atomic<uint64_t> next_event_sequence{1};
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> retained{0};
    std::atomic<uint64_t> callback_failures{0};
    std::atomic<uint64_t> slow_callbacks{0};
};

namespace {

std::vector<std::shared_ptr<SubscriberEntry>> snapshot_subscribers(
    sao_engine_event_bus_handle_t handle,
    const std::string& event_type,
    bool include_wildcard) {
    std::vector<std::shared_ptr<SubscriberEntry>> snapshot;
    std::shared_lock lock(handle->mutex);

    const auto append_bucket = [&](const std::string& key) {
        const auto bucket = handle->subscribers.find(key);
        if (bucket == handle->subscribers.end()) {
            return;
        }
        snapshot.reserve(snapshot.size() + bucket->second.size());
        for (const auto& subscriber : bucket->second) {
            if (subscriber->active.load(std::memory_order_acquire)) {
                snapshot.push_back(subscriber);
            }
        }
    };

    append_bucket(event_type);
    if (include_wildcard && event_type != "*") {
        append_bucket("*");
    }

    std::stable_sort(
        snapshot.begin(), snapshot.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs->priority != rhs->priority) {
                return lhs->priority > rhs->priority;
            }
            return lhs->insertion_seq < rhs->insertion_seq;
        });
    return snapshot;
}

void retain_event(sao_engine_event_bus_handle_t handle,
                  const std::string& topic,
                  const uint8_t* payload,
                  size_t payload_len,
                  const SaoEngineEventHeader& header) {
    std::unique_lock lock(handle->mutex);
    if (handle->ephemeral_topics.contains(topic)) {
        return;
    }

    RetainedEvent event;
    event.topic = topic;
    if (payload_len != 0) {
        event.payload.assign(payload, payload + payload_len);
    }
    event.header = header;
    handle->recent.push_back(std::move(event));
    while (handle->recent.size() > handle->max_recent) {
        handle->recent.pop_front();
    }
    handle->retained.fetch_add(1, std::memory_order_relaxed);
}

sao_status_t dispatch_event(sao_engine_event_bus_handle_t handle,
                            const char* event_type_utf8,
                            const uint8_t* data_ptr,
                            size_t data_size,
                            const SaoEngineEventHeader* supplied_header,
                            bool include_wildcard) {
    if (handle == nullptr || event_type_utf8 == nullptr ||
        event_type_utf8[0] == '\0' || !valid_bytes(data_ptr, data_size)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        const std::string event_type(event_type_utf8);
        SaoEngineEventHeader header =
            supplied_header != nullptr ? *supplied_header
                                       : SaoEngineEventHeader{};
        header.sequence_id = handle->next_event_sequence.fetch_add(
            1, std::memory_order_relaxed);
        if (header.ts_unix_ns == 0) {
            header.ts_unix_ns = unix_now_ns();
        }
        if (supplied_header == nullptr) {
            header.confidence = 1.0F;
        }

        handle->published.fetch_add(1, std::memory_order_relaxed);
        retain_event(handle, event_type, data_ptr, data_size, header);
        auto snapshot =
            snapshot_subscribers(handle, event_type, include_wildcard);

        for (const auto& subscriber : snapshot) {
            if (!subscriber->active.load(std::memory_order_acquire)) {
                continue;
            }

            const auto started = std::chrono::steady_clock::now();
            int callback_result = SAO_ENGINE_EVENT_CONTINUE;
            try {
                if (subscriber->priority_callback != nullptr) {
                    callback_result = subscriber->priority_callback(
                        event_type_utf8, data_ptr, data_size,
                        subscriber->user_data);
                } else if (subscriber->callback != nullptr) {
                    subscriber->callback(event_type_utf8, data_ptr, data_size,
                                         &header, subscriber->user_data);
                }
            } catch (...) {
                handle->callback_failures.fetch_add(
                    1, std::memory_order_relaxed);
            }

            if (handle->slow_callback_ms > 0.0F) {
                const auto elapsed =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count();
                if (elapsed > handle->slow_callback_ms) {
                    handle->slow_callbacks.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }

            if (callback_result == SAO_ENGINE_EVENT_CANCELLED) {
                break;
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t subscribe_common(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const char* owner_id_utf8,
    int32_t priority,
    sao_engine_event_callback_t callback,
    sao_engine_event_priority_callback_t priority_callback,
    void* user_data,
    sao_engine_subscription_t* out_subscription) {
    if (out_subscription != nullptr) {
        *out_subscription = 0;
    }
    if (handle == nullptr || event_type_utf8 == nullptr ||
        event_type_utf8[0] == '\0' ||
        (callback == nullptr && priority_callback == nullptr) ||
        out_subscription == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        auto subscriber = std::make_shared<SubscriberEntry>();
        subscriber->token =
            handle->next_token.fetch_add(1, std::memory_order_relaxed);
        subscriber->event_type = event_type_utf8;
        subscriber->owner_id = owner_id_utf8 != nullptr ? owner_id_utf8 : "";
        subscriber->priority = priority;
        subscriber->insertion_seq = handle->next_insertion_seq.fetch_add(
            1, std::memory_order_relaxed);
        subscriber->callback = callback;
        subscriber->priority_callback = priority_callback;
        subscriber->user_data = user_data;

        {
            std::unique_lock lock(handle->mutex);
            handle->subscribers[subscriber->event_type].push_back(subscriber);
            handle->subscriptions_by_token.emplace(subscriber->token,
                                                    subscriber);
        }
        *out_subscription = subscriber->token;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

}  // namespace

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create(
    uint32_t max_recent,
    float slow_callback_ms,
    sao_engine_event_bus_handle_t* out_handle) {
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    if (out_handle == nullptr || !std::isfinite(slow_callback_ms)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        auto handle = std::make_unique<sao_engine_event_bus_s>();
        handle->max_recent =
            max_recent == 0 ? kDefaultRecentCapacity : max_recent;
        handle->slow_callback_ms = std::max(0.0F, slow_callback_ms);
        *out_handle = handle.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_ENGINE_CALL sao_engine_event_bus_destroy(
    sao_engine_event_bus_handle_t handle) {
    delete handle;
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_mark_ephemeral(
    sao_engine_event_bus_handle_t handle,
    const char* topic_utf8) {
    if (handle == nullptr || topic_utf8 == nullptr || topic_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::unique_lock lock(handle->mutex);
        handle->ephemeral_topics.emplace(topic_utf8);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_subscribe(
    sao_engine_event_bus_handle_t handle,
    const char* topic_utf8,
    const char* owner_id_utf8,
    sao_engine_event_callback_t callback,
    void* user_data,
    sao_engine_subscription_t* out_subscription) {
    return subscribe_common(handle, topic_utf8, owner_id_utf8, 0, callback,
                            nullptr, user_data, out_subscription);
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_unsubscribe(
    sao_engine_event_bus_handle_t handle,
    sao_engine_subscription_t subscription) {
    if (handle == nullptr || subscription == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        std::unique_lock lock(handle->mutex);
        const auto found = handle->subscriptions_by_token.find(subscription);
        if (found == handle->subscriptions_by_token.end()) {
            return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
        }

        const auto subscriber = found->second;
        subscriber->active.store(false, std::memory_order_release);
        handle->subscriptions_by_token.erase(found);

        const auto bucket = handle->subscribers.find(subscriber->event_type);
        if (bucket != handle->subscribers.end()) {
            auto& entries = bucket->second;
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                               [subscription](const auto& entry) {
                                   return entry->token == subscription;
                               }),
                entries.end());
            if (entries.empty()) {
                handle->subscribers.erase(bucket);
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_unsubscribe_owner(
    sao_engine_event_bus_handle_t handle,
    const char* owner_id_utf8,
    uint32_t* out_removed_count) {
    if (out_removed_count != nullptr) {
        *out_removed_count = 0;
    }
    if (handle == nullptr || owner_id_utf8 == nullptr ||
        out_removed_count == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        uint32_t removed = 0;
        std::unique_lock lock(handle->mutex);
        for (auto bucket = handle->subscribers.begin();
             bucket != handle->subscribers.end();) {
            auto& entries = bucket->second;
            for (const auto& subscriber : entries) {
                if (subscriber->owner_id == owner_id_utf8 &&
                    subscriber->active.exchange(false,
                                                std::memory_order_acq_rel)) {
                    handle->subscriptions_by_token.erase(subscriber->token);
                    ++removed;
                }
            }
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                               [](const auto& subscriber) {
                                   return !subscriber->active.load(
                                       std::memory_order_acquire);
                               }),
                entries.end());
            if (entries.empty()) {
                bucket = handle->subscribers.erase(bucket);
            } else {
                ++bucket;
            }
        }
        *out_removed_count = removed;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_publish(
    sao_engine_event_bus_handle_t handle,
    const char* topic_utf8,
    const uint8_t* json_payload_utf8,
    size_t payload_len,
    const SaoEngineEventHeader* header) {
    return dispatch_event(handle, topic_utf8, json_payload_utf8, payload_len,
                          header, true);
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_recent(
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
    size_t* out_jsons_used) {
    if (out_event_count != nullptr) {
        *out_event_count = 0;
    }
    if (out_topics_used != nullptr) {
        *out_topics_used = 0;
    }
    if (out_jsons_used != nullptr) {
        *out_jsons_used = 0;
    }
    if (handle == nullptr || out_event_count == nullptr ||
        out_topics_used == nullptr || out_jsons_used == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        std::vector<RetainedEvent> snapshot;
        {
            std::shared_lock lock(handle->mutex);
            const size_t available = handle->recent.size();
            const size_t requested =
                max_events == 0
                    ? available
                    : std::min(available, static_cast<size_t>(max_events));
            snapshot.reserve(requested);
            const size_t first = available - requested;
            for (size_t index = first; index < available; ++index) {
                snapshot.push_back(handle->recent[index]);
            }
        }

        size_t topics_required = 0;
        size_t jsons_required = 0;
        for (const auto& event : snapshot) {
            topics_required += event.topic.size();
            jsons_required += event.payload.size();
        }

        *out_event_count = static_cast<uint32_t>(snapshot.size());
        *out_topics_used = topics_required;
        *out_jsons_used = jsons_required;

        if (out_events == nullptr) {
            return SAO_STATUS_OK;
        }
        if (out_events_capacity < snapshot.size() ||
            (topics_required != 0 &&
             (out_topics_utf8 == nullptr ||
              topics_capacity < topics_required)) ||
            (jsons_required != 0 &&
             (out_jsons_utf8 == nullptr || jsons_capacity < jsons_required))) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }

        size_t topic_offset = 0;
        size_t json_offset = 0;
        for (size_t index = 0; index < snapshot.size(); ++index) {
            const auto& source = snapshot[index];
            auto& destination = out_events[index];
            destination.topic_offset = static_cast<uint32_t>(topic_offset);
            destination.topic_length =
                static_cast<uint32_t>(source.topic.size());
            destination.json_offset = static_cast<uint32_t>(json_offset);
            destination.json_length =
                static_cast<uint32_t>(source.payload.size());
            destination.header = source.header;
            if (!source.topic.empty()) {
                std::memcpy(out_topics_utf8 + topic_offset,
                            source.topic.data(), source.topic.size());
            }
            if (!source.payload.empty()) {
                std::memcpy(out_jsons_utf8 + json_offset,
                            source.payload.data(), source.payload.size());
            }
            topic_offset += source.topic.size();
            json_offset += source.payload.size();
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_stats(
    sao_engine_event_bus_handle_t handle,
    SaoEngineBusStats* out_stats) {
    if (handle == nullptr || out_stats == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        SaoEngineBusStats stats{};
        stats.published = handle->published.load(std::memory_order_relaxed);
        stats.retained = handle->retained.load(std::memory_order_relaxed);
        stats.callback_failures =
            handle->callback_failures.load(std::memory_order_relaxed);
        stats.slow_callbacks =
            handle->slow_callbacks.load(std::memory_order_relaxed);
        {
            std::shared_lock lock(handle->mutex);
            stats.topic_count =
                static_cast<uint32_t>(handle->subscribers.size());
            stats.active_subscription_count = static_cast<uint32_t>(
                handle->subscriptions_by_token.size());
        }
        *out_stats = stats;
        return SAO_STATUS_OK;
    } catch (...) {
        *out_stats = {};
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create_priority(
    sao_engine_event_bus_handle_t* out_handle) {
    return sao_engine_event_bus_create(kDefaultRecentCapacity, 0.0F,
                                       out_handle);
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_subscribe_priority(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    int32_t priority,
    sao_engine_event_priority_callback_t callback,
    void* user_data,
    sao_engine_subscription_t* out_handle) {
    return subscribe_common(handle, event_type_utf8, "", priority, nullptr,
                            callback, user_data, out_handle);
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_publish_priority(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size) {
    return dispatch_event(handle, event_type_utf8, data_ptr, data_size,
                          nullptr, false);
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_publish_ex_priority(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size,
    const SaoEnginePriorityPublishOptions*) {
    return dispatch_event(handle, event_type_utf8, data_ptr, data_size,
                          nullptr, false);
}

// ---------------------------------------------------------------------------
// Backward-compatibility aliases — historical ``_wave5`` names.
// New code should bind against the ``_priority`` symbols above.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_event_bus_create_wave5(
    sao_engine_event_bus_handle_t* out_handle) {
    return sao_engine_event_bus_create_priority(out_handle);
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_subscribe_wave5(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    int32_t priority,
    sao_engine_event_priority_callback_t callback,
    void* user_data,
    sao_engine_subscription_t* out_handle) {
    return sao_engine_event_bus_subscribe_priority(handle, event_type_utf8,
                                                    priority, callback,
                                                    user_data, out_handle);
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_publish_wave5(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size) {
    return sao_engine_event_bus_publish_priority(handle, event_type_utf8,
                                                  data_ptr, data_size);
}

extern "C" sao_status_t SAO_ENGINE_CALL
sao_engine_event_bus_publish_ex_wave5(
    sao_engine_event_bus_handle_t handle,
    const char* event_type_utf8,
    const uint8_t* data_ptr,
    size_t data_size,
    const SaoEnginePriorityPublishOptions* options) {
    return sao_engine_event_bus_publish_ex_priority(handle, event_type_utf8,
                                                     data_ptr, data_size,
                                                     options);
}
