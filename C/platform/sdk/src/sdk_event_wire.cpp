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
#include <mutex>

namespace sao_sdk_internal {
namespace {

// Bridge — invoked by the Wave 5 bus.  user_data is the
// EventSubscription* stashed at subscribe time; we look up the plugin
// callback + user data through it.
int SAO_ENGINE_CALL wave5_bridge(const char* topic_utf8, const uint8_t* data_ptr, size_t data_size,
                                 void* user_data) {
    auto* sub = static_cast<EventSubscription*>(user_data);
    if (sub != nullptr && sub->plugin_cb != nullptr) {
        PluginCallbackLease callback_lease(sub->owner);
        if (callback_lease) {
            (void)invoke_void_callback_barrier(
                [&] { sub->plugin_cb(topic_utf8, data_ptr, data_size, sub->plugin_ud); });
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

    // Heap-allocate the subscription record so the wave5 bus holds a
    // stable pointer for its lifetime.  The heap pointer is owned by
    // this context state and freed at unsubscribe / destroy.
    auto* sub = new (std::nothrow) EventSubscription();
    sub->owner = state;
    if (sub == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    sub->plugin_cb = callback;
    sub->plugin_ud = user_data;

    sao_engine_subscription_t bus_token = 0;
    const sao_status_t rc = sao_engine_event_bus_subscribe_wave5(
        rt.event_bus, topic_utf8, /*priority=*/0, wave5_bridge, sub, &bus_token);
    if (rc != SAO_STATUS_OK) {
        delete sub;
        return static_cast<sao_sdk_status_t>(rc);
    }
    sub->bus_token = bus_token;
    sub->heap_owner = sub; // self-referential: the copy stored in
                           // event_subs carries this pointer so
                           // unsubscribe/destroy can `delete` it.

    {
        std::lock_guard<std::mutex> lk(state->mu);
        sub->sdk_token = state->next_event_token++;
        state->event_subs.push_back(*sub);
    }

    if (out_subscription != nullptr)
        *out_subscription = sub->sdk_token;
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
    EventSubscription* heap_owner = nullptr;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = std::find_if(
            state->event_subs.begin(), state->event_subs.end(),
            [subscription](const EventSubscription& s) { return s.sdk_token == subscription; });
        if (it == state->event_subs.end())
            return SAO_SDK_ERR_NOT_FOUND;
        bus_token = it->bus_token;
        heap_owner = it->heap_owner;
    }
    // Unsubscribe first so no in-flight publish can grab the heap ptr
    // after we free it.  The wave5 bus swept subscribers already saw a
    // consistent snapshot; new publishes won't include this sub.
    const auto status = sao_engine_event_bus_unsubscribe(rt.event_bus, bus_token);
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_SUBSCRIPTION_GONE)
        return static_cast<sao_sdk_status_t>(status);
    {
        std::unique_lock<std::mutex> callback_lock(state->callback_mutex);
        state->callback_idle.wait(callback_lock,
                                  [state] { return state->active_plugin_callbacks == 0; });
    }
    {
        std::lock_guard<std::mutex> lk(state->mu);
        std::erase_if(state->event_subs, [subscription](const EventSubscription& entry) {
            return entry.sdk_token == subscription;
        });
    }
    delete heap_owner;
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

} // namespace sao_sdk_internal

// ─── Public free-function wrappers ──────────────────────────────────

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_subscribe_event(
    const struct SaoSdkContext* ctx, const char* event_type_utf8, sao_sdk_event_callback_t callback,
    void* user_data, sao_sdk_subscription_t* out_handle) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->event->subscribe(ctx->ctx_impl, event_type_utf8, callback, user_data, out_handle);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unsubscribe_event(const struct SaoSdkContext* ctx, sao_sdk_subscription_t handle) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->event->unsubscribe(ctx->ctx_impl, handle);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_publish_event(const struct SaoSdkContext* ctx, const char* event_type_utf8,
                      const uint8_t* data_ptr, size_t size) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return ctx->event->publish(ctx->ctx_impl, event_type_utf8, data_ptr, size);
}
