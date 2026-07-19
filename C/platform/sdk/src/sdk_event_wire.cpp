// SAO Auto — Wave 7 SDK event-bus wire.
//
// Forwards `SaoSdkContext::event->*` into the platform's Wave 5 event
// bus (`sao_engine_event_bus_*_wave5`).  Plugins never link the engine
// module directly; they see only the SDK vtable.
//
// The Wave 5 callback signature is `int(topic, data, size, ud)` — we
// wrap the plugin's void-returning `sao_sdk_event_callback_t` in a
// bridge that returns SAO_ENGINE_EVENT_CONTINUE unconditionally.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace sao_sdk_internal {
namespace {

std::mutex g_event_owner_mutex;
std::vector<std::shared_ptr<EventSubscriptionOwner>> g_event_owners;

void preserve_event_owner(const std::shared_ptr<EventSubscriptionOwner>& owner) {
    std::lock_guard<std::mutex> lock(g_event_owner_mutex);
    g_event_owners.push_back(owner);
}

// Bridge — invoked by the Wave 5 bus.  user_data is the
// stable owner stashed at subscribe time. Retired owners remain valid
// because an already-copied bus snapshot may still hold this pointer.
int SAO_ENGINE_CALL wave5_bridge(const char* topic_utf8, const uint8_t* data_ptr, size_t data_size,
                                 void* user_data) {
    auto* owner = static_cast<EventSubscriptionOwner*>(user_data);
    if (owner != nullptr) {
        CallbackActivityLease subscription_lease(&owner->callback_activity);
        if (!subscription_lease)
            return SAO_ENGINE_EVENT_CONTINUE;
        PluginCallbackLease callback_lease(owner->context);
        if (callback_lease) {
            const auto callback = owner->plugin_cb;
            if (callback != nullptr) {
                (void)invoke_void_callback_barrier(
                    [&] { callback(topic_utf8, data_ptr, data_size, owner->plugin_ud); });
            }
        }
    }
    return SAO_ENGINE_EVENT_CONTINUE;
}

sao_sdk_status_t SAO_SDK_CALL event_subscribe(void* ctx_impl, const char* topic_utf8,
                                              sao_sdk_event_callback_t callback, void* user_data,
                                              sao_sdk_subscription_t* out_subscription) {
    if (out_subscription != nullptr)
        *out_subscription = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (topic_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (callback == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    auto& rt = SharedRuntime::instance();
    if (rt.event_bus == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;

    std::shared_ptr<EventSubscriptionOwner> owner;
    try {
        owner = std::make_shared<EventSubscriptionOwner>();
        owner->context = state;
        owner->plugin_cb = callback;
        owner->plugin_ud = user_data;
        preserve_event_owner(owner);
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }

    sao_engine_subscription_t bus_token = 0;
    const sao_status_t rc = sao_engine_event_bus_subscribe_wave5(
        rt.event_bus, topic_utf8, /*priority=*/0, wave5_bridge, owner.get(), &bus_token);
    if (rc != SAO_STATUS_OK) {
        owner->callback_activity.retire_and_wait();
        return static_cast<sao_sdk_status_t>(rc);
    }

    try {
        std::lock_guard<std::mutex> lk(state->mu);
        EventSubscription subscription;
        subscription.sdk_token = state->next_event_token++;
        subscription.bus_token = bus_token;
        subscription.owner = owner;
        state->event_subs.push_back(std::move(subscription));
        if (out_subscription != nullptr)
            *out_subscription = state->event_subs.back().sdk_token;
    } catch (...) {
        (void)sao_engine_event_bus_unsubscribe(rt.event_bus, bus_token);
        owner->callback_activity.retire_and_wait();
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    pause_context_api_test_point(ContextApiTestPoint::event_subscribe_registered);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL event_unsubscribe(void* ctx_impl,
                                                sao_sdk_subscription_t subscription) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;

    auto& rt = SharedRuntime::instance();
    sao_engine_subscription_t bus_token = 0;
    std::shared_ptr<EventSubscriptionOwner> owner;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = std::find_if(
            state->event_subs.begin(), state->event_subs.end(),
            [subscription](const EventSubscription& s) { return s.sdk_token == subscription; });
        if (it == state->event_subs.end())
            return SAO_SDK_ERR_NOT_FOUND;
        if (it->unregistering)
            return SAO_SDK_ERR_BUSY;
        it->unregistering = true;
        bus_token = it->bus_token;
        owner = it->owner;
    }
    pause_context_api_test_point(ContextApiTestPoint::event_unsubscribe_unlocked);
    const auto status = sao_engine_event_bus_unsubscribe(rt.event_bus, bus_token);
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_SUBSCRIPTION_GONE) {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto found = std::find_if(
            state->event_subs.begin(), state->event_subs.end(),
            [subscription](const EventSubscription& item) {
                return item.sdk_token == subscription;
            });
        if (found != state->event_subs.end())
            found->unregistering = false;
        return static_cast<sao_sdk_status_t>(status);
    }
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const auto found = std::find_if(
            state->event_subs.begin(), state->event_subs.end(),
            [subscription](const EventSubscription& item) {
                return item.sdk_token == subscription;
            });
        if (found != state->event_subs.end()) {
            found->owner.reset();
            state->event_subs.erase(found);
        }
    }
    if (owner != nullptr)
        owner->callback_activity.retire_and_wait();
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL event_publish(void* ctx_impl, const char* topic_utf8,
                                            const uint8_t* json_payload_utf8, size_t payload_len) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (topic_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    auto& rt = SharedRuntime::instance();
    if (rt.event_bus == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    const sao_status_t rc = sao_engine_event_bus_publish_wave5(rt.event_bus, topic_utf8,
                                                               json_payload_utf8, payload_len);
    if (rc != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(rc);
    return SAO_SDK_OK;
}

} // namespace

const SaoSdkEventTable* make_event_table() {
    static const SaoSdkEventTable table = {
        event_subscribe,
        event_unsubscribe,
        event_publish,
    };
    return &table;
}

void cleanup_event_subscriptions(ContextState* state) {
    std::vector<EventSubscription> subscriptions;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        subscriptions.swap(state->event_subs);
    }
    auto& runtime = SharedRuntime::instance();
    for (auto& subscription : subscriptions) {
        (void)sao_engine_event_bus_unsubscribe(runtime.event_bus, subscription.bus_token);
        subscription.retire();
        delete std::exchange(subscription.heap_owner, nullptr);
    }
}

extern "C" SAO_SDK_API void* SAO_SDK_CALL sao_sdk_test_event_snapshot_user_data(
    const SaoSdkContext* ctx, sao_sdk_subscription_t subscription) {
    ContextApiLease lease(ctx);
    if (!lease)
        return nullptr;
    auto* state = lease.state();
    std::lock_guard<std::mutex> lock(state->mu);
    const auto found = std::find_if(
        state->event_subs.begin(), state->event_subs.end(),
        [subscription](const EventSubscription& item) { return item.sdk_token == subscription; });
    return found == state->event_subs.end() || found->owner == nullptr ? nullptr
                                                                      : found->owner.get();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_invoke_event_snapshot(void* snapshot_user_data) {
    static constexpr char kTopic[] = "sdk.test.snapshot";
    (void)wave5_bridge(kTopic, nullptr, 0, snapshot_user_data);
}

} // namespace sao_sdk_internal

// ─── Public free-function wrappers ──────────────────────────────────

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_subscribe_event(
    const struct SaoSdkContext* ctx, const char* event_type_utf8, sao_sdk_event_callback_t callback,
    void* user_data, sao_sdk_subscription_t* out_handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->event == nullptr || public_context->event->subscribe == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->event->subscribe(lease.state(), event_type_utf8, callback, user_data,
                                                out_handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unsubscribe_event(const struct SaoSdkContext* ctx, sao_sdk_subscription_t handle) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->event == nullptr || public_context->event->unsubscribe == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->event->unsubscribe(lease.state(), handle);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_publish_event(const struct SaoSdkContext* ctx, const char* event_type_utf8,
                      const uint8_t* data_ptr, size_t size) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    const auto* public_context = lease.public_context();
    return sao_sdk_internal::invoke_callback_barrier([&]() -> sao_sdk_status_t {
        if (public_context->event == nullptr || public_context->event->publish == nullptr)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        return public_context->event->publish(lease.state(), event_type_utf8, data_ptr, size);
    });
}
