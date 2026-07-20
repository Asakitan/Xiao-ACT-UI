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

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "sao/core/status.h"
#include "sao/engine/event_bus.h"
#include "sao/engine/render_hook.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/ui/compositor.h"
#include "sao/ui/input_router.h"
#include "sao/ui/panel_layout.h"
#include "sao/ui/panel_sdk.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"

#include "sdk_callback_barrier.h"

namespace sao_sdk_internal {

struct ContextState;

struct ContextCallbackGate {
    std::mutex mutex;
    std::condition_variable idle;
    ContextState* state = nullptr;
    size_t active = 0;
    bool accepting = true;
};

// Per-widget entry inside a panel — the SDK stores the widget handle
// alongside its spec id so update/remove can re-target it.
struct WidgetEntry {
    sao_sdk_ui_widget_t sdk_handle = nullptr; // opaque token handed to plugin
    sao_ui_widget_handle_t ui_widget = nullptr;
    std::string widget_id; // caller-defined id (unique per panel)
    int32_t kind = 0;
    // A synthetic layout-node handle produced by
    // sao_ui_panel_update_body().  Kept so remove/update mutations know
    // which node to touch.
    sao_ui_layout_node_handle_t layout_node = nullptr;
};

// Per-panel entry.
struct PanelEntry {
    sao_sdk_ui_panel_t sdk_handle = nullptr;
    sao_ui_panel_handle_t ui_panel = nullptr;
    sao_ui_panel_body_handle_t ui_body = nullptr;
    std::string panel_id;
    // Widgets in insertion order (also z_order-sorted on add).
    std::vector<WidgetEntry> widgets;
    // Next widget-handle counter for stable synthesis.
    uint64_t next_widget_id = 1;
    // Redraw request counter — tests inspect via getters.
    uint64_t redraw_count = 0;
    sao_sdk_panel_action_callback_t legacy_action_cb = nullptr;
    void* legacy_action_user_data = nullptr;
    std::vector<sao_ui_script_canvas_handle_t> canvases;
    std::vector<sao_ui_widget_handle_t> canvas_placeholders;
    bool unregistering = false;
};

// Render-hook registration owned by this context.  Fires when the
// context's owner drives the clock via
// `sao_sdk_internal::fire_render_hook`.
struct RenderHookEntry {
    sao_sdk_hook_token_t token = 0;
    int32_t hook_point = 0;
    sao_sdk_render_hook_callback_t callback = nullptr;
    void* user_data = nullptr;
    std::string surface_id;
    float priority = 0.0f;
    void* legacy_callback = nullptr;
};

// SDK event subscription — owned by the context so unloading a plugin
// unsubscribes automatically.  Also wraps the priority-bus callback
// signature so plugins can register `sao_sdk_event_callback_t` (void return).
//
struct EventSubscriptionOwner {
    std::shared_ptr<ContextCallbackGate> callback_gate;
    sao_sdk_event_callback_t plugin_cb = nullptr;
    void* plugin_ud = nullptr;
    CallbackActivity callback_activity;
};

struct EventSubscription {
    EventSubscription() = default;

    EventSubscription(EventSubscription&& other) noexcept
                                : sdk_token(other.sdk_token), bus_token(other.bus_token), owner(std::move(other.owner)),
                                        unregistering(other.unregistering) {}

    EventSubscription& operator=(EventSubscription&& other) noexcept {
        if (this == &other)
            return *this;
        retire();
        sdk_token = other.sdk_token;
        bus_token = other.bus_token;
        owner = std::move(other.owner);
        unregistering = other.unregistering;
        return *this;
    }

    ~EventSubscription() {
        retire();
    }

    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    void retire() noexcept {
        if (owner != nullptr)
            owner->callback_activity.retire_and_wait();
    }

    sao_sdk_subscription_t sdk_token = 0;
    sao_engine_subscription_t bus_token = 0; // priority-bus token
    std::shared_ptr<EventSubscriptionOwner> owner;
    bool unregistering = false;
};

// Hotkey registration.
struct HotkeyEntry {
    sao_sdk_hotkey_id_t sdk_id = 0;
    sao_ui_hotkey_binding_t router_binding = 0;
    sao_sdk_hotkey_callback_t plugin_cb = nullptr;
    void* plugin_ud = nullptr;
    std::string binding_id;
    void* bridge = nullptr;
};

enum class CapabilityKind : uint8_t {
    render_hook,
    timer,
    hotkey,
    dialog,
    notify,
    overlay,
};

enum class ProviderConfigureOrigin : uint8_t {
    public_api,
    process_binding,
    cleanup,
};

struct CapabilityRegistration {
    CapabilityKind kind = CapabilityKind::timer;
    uint64_t sdk_token = 0;
    uint64_t provider_token = 0;
    void* bridge = nullptr;
    void (*destroy_bridge)(void*) = nullptr;
    bool unregistering = false;
};

struct ProviderBindingCandidate {
    SaoSdkProviderVTable provider{};
    bool retained = false;
};

struct NetProviderSession;

template <typename Session> class ProviderSessionSlot {
  public:
    using Pointer = std::shared_ptr<Session>;

    ProviderSessionSlot() = default;
    ProviderSessionSlot(const ProviderSessionSlot&) = delete;
    ProviderSessionSlot& operator=(const ProviderSessionSlot&) = delete;

    operator Pointer() const {
        return active_staging_slot_ == this ? staged_ : published_;
    }

    ProviderSessionSlot& operator=(Pointer value) {
        current() = std::move(value);
        return *this;
    }

    bool operator==(const Pointer& value) const {
        return current() == value;
    }

    void reset() {
        current().reset();
    }

    bool begin_staging(Pointer initial = {}) {
        if (active_staging_slot_ != nullptr)
            return false;
        staged_ = std::move(initial);
        active_staging_slot_ = this;
        return true;
    }

    Pointer take_staged() {
        if (active_staging_slot_ != this)
            return {};
        active_staging_slot_ = nullptr;
        return std::exchange(staged_, {});
    }

    Pointer published() const {
        return published_;
    }

    void publish(Pointer value) {
        published_ = std::move(value);
    }

  private:
    Pointer& current() {
        return active_staging_slot_ == this ? staged_ : published_;
    }

    const Pointer& current() const {
        return active_staging_slot_ == this ? staged_ : published_;
    }

    Pointer published_;
    Pointer staged_;
    inline static thread_local ProviderSessionSlot* active_staging_slot_ = nullptr;
};

struct MemoryProviderSession {
    ContextState* owner = nullptr;
    SaoSdkMemoryProviderVTable provider{};
    void* session = nullptr;
    std::mutex mutex;
    std::mutex operation_mutex;
    std::condition_variable idle;
    size_t active_calls = 0;
    bool accepting = true;
    bool attached = false;
    bool retained = false;
    sao_sdk_status_t cleanup_status = SAO_SDK_OK;
};

// Shared runtime bag — a single instance per process.  Contexts point
// into it for the compositor / event bus / router handles.  Created
// lazily on the first context_create; never destroyed (mirrors process
// lifetime of a real plugin host).
struct SharedRuntime {
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_engine_event_bus_handle_t event_bus = nullptr;
    sao_engine_render_hook_registry_handle_t render_registry = nullptr;
    sao_ui_input_router_deep_handle_t input_router = nullptr;
    std::mutex mu;

    static SharedRuntime& instance();
    // Idempotent; safe under concurrent context_create calls.
    void ensure_started();
};

// Per-context private state — `ctx_impl` in SaoSdkContext points here.
struct ContextState {
    ContextState() : callback_gate(std::make_shared<ContextCallbackGate>()) {}

    std::string plugin_id;
    std::string plugin_version;
    std::string base_dir;

    // Panels registered by this context, keyed by SDK opaque handle.
    std::unordered_map<sao_sdk_ui_panel_t, PanelEntry> panels;
    std::list<PanelEntry> panel_cleanup_pending;
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
    bool provider_accepting = false;
    bool provider_retained = false;
    sao_sdk_status_t provider_cleanup_status = SAO_SDK_OK;
    size_t provider_active_calls = 0;
    std::vector<CapabilityRegistration> capability_registrations;
    std::vector<SaoSdkProviderVTable> provider_release_quarantine;
    uint64_t next_capability_token = 1;
    std::mutex provider_mutex;
    std::condition_variable provider_idle;
    std::recursive_mutex provider_lifecycle_mutex;

    ProviderSessionSlot<MemoryProviderSession> memory_provider;
    std::vector<std::shared_ptr<MemoryProviderSession>> memory_quarantine;
    std::weak_ptr<MemoryProviderSession> process_memory_session;
    const void* process_memory_owner_tag = nullptr;
    uint64_t process_memory_owner_generation = 0;
    std::mutex memory_lifecycle_mutex;

    ProviderSessionSlot<NetProviderSession> net_provider;
    std::vector<std::shared_ptr<NetProviderSession>> net_quarantine;
    std::weak_ptr<NetProviderSession> process_net_session;
    const void* process_net_owner_tag = nullptr;
    uint64_t process_net_owner_generation = 0;
    std::mutex net_lifecycle_mutex;

    std::atomic_bool provider_bind_transaction = false;

    // Public struct fields for plugins to peek at.
    SaoSdkContext public_ctx{};
    SaoSdkContext* bound_public_ctx = nullptr;

    std::shared_ptr<ContextCallbackGate> callback_gate;
    std::atomic_bool destroy_quarantined = false;
    std::atomic_bool destroying = false;
    std::mutex destroy_mutex;

    // Guarded by the process-wide context registry mutex. Public API
    // entry points pin the state there before touching any context fields.
    size_t active_api_calls = 0;
    bool api_accepting = true;

    std::mutex mu;
};

inline thread_local ContextState* g_plugin_callback_owner = nullptr;
inline thread_local ContextState* g_context_destroy_owner = nullptr;
inline thread_local ContextState* g_context_api_owner = nullptr;

class ContextApiLease {
  public:
    explicit ContextApiLease(const SaoSdkContext* context) noexcept;
                explicit ContextApiLease(ContextState* state) noexcept;
    ~ContextApiLease();

    ContextApiLease(const ContextApiLease&) = delete;
    ContextApiLease& operator=(const ContextApiLease&) = delete;

    explicit operator bool() const noexcept {
        return state_ != nullptr;
    }

    sao_sdk_status_t status() const noexcept {
        return status_;
    }

    ContextState* state() const noexcept {
        return state_;
    }

    const SaoSdkContext* public_context() const noexcept {
        return state_ == nullptr ? nullptr : state_->bound_public_ctx;
    }

  private:
    ContextState* state_ = nullptr;
    ContextState* previous_owner_ = nullptr;
    sao_sdk_status_t status_ = SAO_SDK_ERR_HANDLE_INVALID;
    bool owns_lease_ = false;
};

class PluginCallbackLease {
  public:
    explicit PluginCallbackLease(ContextState* state) noexcept
        : PluginCallbackLease(state == nullptr ? nullptr : state->callback_gate) {}

    explicit PluginCallbackLease(std::shared_ptr<ContextCallbackGate> callback_gate) noexcept
        : callback_gate_(std::move(callback_gate)), previous_owner_(g_plugin_callback_owner) {
        if (callback_gate_ == nullptr)
            return;
        std::lock_guard<std::mutex> lock(callback_gate_->mutex);
        if (!callback_gate_->accepting || callback_gate_->state == nullptr)
            return;
        state_ = callback_gate_->state;
        ++callback_gate_->active;
        active_ = true;
        g_plugin_callback_owner = state_;
    }

    ~PluginCallbackLease() {
        if (!active_)
            return;
        g_plugin_callback_owner = previous_owner_;
        std::lock_guard<std::mutex> lock(callback_gate_->mutex);
        --callback_gate_->active;
        if (callback_gate_->active == 0)
            callback_gate_->idle.notify_all();
    }

    PluginCallbackLease(const PluginCallbackLease&) = delete;
    PluginCallbackLease& operator=(const PluginCallbackLease&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }

  private:
                std::shared_ptr<ContextCallbackGate> callback_gate_;
    ContextState* state_ = nullptr;
    ContextState* previous_owner_ = nullptr;
    bool active_ = false;
};

// Vtable factories — declared here, defined in sdk_context.cpp.
const SaoSdkUiTable* make_ui_table();
const SaoSdkEventTable* make_event_table();
void cleanup_event_subscriptions(ContextState* state);
const SaoSdkHotkeyTable* make_hotkey_table();
const SaoSdkMemTable* make_mem_table();
const SaoSdkNetTable* make_net_table();
const SaoSdkConfigTable* make_config_table();
const SaoSdkTtsTable* make_tts_table();
const SaoSdkBannerTable* make_banner_table();
const SaoSdkGpuHuntTable* make_gpu_hunt_table();
sao_sdk_status_t sdk_gpu_hunt_sweep_owner(ContextState* owner);
bool gpu_callback_reentered(ContextState* owner) noexcept;

void destroy_hotkey_bridge(void* bridge);
void destroy_widget_for_kind(int32_t kind, sao_ui_widget_handle_t widget);
sao_sdk_status_t cleanup_ui_panels(ContextState* state);

// Cast helper.
inline ContextState* cast_ctx(void* ctx_impl) {
    return reinterpret_cast<ContextState*>(ctx_impl);
}

// Called from the SDK's public wire helpers to drive the render hook
// clock during tests.  Fires every hook registered against the given
// point across every context alive.  Real production drives this from
// the compositor.
void fire_render_hook_test(int32_t hook_point, const SaoSdkRenderHookPayload& payload);

// Register/unregister a context with the process-wide registry so the
// test driver above can iterate every live context.
void register_context(ContextState* state);
void unregister_context(ContextState* state);
void populate_context(ContextState* state, SaoSdkContext* out_ctx, const char* plugin_version_utf8);

sao_sdk_status_t bind_provider(ContextState* state, const SaoSdkProviderVTable* provider);
sao_sdk_status_t bind_platform_provider(ContextState* state);
sao_sdk_status_t prepare_platform_provider_binding(ContextState* state,
                                                   ProviderBindingCandidate* out_candidate);
sao_sdk_status_t discard_provider_binding_candidate(
    ContextState* state, ProviderBindingCandidate* candidate);
bool platform_provider_bound(const ContextState* state);
sao_sdk_status_t provider_status(const ContextState* state);
sao_sdk_status_t provider_cleanup(ContextState* state);
bool provider_callback_reentered(ContextState* state) noexcept;
sao_sdk_status_t normalize_provider_status(sao_sdk_status_t status);

sao_sdk_status_t configure_memory_provider(ContextState* state,
                                           const SaoSdkMemoryProviderVTable* provider,
                                           ProviderConfigureOrigin origin);
sao_sdk_status_t memory_provider_status(const ContextState* state);
sao_sdk_status_t memory_attachment_status(ContextState* state);
sao_sdk_status_t memory_provider_cleanup(ContextState* state);
bool memory_callback_reentered(ContextState* state) noexcept;
sao_sdk_status_t memory_attach(ContextState* state, const SaoSdkMemoryTargetIdentity* identity);
sao_sdk_status_t memory_detach(ContextState* state);
sao_sdk_status_t memory_read(ContextState* state, uint64_t address, void* out_buffer,
                             size_t buffer_size, size_t* out_bytes_read);
sao_sdk_status_t memory_enumerate_modules(ContextState* state, SaoSdkMemoryModule* out_modules,
                                          size_t capacity, size_t element_stride,
                                          size_t* out_count);

sao_sdk_status_t configure_net_provider(ContextState* state,
                                        const SaoSdkNetProviderVTable* provider,
                                        ProviderConfigureOrigin origin);
sao_sdk_status_t net_provider_status(const ContextState* state);
sao_sdk_status_t net_provider_cleanup(ContextState* state);
bool net_callback_reentered(ContextState* state) noexcept;
sao_sdk_status_t net_capture_start(ContextState* state, const SaoSdkNetCaptureConfig* config,
                                   sao_sdk_net_packet_callback_t callback, void* user_data);
sao_sdk_status_t net_capture_stop(ContextState* state);
sao_sdk_status_t net_parse_packet(ContextState* state, const SaoSdkNetPacketView* packet,
                                  SaoSdkNetParsedResult* out_results, size_t capacity,
                                  size_t element_stride, size_t* out_count);

sao_sdk_status_t provider_tts_speak(ContextState* state, const char* text_utf8, float volume,
                                    float rate);
sao_sdk_status_t provider_tts_stop(ContextState* state);
sao_sdk_status_t provider_render_register(ContextState* state, int32_t hook_point,
                                          sao_sdk_render_hook_callback_t callback, void* user_data,
                                          sao_sdk_hook_token_t* out_token);
sao_sdk_status_t provider_render_register_ex(ContextState* state, const SaoSdkRenderHookSpec* spec,
                                             sao_sdk_render_hook_callback_t callback,
                                             void* user_data, sao_sdk_hook_token_t* out_token);
sao_sdk_status_t provider_render_unregister(ContextState* state, sao_sdk_hook_token_t token);
sao_sdk_status_t provider_request_redraw(ContextState* state, const char* surface_id_utf8);
sao_sdk_status_t provider_hotkey_register(ContextState* state, const char* binding_id_utf8,
                                          uint32_t virtual_key, uint32_t modifiers,
                                          sao_sdk_hotkey_callback_t callback, void* user_data,
                                          sao_sdk_hotkey_id_t* out_id);
sao_sdk_status_t provider_hotkey_unregister(ContextState* state, sao_sdk_hotkey_id_t id);
sao_sdk_status_t provider_overlay_set(ContextState* state, const SaoSdkOverlaySpec* spec,
                                      sao_sdk_overlay_token_t* out_overlay);
sao_sdk_status_t provider_overlay_clear(ContextState* state, sao_sdk_overlay_token_t overlay);
sao_sdk_status_t retain_gpu_provider(ContextState* state, SaoSdkProviderVTable* out_provider,
                                     std::string* out_plugin_id);

sao_sdk_status_t configure_process_memory_provider(const SaoSdkMemoryProviderVTable* provider);
sao_sdk_status_t configure_process_net_provider(const SaoSdkNetProviderVTable* provider);
sao_sdk_status_t bind_process_providers(ContextState* state);
sao_sdk_status_t bind_platform_services(ContextState* state);

inline bool plugin_callback_reentered(ContextState* state) noexcept {
    return state != nullptr && g_plugin_callback_owner == state;
}

inline bool context_destroy_on_current_thread(ContextState* state) noexcept {
    return state != nullptr && g_context_destroy_owner == state;
}
inline bool context_api_reentered(ContextState* state) noexcept {
    return state != nullptr && g_context_api_owner == state;
}
sao_sdk_status_t begin_context_shutdown(const SaoSdkContext* context, ContextState** out_state,
                                        std::unique_lock<std::mutex>* out_destroy_lock);
void cancel_context_shutdown(ContextState* state) noexcept;
void quarantine_context(ContextState* state);
void unquarantine_context(ContextState* state);

enum class ContextApiTestPoint : uint32_t {
    event_subscribe_registered = 1,
    event_unsubscribe_unlocked = 2,
    panel_registered = 3,
    panel_operation_unlocked = 4,
    gpu_runtime_toggles_leased = 5,
};

void pause_context_api_test_point(ContextApiTestPoint point);

} // namespace sao_sdk_internal
