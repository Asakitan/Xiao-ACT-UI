// SAO Auto — Wave 7 SDK hotkey wire.
//
// Forwards `SaoSdkContext::hotkey->*` into the platform input router
// (`sao_ui_input_router_deep_*`).  Plugins never see the deep router;
// they only see the SDK vtable.
//
// The router callback signature is
//   `void(binding_id, event_ptr, user_data)`
// while the SDK plugin callback is
//   `void(hotkey_id, user_data)`.
// A per-registration bridge stashes the SDK hotkey id so the plugin
// callback receives its own id back.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace sao_sdk_internal {
namespace {

// Bridge — user_data at the router side is a HotkeyBridge* stashed on
// the heap for the duration of the binding.
struct HotkeyBridge {
    sao_sdk_hotkey_id_t sdk_id = 0;
    sao_sdk_hotkey_callback_t plugin_cb = nullptr;
    void* plugin_ud = nullptr;
};

void destroy_hotkey_bridge_impl(void* bridge) {
    delete static_cast<HotkeyBridge*>(bridge);
}

void SAO_UI_CALL router_hotkey_bridge(const char* /*binding_id_utf8*/,
                                      const SaoUiInputEvent* /*event*/, void* user_data) {
    auto* bridge = static_cast<HotkeyBridge*>(user_data);
    if (bridge != nullptr && bridge->plugin_cb != nullptr) {
        bridge->plugin_cb(bridge->sdk_id, bridge->plugin_ud);
    }
}

sao_sdk_status_t SAO_SDK_CALL hotkey_register(void* ctx_impl, const char* hotkey_name_utf8,
                                              uint32_t virtual_key, uint32_t modifier_mask,
                                              sao_sdk_hotkey_callback_t callback, void* user_data,
                                              sao_sdk_hotkey_id_t* out_id) {
    auto* state = cast_ctx(ctx_impl);
    return provider_hotkey_register(state, hotkey_name_utf8, virtual_key, modifier_mask, callback,
                                    user_data, out_id);
}

sao_sdk_status_t SAO_SDK_CALL hotkey_unregister(void* ctx_impl, sao_sdk_hotkey_id_t id) {
    return provider_hotkey_unregister(cast_ctx(ctx_impl), id);
}

} // namespace

void destroy_hotkey_bridge(void* bridge) {
    destroy_hotkey_bridge_impl(bridge);
}

const SaoSdkHotkeyTable* make_hotkey_table() {
    static const SaoSdkHotkeyTable table = {
        hotkey_register,
        hotkey_unregister,
    };
    return &table;
}

// ─── Test-only bridge introspection ─────────────────────────────────
//
// The router's dispatch path is exercised by feeding a synthetic
// SaoUiInputEvent through sao_ui_input_router_route_event — but the
// tests only need to verify the bridge would fire.  This helper
// simulates a router match by looking up the plugin's callback in the
// state table and invoking it.

size_t fire_hotkey_by_binding_id(ContextState* state, const char* binding_id_utf8) {
    if (state == nullptr || binding_id_utf8 == nullptr)
        return 0;
    std::vector<HotkeyEntry> snap;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        for (const auto& h : state->hotkeys) {
            if (h.binding_id == binding_id_utf8)
                snap.push_back(h);
        }
    }
    for (const auto& h : snap) {
        if (h.plugin_cb != nullptr) {
            PluginCallbackLease callback_lease(state);
            if (callback_lease) {
                (void)invoke_void_callback_barrier([&h] { h.plugin_cb(h.sdk_id, h.plugin_ud); });
            }
        }
    }
    return snap.size();
}

} // namespace sao_sdk_internal

// ─── Public free-function wrappers ──────────────────────────────────

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_register_hotkey(
    const struct SaoSdkContext* ctx, const struct SaoSdkHotkeySpec* spec,
    sao_sdk_hotkey_callback_t callback, void* user_data, sao_sdk_hotkey_id_t* out_handle) {
    if (ctx == nullptr || spec == nullptr || ctx->hotkey == nullptr ||
        ctx->hotkey->register_hotkey == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    return ctx->hotkey->register_hotkey(ctx->ctx_impl, spec->binding_id_utf8, spec->virtual_key,
                                        spec->modifiers, callback, user_data, out_handle);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_unregister_hotkey(const struct SaoSdkContext* ctx, sao_sdk_hotkey_id_t handle) {
    if (ctx == nullptr || ctx->hotkey == nullptr || ctx->hotkey->unregister_hotkey == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    return ctx->hotkey->unregister_hotkey(ctx->ctx_impl, handle);
}

// ─── Test-only observability ────────────────────────────────────────

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_hotkey_count(const struct SaoSdkContext* ctx) {
    if (ctx == nullptr)
        return 0;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return 0;
    std::lock_guard<std::mutex> lk(state->mu);
    return state->hotkeys.size();
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_hotkey_fire_by_id(const struct SaoSdkContext* ctx, const char* binding_id_utf8) {
    if (ctx == nullptr)
        return 0;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return 0;
    return sao_sdk_internal::fire_hotkey_by_binding_id(state, binding_id_utf8);
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_route_hotkey(uint32_t virtual_key,
                                                                     uint32_t modifiers) {
    auto& rt = sao_sdk_internal::SharedRuntime::instance();
    if (rt.input_router == nullptr)
        return 0;
    SaoUiInputEvent event{};
    event.kind = SAO_UI_INPUT_KEY_DOWN;
    event.virtual_key = virtual_key;
    event.modifiers = modifiers;
    sao_ui_hotkey_binding_t binding = 0;
    const sao_status_t rc = sao_ui_input_router_match_hotkey(rt.input_router, &event, &binding);
    return rc == SAO_STATUS_OK && binding != 0 ? 1u : 0u;
}
