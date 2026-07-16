// SAO Auto — SDK-side private state.
//
// The context handed to plugins is public (`SaoSdkContext`); this
// header describes the *SDK's* private per-context state that backs
// `ctx_impl`.  It is NOT part of the plugin ABI — plugins never see
// this file.
//
// The SDK owns compositor / event_bus / input_router / render_hook
// registrations on behalf of each plugin so a crashed plugin can be
// unloaded cleanly (`sao_sdk_context_destroy` sweeps every registration
// this instance ever produced).

#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "sao/sdk/sao_sdk.h"
#include "sao/core/status.h"
#include "sao/engine/event_bus.h"
#include "sao/engine/render_hook.h"
#include "sao/ui/compositor.h"
#include "sao/ui/input_router.h"
#include "sao/ui/panel_sdk.h"
#include "sao/ui/panel_layout.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"

namespace sao_sdk_internal {

// Per-widget entry inside a panel — the SDK stores the widget handle
// alongside its spec id so update/remove can re-target it.
struct WidgetEntry {
    sao_sdk_ui_widget_t sdk_handle = nullptr;   // opaque token handed to plugin
    sao_ui_widget_handle_t ui_widget = nullptr;
    std::string         widget_id;              // caller-defined id (unique per panel)
    int32_t             kind = 0;
    // A synthetic layout-node handle produced by
    // sao_ui_panel_update_body().  Kept so remove/update mutations know
    // which node to touch.
    sao_ui_layout_node_handle_t layout_node = nullptr;
};

// Per-panel entry.
struct PanelEntry {
    sao_sdk_ui_panel_t         sdk_handle = nullptr;
    sao_ui_panel_handle_t      ui_panel   = nullptr;
    sao_ui_panel_body_handle_t ui_body    = nullptr;
    std::string                panel_id;
    // Widgets in insertion order (also z_order-sorted on add).
    std::vector<WidgetEntry>   widgets;
    // Next widget-handle counter for stable synthesis.
    uint64_t                   next_widget_id = 1;
    // Redraw request counter — tests inspect via getters.
    uint64_t                   redraw_count = 0;
    sao_sdk_panel_action_callback_t legacy_action_cb = nullptr;
    void*                      legacy_action_user_data = nullptr;
    std::vector<sao_ui_script_canvas_handle_t> canvases;
};

// Render-hook registration owned by this context.  Fires when the
// context's owner drives the clock via
// `sao_sdk_internal::fire_render_hook`.
struct RenderHookEntry {
    sao_sdk_hook_token_t           token = 0;
    int32_t                        hook_point = 0;
    sao_sdk_render_hook_callback_t callback = nullptr;
    void*                          user_data = nullptr;
    std::string                    surface_id;
    float                          priority = 0.0f;
    void*                          legacy_callback = nullptr;
};

// Wave7 SDK subscription — owned by the context so unloading a plugin
// unsubscribes automatically.  Also wraps the wave5 callback signature
// so plugins can register `sao_sdk_event_callback_t` (void return).
//
// The bus is handed a stable heap pointer as user_data (needed for the
// bridge lookup); ``heap_owner`` is that pointer, which must be freed
// with `delete` at unsubscribe or destroy.
struct EventSubscription {
    sao_sdk_subscription_t     sdk_token = 0;
    sao_engine_subscription_t  bus_token = 0;   // wave5 token
    sao_sdk_event_callback_t   plugin_cb = nullptr;
    void*                      plugin_ud = nullptr;
    EventSubscription*         heap_owner = nullptr;   // matches the wave5 user_data slot
};

// Hotkey registration.
struct HotkeyEntry {
    sao_sdk_hotkey_id_t         sdk_id = 0;
    sao_ui_hotkey_binding_t     router_binding = 0;
    sao_sdk_hotkey_callback_t   plugin_cb = nullptr;
    void*                       plugin_ud = nullptr;
    std::string                 binding_id;
    void*                       bridge = nullptr;
};

enum class CapabilityKind : uint8_t {
    render_hook,
    timer,
    hotkey,
    dialog,
    notify,
    overlay,
};

struct CapabilityRegistration {
    CapabilityKind kind = CapabilityKind::timer;
    uint64_t sdk_token = 0;
    uint64_t provider_token = 0;
    void* bridge = nullptr;
    void (*destroy_bridge)(void*) = nullptr;
};

// Shared runtime bag — a single instance per process.  Contexts point
// into it for the compositor / event bus / router handles.  Created
// lazily on the first context_create; never destroyed (mirrors process
// lifetime of a real plugin host).
struct SharedRuntime {
    sao_ui_compositor_handle_t         compositor = nullptr;
    sao_engine_event_bus_handle_t      event_bus  = nullptr;
    sao_engine_render_hook_registry_handle_t render_registry = nullptr;
    sao_ui_input_router_deep_handle_t  input_router = nullptr;
    std::mutex                         mu;

    static SharedRuntime& instance();
    // Idempotent; safe under concurrent context_create calls.
    void ensure_started();
};

// Per-context private state — `ctx_impl` in SaoSdkContext points here.
struct ContextState {
    std::string plugin_id;
    std::string plugin_version;
    std::string base_dir;

    // Panels registered by this context, keyed by SDK opaque handle.
    std::unordered_map<sao_sdk_ui_panel_t, PanelEntry> panels;
    std::unordered_map<std::string, sao_sdk_ui_panel_t> overlays;

    // Render hooks owned by this context.
    std::vector<RenderHookEntry> render_hooks;
    uint64_t next_render_hook_token = 1;

    // Event subs owned by this context.
    std::vector<EventSubscription> event_subs;
    uint64_t next_event_token = 1;

    // Hotkeys owned by this context.
    std::vector<HotkeyEntry> hotkeys;
    uint64_t next_hotkey_id = 1;

    using ConfigValue = std::variant<bool, int64_t, double, std::string>;
    std::unordered_map<std::string, ConfigValue> config_values;

    std::vector<uint64_t> banner_ids;

    SaoSdkProviderVTable provider{};
    bool provider_bound = false;
    std::vector<CapabilityRegistration> capability_registrations;
    uint64_t next_capability_token = 1;

    // Public struct fields for plugins to peek at.
    SaoSdkContext public_ctx{};
    SaoSdkContext* bound_public_ctx = nullptr;

    std::mutex mu;
};

// Vtable factories — declared here, defined in sdk_context.cpp.
const SaoSdkUiTable*     make_ui_table();
const SaoSdkEventTable*  make_event_table();
const SaoSdkHotkeyTable* make_hotkey_table();
const SaoSdkMemTable*    make_mem_table_fail_closed();
const SaoSdkNetTable*    make_net_table_fail_closed();
const SaoSdkConfigTable* make_config_table();
const SaoSdkTtsTable*    make_tts_table();
const SaoSdkBannerTable* make_banner_table();

void destroy_hotkey_bridge(void* bridge);
void destroy_widget_for_kind(int32_t kind, sao_ui_widget_handle_t widget);

// Cast helper.
inline ContextState* cast_ctx(void* ctx_impl) {
    return reinterpret_cast<ContextState*>(ctx_impl);
}

// Called from the SDK's public wire helpers to drive the render hook
// clock during tests.  Fires every hook registered against the given
// point across every context alive.  Real production drives this from
// the compositor.
void fire_render_hook_test(int32_t hook_point,
                           const SaoSdkRenderHookPayload& payload);

// Register/unregister a context with the process-wide registry so the
// test driver above can iterate every live context.
void register_context(ContextState* state);
void unregister_context(ContextState* state);
void populate_context(ContextState* state, SaoSdkContext* out_ctx,
                      const char* plugin_version_utf8);

sao_sdk_status_t bind_provider(ContextState* state,
                               const SaoSdkProviderVTable* provider);
sao_sdk_status_t bind_platform_provider(ContextState* state);
sao_sdk_status_t provider_status(const ContextState* state);
void provider_cleanup(ContextState* state);

sao_sdk_status_t provider_tts_speak(ContextState* state,
                                    const char* text_utf8,
                                    float volume,
                                    float rate);
sao_sdk_status_t provider_tts_stop(ContextState* state);
sao_sdk_status_t provider_render_register(
    ContextState* state, int32_t hook_point,
    sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_token);
sao_sdk_status_t provider_render_register_ex(
    ContextState* state, const SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_token);
sao_sdk_status_t provider_render_unregister(ContextState* state,
                                            sao_sdk_hook_token_t token);
sao_sdk_status_t provider_request_redraw(ContextState* state,
                                         const char* surface_id_utf8);
sao_sdk_status_t provider_hotkey_register(
    ContextState* state, const char* binding_id_utf8,
    uint32_t virtual_key, uint32_t modifiers,
    sao_sdk_hotkey_callback_t callback, void* user_data,
    sao_sdk_hotkey_id_t* out_id);
sao_sdk_status_t provider_hotkey_unregister(ContextState* state,
                                            sao_sdk_hotkey_id_t id);
sao_sdk_status_t provider_overlay_set(
    ContextState* state, const SaoSdkOverlaySpec* spec,
    sao_sdk_overlay_token_t* out_overlay);
sao_sdk_status_t provider_overlay_clear(ContextState* state,
                                        sao_sdk_overlay_token_t overlay);

}  // namespace sao_sdk_internal
