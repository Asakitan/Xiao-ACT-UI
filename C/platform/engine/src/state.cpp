#include "sao/engine/state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <condition_variable>

namespace {

struct StoredValue {
    SaoEngineStateValue value{};
    std::string string_value;
};

struct StateSubscriber {
    sao_engine_state_subscription_t token = 0;
    sao_engine_state_change_callback_t callback = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> active{true};
    std::mutex callback_mutex;
    std::condition_variable callback_idle;
    size_t active_callbacks = 0;
};

uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool valid_key(const char* key) noexcept {
    return key != nullptr && key[0] != '\0';
}

}  // namespace

struct sao_engine_state_s {
    mutable std::shared_mutex mutex;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    std::unordered_map<std::string, StoredValue> values;
    std::unordered_map<sao_engine_state_subscription_t,
                       std::shared_ptr<StateSubscriber>>
        subscribers;
    std::atomic<sao_engine_state_subscription_t> next_token{1};
    bool destroying = false;
    size_t active_operations = 0;
};

namespace {

std::mutex g_state_registry_mutex;
std::unordered_map<sao_engine_state_handle_t,
                   std::shared_ptr<sao_engine_state_s>> g_states;
thread_local std::vector<const sao_engine_state_s*> g_state_operation_stack;

std::shared_ptr<sao_engine_state_s> acquire_state(
    sao_engine_state_handle_t raw) noexcept {
    try {
        std::lock_guard<std::mutex> registry_lock(g_state_registry_mutex);
        const auto found = g_states.find(raw);
        if (found == g_states.end()) return nullptr;
        auto state = found->second;
        {
            std::lock_guard<std::mutex> lifecycle_lock(state->lifecycle_mutex);
            if (state->destroying) return nullptr;
            ++state->active_operations;
        }
        try {
            g_state_operation_stack.push_back(state.get());
        } catch (...) {
            std::lock_guard<std::mutex> lifecycle_lock(state->lifecycle_mutex);
            --state->active_operations;
            state->lifecycle_cv.notify_all();
            return nullptr;
        }
        return state;
    } catch (...) {
        return nullptr;
    }
}

size_t owned_state_operations(const sao_engine_state_s* state) {
    return static_cast<size_t>(std::count(g_state_operation_stack.begin(),
                                          g_state_operation_stack.end(), state));
}

class StateOperationLease final {
  public:
    explicit StateOperationLease(sao_engine_state_handle_t raw)
        : state_(acquire_state(raw)) {}

    ~StateOperationLease() {
        if (!state_) return;
        g_state_operation_stack.pop_back();
        {
            std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
            --state_->active_operations;
        }
        state_->lifecycle_cv.notify_all();
    }

    StateOperationLease(const StateOperationLease&) = delete;
    StateOperationLease& operator=(const StateOperationLease&) = delete;

    explicit operator bool() const noexcept { return state_ != nullptr; }

  private:
    std::shared_ptr<sao_engine_state_s> state_;
};

struct StateCallbackFrame {
    const StateSubscriber* subscriber = nullptr;
};

thread_local std::vector<StateCallbackFrame> g_callback_stack;

class StateCallbackLease final {
  public:
    explicit StateCallbackLease(
        std::shared_ptr<StateSubscriber> subscriber) noexcept
        : subscriber_(std::move(subscriber)) {
        if (!subscriber_) return;
        bool stack_pushed = false;
        bool counted = false;
        try {
            g_callback_stack.push_back({subscriber_.get()});
            stack_pushed = true;
            {
                std::lock_guard<std::mutex> lock(subscriber_->callback_mutex);
                if (!subscriber_->active.load(std::memory_order_acquire)) {
                    g_callback_stack.pop_back();
                    return;
                }
                ++subscriber_->active_callbacks;
                counted = true;
            }
            active_ = true;
        } catch (...) {
            if (counted) {
                try {
                    bool last_callback = false;
                    {
                        std::lock_guard<std::mutex> lock(
                            subscriber_->callback_mutex);
                        last_callback = --subscriber_->active_callbacks == 0;
                    }
                    if (last_callback) subscriber_->callback_idle.notify_all();
                } catch (...) {
                }
            }
            if (stack_pushed) g_callback_stack.pop_back();
        }
    }
    ~StateCallbackLease() {
        if (!active_) return;
        g_callback_stack.pop_back();
        bool last_callback = false;
        {
            std::lock_guard<std::mutex> lock(subscriber_->callback_mutex);
            last_callback = --subscriber_->active_callbacks == 0;
        }
        if (last_callback) subscriber_->callback_idle.notify_all();
    }
    explicit operator bool() const noexcept { return active_; }
  private:
    std::shared_ptr<StateSubscriber> subscriber_;
    bool active_ = false;
};

size_t callback_owned(const StateSubscriber* subscriber) {
    return static_cast<size_t>(std::count_if(
        g_callback_stack.begin(), g_callback_stack.end(),
        [subscriber](const StateCallbackFrame& frame) {
            return frame.subscriber == subscriber;
        }));
}

void deactivate_subscriber(const std::shared_ptr<StateSubscriber>& subscriber) {
    subscriber->active.store(false, std::memory_order_release);
    const size_t owned = callback_owned(subscriber.get());
    std::unique_lock<std::mutex> lock(subscriber->callback_mutex);
    subscriber->callback_idle.wait(lock, [&] {
        return subscriber->active_callbacks <= owned;
    });
}

std::vector<std::shared_ptr<StateSubscriber>> snapshot_subscribers(
    sao_engine_state_handle_t handle) {
    std::vector<std::shared_ptr<StateSubscriber>> result;
    std::shared_lock lock(handle->mutex);
    result.reserve(handle->subscribers.size());
    for (const auto& [token, subscriber] : handle->subscribers) {
        (void)token;
        if (subscriber->active.load(std::memory_order_acquire)) {
            result.push_back(subscriber);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs->token < rhs->token;
              });
    return result;
}

void notify_change(sao_engine_state_handle_t handle,
                   const std::string& key,
                   const StoredValue* stored,
                   bool erased) noexcept {
    try {
        auto subscribers = snapshot_subscribers(handle);
        for (const auto& subscriber : subscribers) {
            if (!subscriber->active.load(std::memory_order_acquire)) {
                continue;
            }
            StateCallbackLease callback_lease(subscriber);
            if (!callback_lease) continue;
            try {
                subscriber->callback(
                    key.c_str(), stored != nullptr ? &stored->value : nullptr,
                    stored != nullptr &&
                            stored->value.value_type == SAO_ENGINE_STATE_STRING
                        ? stored->string_value.c_str()
                        : nullptr,
                    erased ? 1U : 0U, subscriber->user_data);
            } catch (...) {
            }
        }
    } catch (...) {
    }
}

sao_status_t set_value(sao_engine_state_handle_t handle,
                       const char* key,
                       StoredValue stored) {
    if (handle == nullptr || !valid_key(key)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        stored.value.timestamp_ns = now_ns();
        const std::string owned_key(key);
        {
            std::unique_lock lock(handle->mutex);
            handle->values[owned_key] = stored;
        }
        notify_change(handle, owned_key, &stored, false);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

}  // namespace

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_create(
    sao_engine_state_handle_t* out_handle) {
    if (out_handle != nullptr) {
        *out_handle = nullptr;
    }
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        auto state = std::make_shared<sao_engine_state_s>();
        {
            std::lock_guard<std::mutex> lock(g_state_registry_mutex);
            const auto [unused, inserted] = g_states.emplace(state.get(), state);
            (void)unused;
            if (!inserted) return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        *out_handle = state.get();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_ENGINE_CALL sao_engine_state_destroy(
    sao_engine_state_handle_t handle) {
    if (handle == nullptr) return;
    std::shared_ptr<sao_engine_state_s> state;
    try {
        {
            std::lock_guard<std::mutex> registry_lock(g_state_registry_mutex);
            const auto found = g_states.find(handle);
            if (found == g_states.end()) return;
            state = found->second;
            g_states.erase(found);
        }
        std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
        state->destroying = true;
        const size_t owned = owned_state_operations(state.get());
        if (state->active_operations > owned) {
            state->lifecycle_cv.wait(lock, [&] {
                return state->active_operations <= owned;
            });
        }
    } catch (...) {
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_set_int64(
    sao_engine_state_handle_t handle,
    const char* key,
    int64_t value,
    int32_t source) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    StoredValue stored;
    stored.value.value_type = SAO_ENGINE_STATE_INT64;
    stored.value.source = source;
    stored.value.as_int64 = value;
    return set_value(handle, key, std::move(stored));
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_set_double(
    sao_engine_state_handle_t handle,
    const char* key,
    double value,
    int32_t source) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    StoredValue stored;
    stored.value.value_type = SAO_ENGINE_STATE_DOUBLE;
    stored.value.source = source;
    stored.value.as_double = value;
    return set_value(handle, key, std::move(stored));
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_set_string(
    sao_engine_state_handle_t handle,
    const char* key,
    const char* value,
    int32_t source) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        StoredValue stored;
        stored.value.value_type = SAO_ENGINE_STATE_STRING;
        stored.value.source = source;
        stored.string_value = value;
        stored.value.string_len =
            static_cast<uint32_t>(stored.string_value.size() + 1);
        return set_value(handle, key, std::move(stored));
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_get(
    sao_engine_state_handle_t handle,
    const char* key,
    SaoEngineStateValue* out_value,
    char* out_string,
    size_t string_capacity) {
    if (out_value != nullptr) {
        *out_value = {};
    }
    if (handle == nullptr || !valid_key(key) || out_value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::shared_lock lock(handle->mutex);
        const auto found = handle->values.find(key);
        if (found == handle->values.end()) {
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        *out_value = found->second.value;
        if (found->second.value.value_type == SAO_ENGINE_STATE_STRING) {
            const size_t required = found->second.string_value.size() + 1;
            if (out_string == nullptr || string_capacity < required) {
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            }
            std::memcpy(out_string, found->second.string_value.c_str(),
                        required);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_erase(
    sao_engine_state_handle_t handle,
    const char* key) {
    if (handle == nullptr || !valid_key(key)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        const std::string owned_key(key);
        {
            std::unique_lock lock(handle->mutex);
            if (handle->values.erase(owned_key) == 0) {
                return SAO_STATUS_ERR_NOT_FOUND;
            }
        }
        notify_change(handle, owned_key, nullptr, true);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_snapshot(
    sao_engine_state_handle_t handle,
    SaoEngineStateSnapshotEntry* out_entries,
    size_t entries_capacity,
    char* out_buffer,
    size_t buffer_capacity,
    uint32_t* out_entry_count,
    size_t* out_buffer_used) {
    if (out_entry_count != nullptr) {
        *out_entry_count = 0;
    }
    if (out_buffer_used != nullptr) {
        *out_buffer_used = 0;
    }
    if (handle == nullptr || out_entry_count == nullptr ||
        out_buffer_used == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::vector<std::pair<std::string, StoredValue>> snapshot;
        {
            std::shared_lock lock(handle->mutex);
            snapshot.reserve(handle->values.size());
            for (const auto& entry : handle->values) {
                snapshot.push_back(entry);
            }
        }
        std::sort(snapshot.begin(), snapshot.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.first < rhs.first;
                  });
        size_t required = 0;
        for (const auto& [key, stored] : snapshot) {
            required += key.size();
            if (stored.value.value_type == SAO_ENGINE_STATE_STRING) {
                required += stored.string_value.size();
            }
        }
        *out_entry_count = static_cast<uint32_t>(snapshot.size());
        *out_buffer_used = required;
        if (out_entries == nullptr) {
            return SAO_STATUS_OK;
        }
        if (entries_capacity < snapshot.size() ||
            (required != 0 &&
             (out_buffer == nullptr || buffer_capacity < required))) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        size_t offset = 0;
        for (size_t index = 0; index < snapshot.size(); ++index) {
            const auto& [key, stored] = snapshot[index];
            auto& entry = out_entries[index];
            entry = {};
            entry.key_offset = static_cast<uint32_t>(offset);
            entry.key_length = static_cast<uint32_t>(key.size());
            entry.value = stored.value;
            if (!key.empty()) {
                std::memcpy(out_buffer + offset, key.data(), key.size());
            }
            offset += key.size();
            if (stored.value.value_type == SAO_ENGINE_STATE_STRING) {
                entry.string_offset = static_cast<uint32_t>(offset);
                entry.string_length =
                    static_cast<uint32_t>(stored.string_value.size());
                if (!stored.string_value.empty()) {
                    std::memcpy(out_buffer + offset,
                                stored.string_value.data(),
                                stored.string_value.size());
                }
                offset += stored.string_value.size();
            }
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_subscribe(
    sao_engine_state_handle_t handle,
    sao_engine_state_change_callback_t callback,
    void* user_data,
    sao_engine_state_subscription_t* out_subscription) {
    if (out_subscription != nullptr) {
        *out_subscription = 0;
    }
    if (handle == nullptr || callback == nullptr || out_subscription == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        auto subscriber = std::make_shared<StateSubscriber>();
        subscriber->token =
            handle->next_token.fetch_add(1, std::memory_order_relaxed);
        subscriber->callback = callback;
        subscriber->user_data = user_data;
        {
            std::unique_lock lock(handle->mutex);
            handle->subscribers.emplace(subscriber->token, subscriber);
        }
        *out_subscription = subscriber->token;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_ENGINE_CALL sao_engine_state_unsubscribe(
    sao_engine_state_handle_t handle,
    sao_engine_state_subscription_t subscription) {
    if (handle == nullptr || subscription == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    StateOperationLease lease(handle);
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::unique_lock lock(handle->mutex);
        const auto found = handle->subscribers.find(subscription);
        if (found == handle->subscribers.end()) {
            return SAO_STATUS_ERR_SUBSCRIPTION_GONE;
        }
        const auto subscriber = found->second;
        handle->subscribers.erase(found);
        lock.unlock();
        subscriber->active.store(false, std::memory_order_release);
        deactivate_subscriber(subscriber);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
