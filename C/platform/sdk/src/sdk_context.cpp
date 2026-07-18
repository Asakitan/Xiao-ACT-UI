// SAO Auto — Wave 7 SDK context lifecycle + shared runtime.
//
// Wires:
//   * sao_sdk_context_create / _destroy — allocate a per-plugin state
//     bag, populate the vtables, hand back a SaoSdkContext.
//   * sao_sdk_context_get_plugin_id / _get_base_dir — trivial accessors.
//   * SharedRuntime::instance() — process-wide compositor / event bus /
//     input router; created lazily on the first context_create.
//   * fire_render_hook_test — used by the demo test driver to run the
//     UI clock across every live context without needing a real
//     compositor tick.
//
// Only lightweight compat state lives here — the vtable factories that
// forward into the platform live in sdk_ui_wire.cpp / sdk_event_wire.cpp
// / sdk_hotkey_wire.cpp.
//
// PLATFORM DOES NOT IMPORT PLUGINS — this file speaks only to
// sao::core / sao::engine / sao::ui.  A plugin sees SaoSdkContext and
// the wire helpers; nothing here.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sao/engine/render_hook.h"

namespace sao_sdk_internal {

// ─── Shared runtime ──────────────────────────────────────────────────

SharedRuntime& SharedRuntime::instance() {
    static SharedRuntime rt;
    return rt;
}

void SharedRuntime::ensure_started() {
    std::lock_guard<std::mutex> guard(mu);
    if (compositor == nullptr) {
        SaoCompositorConfig cfg{};
        cfg.enable_temporal_union = true;
        cfg.enable_rgn_cache      = true;
        (void)sao_ui_compositor_create(nullptr, &cfg, &compositor);
    }
    if (event_bus == nullptr) {
        (void)sao_engine_event_bus_create_wave5(&event_bus);
    }
    if (render_registry == nullptr) {
        (void)sao_engine_render_hook_registry_create(&render_registry);
    }
    if (input_router == nullptr) {
        (void)sao_ui_input_router_deep_create(compositor, &input_router);
    }
}

// ─── Context registry (process-wide) ─────────────────────────────────

namespace {
std::mutex g_ctx_registry_mu;
std::unordered_set<ContextState*> g_ctx_registry;
}  // namespace

void register_context(ContextState* state) {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    g_ctx_registry.insert(state);
}

void unregister_context(ContextState* state) {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    g_ctx_registry.erase(state);
}

size_t live_context_count() {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    return g_ctx_registry.size();
}

// Snapshot the alive contexts (copy).  Called from
// fire_render_hook_test — we drop the lock before firing user code
// (callbacks must never run while we hold the registry mutex).
static std::vector<ContextState*> snapshot_contexts() {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    return std::vector<ContextState*>(g_ctx_registry.begin(),
                                      g_ctx_registry.end());
}

void fire_render_hook_test(int32_t hook_point,
                           const SaoSdkRenderHookPayload& payload) {
    auto ctxs = snapshot_contexts();
    for (auto* state : ctxs) {
        // Snapshot the hook list under the per-context lock, then fire
        // outside so callbacks that re-enter the SDK don't deadlock.
        std::vector<RenderHookEntry> hooks_copy;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            hooks_copy = state->render_hooks;
        }
        for (const auto& hook : hooks_copy) {
            if (hook.hook_point == hook_point && hook.callback != nullptr) {
                hook.callback(hook_point, &payload, hook.user_data);
            }
        }
    }
}

// ─── Capability sub-tables ──────────────────────────────────────────

namespace {

// Process selection and packet capture ownership are launcher concerns.
// Until the launcher binds either backend, reject these calls explicitly so
// plugins never mistake a null/default result for successful I/O.
sao_sdk_status_t SAO_SDK_CALL mem_read(void*, uint64_t, void*, size_t,
                                        size_t* out_bytes_read) {
    if (out_bytes_read != nullptr) *out_bytes_read = 0;
    return SAO_SDK_ERR_NOT_INITIALIZED;
}
sao_sdk_status_t SAO_SDK_CALL mem_read_u32(void*, uint64_t, uint32_t* out_value) {
    if (out_value != nullptr) *out_value = 0;
    return SAO_SDK_ERR_NOT_INITIALIZED;
}
sao_sdk_status_t SAO_SDK_CALL mem_read_u64(void*, uint64_t, uint64_t* out_value) {
    if (out_value != nullptr) *out_value = 0;
    return SAO_SDK_ERR_NOT_INITIALIZED;
}
sao_sdk_status_t SAO_SDK_CALL mem_read_ptr_chain(
    void*, uint64_t, const int32_t*, size_t, uint64_t* out_final_address) {
    if (out_final_address != nullptr) *out_final_address = 0;
    return SAO_SDK_ERR_NOT_INITIALIZED;
}
sao_sdk_status_t SAO_SDK_CALL mem_module_base(void*, const char*, uint64_t* out_base) {
    if (out_base != nullptr) *out_base = 0;
    return SAO_SDK_ERR_NOT_INITIALIZED;
}
const SaoSdkMemTable kMemTable = {
    mem_read, mem_read_u32, mem_read_u64, mem_read_ptr_chain, mem_module_base
};

// The net callback must only become available after an owned packet pipeline
// is explicitly attached by the launcher.
sao_sdk_status_t SAO_SDK_CALL net_set_frame(
    void*, SaoSdkNetTable::frame_callback_t, void*) {
    return SAO_SDK_ERR_NOT_INITIALIZED;
}
const SaoSdkNetTable kNetTable = { net_set_frame };

template <typename T>
sao_sdk_status_t cfg_get(void* ctx_impl, const char* key_utf8, T* out_value) {
    if (out_value != nullptr) *out_value = T{};
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || key_utf8[0] == '\0' || out_value == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->config_values.find(key_utf8);
    if (it == state->config_values.end()) return SAO_SDK_ERR_NOT_FOUND;
    const auto* value = std::get_if<T>(&it->second);
    if (value == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_value = *value;
    return SAO_SDK_OK;
}

template <typename T>
sao_sdk_status_t cfg_set(void* ctx_impl, const char* key_utf8, T value) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || key_utf8[0] == '\0') return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(state->mu);
    state->config_values[std::string(key_utf8)] = std::move(value);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL cfg_get_bool(void* ctx_impl, const char* key, bool* value) {
    return cfg_get(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_int(void* ctx_impl, const char* key, int64_t* value) {
    return cfg_get(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_double(void* ctx_impl, const char* key, double* value) {
    return cfg_get(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_string(void* ctx_impl, const char* key,
                                              char* out_buffer, size_t buffer_len,
                                              size_t* out_bytes_needed) {
    if (out_bytes_needed != nullptr) *out_bytes_needed = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (key == nullptr || key[0] == '\0') return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->config_values.find(key);
    if (it == state->config_values.end()) return SAO_SDK_ERR_NOT_FOUND;
    const auto* value = std::get_if<std::string>(&it->second);
    if (value == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    const size_t required = value->size() + 1;
    if (out_bytes_needed != nullptr) *out_bytes_needed = required;
    if (out_buffer == nullptr || buffer_len < required) return SAO_SDK_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out_buffer, value->c_str(), required);
    return SAO_SDK_OK;
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_bool(void* ctx_impl, const char* key, bool value) {
    return cfg_set(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_int(void* ctx_impl, const char* key, int64_t value) {
    return cfg_set(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_double(void* ctx_impl, const char* key, double value) {
    return cfg_set(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_string(void* ctx_impl, const char* key, const char* value) {
    if (value == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return cfg_set(ctx_impl, key, std::string(value));
}
const SaoSdkConfigTable kConfigTable = {
    cfg_get_bool, cfg_get_int, cfg_get_double, cfg_get_string,
    cfg_set_bool, cfg_set_int, cfg_set_double, cfg_set_string,
};

sao_sdk_status_t SAO_SDK_CALL tts_speak(void* ctx_impl, const char* text_utf8,
                                         float volume, float rate) {
    return provider_tts_speak(cast_ctx(ctx_impl), text_utf8, volume, rate);
}
sao_sdk_status_t SAO_SDK_CALL tts_stop(void* ctx_impl) {
    return provider_tts_stop(cast_ctx(ctx_impl));
}
const SaoSdkTtsTable kTtsTable = { tts_speak, tts_stop };

sao_sdk_status_t SAO_SDK_CALL banner_show(void* ctx_impl, const char* text_utf8,
                                           uint32_t duration_ms, uint32_t argb_color) {
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (text_utf8 == nullptr || state->bound_public_ctx == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    SaoSdkNotifySpec spec{};
    spec.text_utf8 = text_utf8;
    spec.duration_ms = duration_ms;
    spec.argb_color = argb_color;
    sao_sdk_notify_token_t token = 0;
    return sao_sdk_notify_show(state->bound_public_ctx, &spec, &token);
}
const SaoSdkBannerTable kBannerTable = { banner_show };

}  // namespace

const SaoSdkMemTable*    make_mem_table_fail_closed() { return &kMemTable; }
const SaoSdkNetTable*    make_net_table_fail_closed() { return &kNetTable; }
const SaoSdkConfigTable* make_config_table()          { return &kConfigTable; }
const SaoSdkTtsTable*    make_tts_table()             { return &kTtsTable; }
const SaoSdkBannerTable* make_banner_table()          { return &kBannerTable; }

void populate_context(ContextState* state, SaoSdkContext* out_ctx,
                      const char* plugin_version_utf8) {
    std::memset(out_ctx, 0, sizeof(*out_ctx));
    state->plugin_version = plugin_version_utf8 == nullptr ? "" : plugin_version_utf8;
    state->bound_public_ctx = out_ctx;
    out_ctx->abi_version         = SAO_SDK_ABI_VERSION;
    out_ctx->ctx_impl            = state;
    out_ctx->plugin_id_utf8      = state->plugin_id.c_str();
    out_ctx->plugin_version_utf8 = state->plugin_version.c_str();
    out_ctx->ui       = make_ui_table();
    out_ctx->event    = make_event_table();
    out_ctx->mem      = make_mem_table_fail_closed();
    out_ctx->net      = make_net_table_fail_closed();
    out_ctx->config   = make_config_table();
    out_ctx->hotkey   = make_hotkey_table();
    out_ctx->tts      = make_tts_table();
    out_ctx->banner   = make_banner_table();
    out_ctx->gpu_hunt = make_gpu_hunt_table();
}

}  // namespace sao_sdk_internal

// ─── Context lifecycle exports ───────────────────────────────────────

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_create(
    const char* base_dir_utf8,
    const char* plugin_id_utf8,
    struct SaoSdkContext** out_ctx) {
    if (out_ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_ctx = nullptr;
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    // Bring up the shared runtime lazily so tests don't need a fixture.
    sao_sdk_internal::SharedRuntime::instance().ensure_started();

    auto* state = new (std::nothrow) sao_sdk_internal::ContextState();
    if (state == nullptr) return SAO_SDK_ERR_NOT_INITIALIZED;
    state->plugin_id = plugin_id_utf8;
    state->base_dir  = (base_dir_utf8 == nullptr) ? "" : base_dir_utf8;

    SaoSdkContext& pub = state->public_ctx;
    sao_sdk_internal::populate_context(state, &pub, "0.0.0");

    sao_sdk_internal::register_context(state);
    *out_ctx = &state->public_ctx;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_context_destroy(
    struct SaoSdkContext* ctx) {
    if (ctx == nullptr) return;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return;
    const bool caller_owns_context = ctx != &state->public_ctx;

    // Provider-owned registrations are swept in exact reverse registration
    // order before local event/panel state is released.
    sao_sdk_internal::provider_cleanup(state);
    sao_sdk_internal::sdk_gpu_hunt_sweep_owner(state);

    auto& rt = sao_sdk_internal::SharedRuntime::instance();
    {
        std::lock_guard<std::mutex> lk(state->mu);
        for (const auto& sub : state->event_subs) {
            (void)sao_engine_event_bus_unsubscribe(rt.event_bus, sub.bus_token);
            delete sub.heap_owner;
        }
        state->event_subs.clear();

        for (auto& kv : state->panels) {
            auto& pe = kv.second;
            for (const auto canvas : pe.canvases) {
                sao_ui_script_canvas_destroy(canvas);
            }
            for (const auto& widget : pe.widgets) {
                if (widget.ui_widget != nullptr) {
                    sao_sdk_internal::destroy_widget_for_kind(widget.kind,
                                                              widget.ui_widget);
                }
            }
            if (pe.ui_panel != nullptr) {
                (void)sao_ui_panel_unregister(pe.ui_panel);
            }
        }
        state->panels.clear();
        state->overlays.clear();

    }

    sao_sdk_internal::unregister_context(state);
    if (caller_owns_context) {
        std::memset(ctx, 0, sizeof(*ctx));
    }
    delete state;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_get_plugin_id(
    const struct SaoSdkContext* ctx,
    const char** out_plugin_id_utf8) {
    if (ctx == nullptr || out_plugin_id_utf8 == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    *out_plugin_id_utf8 = ctx->plugin_id_utf8;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_get_base_dir(
    const struct SaoSdkContext* ctx,
    const char** out_base_dir_utf8) {
    if (ctx == nullptr || out_base_dir_utf8 == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    *out_base_dir_utf8 = state->base_dir.c_str();
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_live_context_count(void) {
    return sao_sdk_internal::live_context_count();
}
