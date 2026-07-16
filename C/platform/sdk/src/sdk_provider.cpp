#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "sao/core/thread.h"
#include "sao/ui/alerts.h"
#include "sao/ui/dialog.h"

namespace sao_sdk_internal {
namespace {

constexpr uint32_t kProviderMinimumSize =
    static_cast<uint32_t>(offsetof(SaoSdkProviderVTable, retain));

struct TimerCallbackBridge {
    sao_sdk_timer_token_t sdk_token = 0;
    sao_sdk_timer_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct HotkeyCallbackBridge {
    sao_sdk_hotkey_id_t sdk_token = 0;
    sao_sdk_hotkey_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct DialogCallbackBridge {
    sao_sdk_dialog_token_t sdk_token = 0;
    sao_sdk_dialog_callback_t callback = nullptr;
    void* user_data = nullptr;
};

void destroy_timer_bridge(void* value) {
    delete static_cast<TimerCallbackBridge*>(value);
}

void destroy_hotkey_provider_bridge(void* value) {
    delete static_cast<HotkeyCallbackBridge*>(value);
}

void destroy_dialog_bridge(void* value) {
    delete static_cast<DialogCallbackBridge*>(value);
}

void SAO_SDK_CALL timer_callback_bridge(sao_sdk_timer_token_t,
                                        void* user_data) {
    auto* bridge = static_cast<TimerCallbackBridge*>(user_data);
    if (bridge != nullptr && bridge->callback != nullptr) {
        bridge->callback(bridge->sdk_token, bridge->user_data);
    }
}

void SAO_SDK_CALL hotkey_callback_bridge(sao_sdk_hotkey_id_t,
                                         void* user_data) {
    auto* bridge = static_cast<HotkeyCallbackBridge*>(user_data);
    if (bridge != nullptr && bridge->callback != nullptr) {
        bridge->callback(bridge->sdk_token, bridge->user_data);
    }
}

void SAO_SDK_CALL dialog_callback_bridge(sao_sdk_dialog_token_t,
                                         int32_t pressed_button,
                                         const char* input_text_utf8,
                                         size_t input_text_len,
                                         void* user_data) {
    auto* bridge = static_cast<DialogCallbackBridge*>(user_data);
    if (bridge != nullptr && bridge->callback != nullptr) {
        bridge->callback(bridge->sdk_token, pressed_button,
                         input_text_utf8, input_text_len, bridge->user_data);
    }
}

uint64_t allocate_capability_token(ContextState* state) {
    std::lock_guard<std::mutex> lock(state->mu);
    return state->next_capability_token++;
}

SaoSdkProviderVTable provider_snapshot(ContextState* state, bool* bound) {
    std::lock_guard<std::mutex> lock(state->mu);
    if (bound != nullptr) *bound = state->provider_bound;
    return state->provider;
}

sao_sdk_status_t add_registration(ContextState* state,
                                  CapabilityRegistration registration) {
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        state->capability_registrations.push_back(registration);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

bool take_registration(ContextState* state, CapabilityKind kind,
                       uint64_t sdk_token,
                       CapabilityRegistration* out_registration,
                       SaoSdkProviderVTable* out_provider) {
    std::lock_guard<std::mutex> lock(state->mu);
    const auto found = std::find_if(
        state->capability_registrations.begin(),
        state->capability_registrations.end(),
        [kind, sdk_token](const CapabilityRegistration& registration) {
            return registration.kind == kind &&
                   registration.sdk_token == sdk_token;
        });
    if (found == state->capability_registrations.end()) return false;
    *out_registration = *found;
    *out_provider = state->provider;
    state->capability_registrations.erase(found);
    return true;
}

sao_sdk_status_t unregister_provider_token(
    const SaoSdkProviderVTable& provider,
    const CapabilityRegistration& registration) {
    sao_sdk_status_t status = SAO_SDK_ERR_UNSUPPORTED;
    switch (registration.kind) {
        case CapabilityKind::render_hook:
            if (provider.unregister_render_hook != nullptr) {
                status = provider.unregister_render_hook(
                    provider.user_data, registration.provider_token);
            }
            break;
        case CapabilityKind::timer:
            if (provider.unregister_timer != nullptr) {
                status = provider.unregister_timer(
                    provider.user_data, registration.provider_token);
            }
            break;
        case CapabilityKind::hotkey:
            if (provider.unregister_hotkey != nullptr) {
                status = provider.unregister_hotkey(
                    provider.user_data, registration.provider_token);
            }
            break;
        case CapabilityKind::dialog:
            if (provider.dismiss_dialog != nullptr) {
                status = provider.dismiss_dialog(
                    provider.user_data, registration.provider_token);
            }
            break;
        case CapabilityKind::notify:
            if (provider.dismiss_notify != nullptr) {
                status = provider.dismiss_notify(
                    provider.user_data, registration.provider_token);
            }
            break;
        case CapabilityKind::overlay:
            if (provider.clear_overlay != nullptr) {
                status = provider.clear_overlay(
                    provider.user_data, registration.provider_token);
            }
            break;
    }
    if (registration.destroy_bridge != nullptr) {
        registration.destroy_bridge(registration.bridge);
    }
    return status;
}

sao_sdk_status_t normalize_unregister_status(sao_sdk_status_t status) {
    if (status == SAO_SDK_OK || status == SAO_SDK_ERR_NOT_FOUND ||
        status == SAO_STATUS_ERR_SUBSCRIPTION_GONE) {
        return SAO_SDK_OK;
    }
    return status;
}

// Platform provider --------------------------------------------------

struct PlatformTimerEntry {
    sao_core_timer_handle_t timer = nullptr;
    sao_sdk_timer_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformRenderBridge {
    sao_sdk_render_hook_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformHotkeyEntry {
    sao_ui_hotkey_binding_t binding = 0;
    sao_sdk_hotkey_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformDialogEntry {
    sao_ui_dialog_handle_t dialog = nullptr;
    sao_sdk_dialog_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformOverlayEntry {
    std::string plugin_id;
    std::string surface_id;
};

struct PlatformProviderState {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::unique_ptr<PlatformTimerEntry>> timers;
    std::unordered_map<uint64_t, std::unique_ptr<PlatformHotkeyEntry>> hotkeys;
    std::unordered_map<uint64_t, std::unique_ptr<PlatformDialogEntry>> dialogs;
    std::unordered_map<uint64_t, PlatformOverlayEntry> overlays;
    std::atomic<uint64_t> next_token{1};
};

PlatformProviderState& platform_provider_state() {
    static PlatformProviderState state;
    return state;
}

std::vector<uint16_t> utf8_to_utf16(const char* value) {
    if (value == nullptr) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value, -1, nullptr, 0);
    if (length <= 0) return {};
    std::vector<uint16_t> result(static_cast<size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                            reinterpret_cast<wchar_t*>(result.data()),
                            length) <= 0) {
        return {};
    }
    return result;
}

sao_sdk_status_t SAO_SDK_CALL platform_tts_speak(
    void*, const char* text_utf8, float volume, float rate) {
    const auto text = utf8_to_utf16(text_utf8);
    if (text.empty()) return SAO_SDK_ERR_INVALID_ARGUMENT;
    const int32_t native_rate = static_cast<int32_t>(
        std::clamp(rate, -10.0f, 10.0f));
    const int32_t native_volume = static_cast<int32_t>(
        std::clamp(volume, 0.0f, 1.0f) * 100.0f);
    return static_cast<sao_sdk_status_t>(sao_ui_alerts_speak(
        text.data(), nullptr, native_rate, native_volume));
}

sao_sdk_status_t SAO_SDK_CALL platform_tts_stop(void*) {
    // SAPI's SPF_PURGEBEFORESPEAK flag is exercised by alerts_speak.
    // A zero-volume word-joiner replaces the queue without producing audio.
    constexpr uint16_t kSilentPurge[] = {0x2060u, 0u};
    return static_cast<sao_sdk_status_t>(
        sao_ui_alerts_speak(kSilentPurge, nullptr, 0, 0));
}

void SAO_ENGINE_CALL platform_render_bridge_release(void* user_data) {
    delete static_cast<PlatformRenderBridge*>(user_data);
}

sao_status_t SAO_ENGINE_CALL platform_render_bridge_callback(
    int32_t hook_point,
    const SaoEngineRenderClockPayload* payload,
    void* user_data) {
    auto* bridge = static_cast<PlatformRenderBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr ||
        payload == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoSdkRenderHookPayload sdk_payload{};
    sdk_payload.frame_time_us = payload->frame_time_us;
    sdk_payload.frame_index = payload->frame_index;
    sdk_payload.frame_delta_us = payload->frame_delta_us;
    sdk_payload.viewport_x_px = payload->viewport_x_px;
    sdk_payload.viewport_y_px = payload->viewport_y_px;
    sdk_payload.viewport_width_px = payload->viewport_width_px;
    sdk_payload.viewport_height_px = payload->viewport_height_px;
    sdk_payload.dispatch_flags = payload->flags;
    return static_cast<sao_status_t>(
        bridge->callback(hook_point, &sdk_payload, bridge->user_data));
}

sao_sdk_status_t SAO_SDK_CALL platform_render_register_ex(
    void*, const char* plugin_id_utf8, const SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    if (out_provider_token != nullptr) *out_provider_token = 0;
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' ||
        spec == nullptr || spec->surface_id_utf8 == nullptr ||
        spec->surface_id_utf8[0] == '\0' || callback == nullptr ||
        out_provider_token == nullptr || !std::isfinite(spec->priority)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    auto* bridge = new (std::nothrow) PlatformRenderBridge();
    if (bridge == nullptr) return SAO_SDK_ERR_NOT_INITIALIZED;
    bridge->callback = callback;
    bridge->user_data = callback_user_data;
    sao_engine_hook_token_t token = 0;
    const sao_status_t status = sao_engine_render_clock_register(
        runtime.render_registry, plugin_id_utf8, spec->surface_id_utf8,
        spec->hook_point, spec->priority, platform_render_bridge_callback,
        bridge, platform_render_bridge_release, &token);
    if (status != SAO_STATUS_OK) {
        delete bridge;
        return static_cast<sao_sdk_status_t>(status);
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_render_register(
    void* user_data, const char* plugin_id_utf8, int32_t hook_point,
    sao_sdk_render_hook_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    const SaoSdkRenderHookSpec spec{
        SAO_ENGINE_ALL_SURFACES, hook_point, 0.0F};
    return platform_render_register_ex(
        user_data, plugin_id_utf8, &spec, callback, callback_user_data,
        out_provider_token);
}

sao_sdk_status_t SAO_SDK_CALL platform_render_unregister(
    void*, uint64_t provider_token) {
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    const sao_status_t status = sao_engine_render_clock_unregister(
        runtime.render_registry, provider_token);
    if (status == SAO_STATUS_ERR_SUBSCRIPTION_GONE) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    return static_cast<sao_sdk_status_t>(status);
}

sao_sdk_status_t SAO_SDK_CALL platform_render_request_redraw(
    void*, const char* surface_id_utf8) {
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    return static_cast<sao_sdk_status_t>(
        sao_engine_render_clock_request_redraw(
            runtime.render_registry, surface_id_utf8));
}

void SAO_CORE_CALL platform_timer_callback(void* user_data) {
    auto* entry = static_cast<PlatformTimerEntry*>(user_data);
    if (entry != nullptr && entry->callback != nullptr) {
        entry->callback(0, entry->user_data);
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_timer_register(
    void* user_data, uint32_t interval_ms,
    sao_sdk_timer_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    if (out_provider_token != nullptr) *out_provider_token = 0;
    if (callback == nullptr || interval_ms == 0 ||
        out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    auto entry = std::make_unique<PlatformTimerEntry>();
    entry->callback = callback;
    entry->user_data = callback_user_data;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->timers.emplace(token, std::move(entry));
        auto& stored = state->timers.at(token);
        const sao_status_t status = sao_core_timer_create(
            interval_ms, platform_timer_callback, stored.get(), &stored->timer);
        if (status != SAO_STATUS_OK) {
            state->timers.erase(token);
            return static_cast<sao_sdk_status_t>(status);
        }
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_timer_unregister(
    void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    std::unique_ptr<PlatformTimerEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->timers.find(provider_token);
        if (found == state->timers.end()) return SAO_SDK_ERR_NOT_FOUND;
        entry = std::move(found->second);
        state->timers.erase(found);
    }
    sao_core_timer_destroy(entry->timer);
    return SAO_SDK_OK;
}

void SAO_UI_CALL platform_hotkey_callback(
    const char*, const SaoUiInputEvent*, void* user_data) {
    auto* entry = static_cast<PlatformHotkeyEntry*>(user_data);
    if (entry != nullptr && entry->callback != nullptr) {
        entry->callback(0, entry->user_data);
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_hotkey_register(
    void* user_data, const char* plugin_id_utf8,
    const char* binding_id_utf8, uint32_t virtual_key,
    uint32_t modifiers, sao_sdk_hotkey_callback_t callback,
    void* callback_user_data, uint64_t* out_provider_token) {
    if (out_provider_token != nullptr) *out_provider_token = 0;
    if (plugin_id_utf8 == nullptr || binding_id_utf8 == nullptr ||
        callback == nullptr || out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto& runtime = SharedRuntime::instance();
    if (runtime.input_router == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    auto entry = std::make_unique<PlatformHotkeyEntry>();
    entry->callback = callback;
    entry->user_data = callback_user_data;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->hotkeys.emplace(token, std::move(entry));
        auto& stored = state->hotkeys.at(token);
        SaoUiHotkeyBindingSpec spec{};
        spec.binding_id_utf8 = binding_id_utf8;
        spec.virtual_key = virtual_key;
        spec.modifiers = modifiers;
        spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
        spec.enforce_ctrl_prefix =
            (modifiers & SAO_UI_MOD_CTRL_BIT) != 0 ||
            (modifiers & SAO_UI_MOD_ANY_CTRL) != 0;
        const sao_status_t status = sao_ui_input_router_register_hotkey(
            runtime.input_router, plugin_id_utf8, &spec,
            platform_hotkey_callback, stored.get(), &stored->binding);
        if (status != SAO_STATUS_OK) {
            state->hotkeys.erase(token);
            return static_cast<sao_sdk_status_t>(status);
        }
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_hotkey_unregister(
    void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    std::unique_ptr<PlatformHotkeyEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->hotkeys.find(provider_token);
        if (found == state->hotkeys.end()) return SAO_SDK_ERR_NOT_FOUND;
        entry = std::move(found->second);
        state->hotkeys.erase(found);
    }
    const sao_status_t status = sao_ui_input_router_unregister_hotkey(
        SharedRuntime::instance().input_router, entry->binding);
    if (status == SAO_STATUS_ERR_NOT_FOUND) return SAO_SDK_OK;
    return static_cast<sao_sdk_status_t>(status);
}

void SAO_UI_CALL platform_dialog_callback(
    SaoUiDialogButton pressed, const char* input_text_utf8,
    size_t input_text_len, void* user_data) {
    auto* entry = static_cast<PlatformDialogEntry*>(user_data);
    if (entry != nullptr && entry->callback != nullptr) {
        entry->callback(0, static_cast<int32_t>(pressed), input_text_utf8,
                        input_text_len, entry->user_data);
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_dialog_show(
    void* user_data, const char*, const SaoSdkDialogSpec* spec,
    sao_sdk_dialog_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    if (out_provider_token != nullptr) *out_provider_token = 0;
    if (spec == nullptr || out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    auto entry = std::make_unique<PlatformDialogEntry>();
    entry->callback = callback;
    entry->user_data = callback_user_data;
    sao_status_t status = sao_ui_dialog_create(
        SharedRuntime::instance().compositor, nullptr, &entry->dialog);
    if (status != SAO_STATUS_OK) return static_cast<sao_sdk_status_t>(status);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dialogs.emplace(token, std::move(entry));
        auto& stored = state->dialogs.at(token);
        SaoUiDialogSpec native{};
        native.kind = static_cast<SaoUiDialogKind>(spec->kind);
        native.title_utf8 = spec->title_utf8;
        native.message_utf8 = spec->message_utf8;
        native.input_prompt_utf8 = spec->input_prompt_utf8;
        native.input_default_utf8 = spec->input_default_utf8;
        native.input_max_length = spec->input_max_length;
        native.dismiss_on_focus_out = spec->dismiss_on_focus_out;
        native.dismiss_on_esc = spec->dismiss_on_esc;
        status = sao_ui_dialog_show(stored->dialog, &native,
                                    platform_dialog_callback, stored.get());
        if (status != SAO_STATUS_OK) {
            auto* dialog = stored->dialog;
            state->dialogs.erase(token);
            sao_ui_dialog_destroy(dialog);
            return static_cast<sao_sdk_status_t>(status);
        }
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_dialog_dismiss(
    void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    std::unique_ptr<PlatformDialogEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->dialogs.find(provider_token);
        if (found == state->dialogs.end()) return SAO_SDK_ERR_NOT_FOUND;
        entry = std::move(found->second);
        state->dialogs.erase(found);
    }
    sao_ui_dialog_destroy(entry->dialog);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_notify_show(
    void*, const char*, const SaoSdkNotifySpec* spec,
    uint64_t* out_provider_token) {
    if (out_provider_token != nullptr) *out_provider_token = 0;
    if (spec == nullptr || spec->text_utf8 == nullptr ||
        out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    const auto text = utf8_to_utf16(spec->text_utf8);
    if (text.empty()) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return static_cast<sao_sdk_status_t>(sao_ui_alerts_banner_show(
        text.data(), spec->duration_ms,
        static_cast<int32_t>(spec->argb_color), out_provider_token));
}

sao_sdk_status_t SAO_SDK_CALL platform_notify_dismiss(
    void*, uint64_t provider_token) {
    return static_cast<sao_sdk_status_t>(
        sao_ui_alerts_banner_hide(provider_token));
}

sao_sdk_status_t SAO_SDK_CALL platform_overlay_set(
    void* user_data, const char* plugin_id_utf8,
    const SaoSdkOverlaySpec* spec, uint64_t* out_provider_token) {
    if (out_provider_token != nullptr) *out_provider_token = 0;
    if (plugin_id_utf8 == nullptr || spec == nullptr ||
        spec->surface_id_utf8 == nullptr ||
        (spec->spec_len != 0 && spec->spec_json_utf8 == nullptr) ||
        out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    const sao_status_t status = sao_engine_render_hook_set_overlay(
        runtime.render_registry, plugin_id_utf8, spec->surface_id_utf8,
        spec->spec_json_utf8, spec->spec_len);
    if (status != SAO_STATUS_OK) return static_cast<sao_sdk_status_t>(status);
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->overlays.emplace(token, PlatformOverlayEntry{
            plugin_id_utf8, spec->surface_id_utf8});
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_overlay_clear(
    void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    PlatformOverlayEntry entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->overlays.find(provider_token);
        if (found == state->overlays.end()) return SAO_SDK_ERR_NOT_FOUND;
        entry = found->second;
        state->overlays.erase(found);
    }
    return static_cast<sao_sdk_status_t>(sao_engine_render_hook_clear_overlay(
        SharedRuntime::instance().render_registry, entry.plugin_id.c_str(),
        entry.surface_id.c_str()));
}

const SaoSdkProviderVTable* platform_provider() {
    static const SaoSdkProviderVTable provider = {
        SAO_SDK_PROVIDER_ABI_VERSION,
        sizeof(SaoSdkProviderVTable),
        &platform_provider_state(),
        nullptr,
        nullptr,
        platform_tts_speak,
        platform_tts_stop,
        platform_render_register,
        platform_render_unregister,
        platform_timer_register,
        platform_timer_unregister,
        platform_hotkey_register,
        platform_hotkey_unregister,
        platform_dialog_show,
        platform_dialog_dismiss,
        platform_notify_show,
        platform_notify_dismiss,
        platform_overlay_set,
        platform_overlay_clear,
        platform_render_register_ex,
        platform_render_request_redraw,
    };
    return &provider;
}

}  // namespace

sao_sdk_status_t bind_provider(ContextState* state,
                               const SaoSdkProviderVTable* provider) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (provider == nullptr) {
        provider_cleanup(state);
        return SAO_SDK_OK;
    }
    if ((provider->abi_version >> 16) !=
            SAO_SDK_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < kProviderMinimumSize) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }

    provider_cleanup(state);
    SaoSdkProviderVTable copy{};
    std::memcpy(&copy, provider,
                std::min<size_t>(provider->struct_size, sizeof(copy)));
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->provider = copy;
        state->provider_bound = true;
    }
    if (copy.retain != nullptr) copy.retain(copy.user_data);
    return SAO_SDK_OK;
}

sao_sdk_status_t bind_platform_provider(ContextState* state) {
    SharedRuntime::instance().ensure_started();
    return bind_provider(state, platform_provider());
}

sao_sdk_status_t provider_status(const ContextState* state) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(
        const_cast<ContextState*>(state)->mu);
    return state->provider_bound ? SAO_SDK_OK : SAO_SDK_ERR_UNSUPPORTED;
}

void provider_cleanup(ContextState* state) {
    if (state == nullptr) return;
    std::vector<CapabilityRegistration> registrations;
    SaoSdkProviderVTable provider{};
    bool was_bound = false;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        registrations.swap(state->capability_registrations);
        provider = state->provider;
        was_bound = state->provider_bound;
        state->provider = {};
        state->provider_bound = false;
        state->render_hooks.clear();
        state->hotkeys.clear();
        state->overlays.clear();
        state->banner_ids.clear();
    }
    for (auto registration = registrations.rbegin();
         registration != registrations.rend(); ++registration) {
        (void)unregister_provider_token(provider, *registration);
    }
    if (was_bound && provider.release != nullptr) {
        provider.release(provider.user_data);
    }
}

sao_sdk_status_t provider_tts_speak(ContextState* state,
                                    const char* text_utf8,
                                    float volume,
                                    float rate) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (text_utf8 == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    bool bound = false;
    const auto provider = provider_snapshot(state, &bound);
    if (!bound || provider.tts_speak == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    return provider.tts_speak(provider.user_data, text_utf8, volume, rate);
}

sao_sdk_status_t provider_tts_stop(ContextState* state) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    bool bound = false;
    const auto provider = provider_snapshot(state, &bound);
    if (!bound || provider.tts_stop == nullptr) return SAO_SDK_ERR_UNSUPPORTED;
    return provider.tts_stop(provider.user_data);
}

sao_sdk_status_t provider_render_register(
    ContextState* state, int32_t hook_point,
    sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_token) {
    const SaoSdkRenderHookSpec spec{
        SAO_ENGINE_ALL_SURFACES, hook_point, 0.0F};
    return provider_render_register_ex(state, &spec, callback, user_data,
                                       out_token);
}

sao_sdk_status_t provider_render_register_ex(
    ContextState* state, const SaoSdkRenderHookSpec* spec,
    sao_sdk_render_hook_callback_t callback, void* user_data,
    sao_sdk_hook_token_t* out_token) {
    if (out_token != nullptr) *out_token = 0;
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (spec == nullptr || spec->surface_id_utf8 == nullptr ||
        spec->surface_id_utf8[0] == '\0' || callback == nullptr ||
        out_token == nullptr || spec->hook_point < 0 ||
        spec->hook_point > SAO_SDK_HOOK_AFTER_PRESENT ||
        !std::isfinite(spec->priority)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    bool bound = false;
    const auto provider = provider_snapshot(state, &bound);
    if (!bound || (provider.register_render_hook == nullptr &&
                   provider.register_render_hook_ex == nullptr) ||
        provider.unregister_render_hook == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = allocate_capability_token(state);
    uint64_t provider_token = 0;
    sao_sdk_status_t status = SAO_SDK_ERR_UNSUPPORTED;
    if (provider.register_render_hook_ex != nullptr) {
        status = provider.register_render_hook_ex(
            provider.user_data, state->plugin_id.c_str(), spec, callback,
            user_data, &provider_token);
    } else if (provider.register_render_hook != nullptr &&
               std::strcmp(spec->surface_id_utf8,
                           SAO_ENGINE_ALL_SURFACES) == 0 &&
               spec->priority == 0.0F) {
        status = provider.register_render_hook(
            provider.user_data, state->plugin_id.c_str(), spec->hook_point,
            callback, user_data, &provider_token);
    }
    if (status != SAO_SDK_OK) return status;
    if (provider_token == 0) return SAO_SDK_ERR_HANDLE_INVALID;
    CapabilityRegistration registration{
        CapabilityKind::render_hook, sdk_token, provider_token, nullptr,
        nullptr};
    const auto add_status = add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)provider.unregister_render_hook(provider.user_data,
                                              provider_token);
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->render_hooks.push_back(RenderHookEntry{
            sdk_token, spec->hook_point, callback, user_data,
            spec->surface_id_utf8, spec->priority});
    }
    *out_token = sdk_token;
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_request_redraw(ContextState* state,
                                         const char* surface_id_utf8) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    bool bound = false;
    const auto provider = provider_snapshot(state, &bound);
    if (!bound || provider.request_redraw == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    return provider.request_redraw(provider.user_data, surface_id_utf8);
}

sao_sdk_status_t provider_render_unregister(ContextState* state,
                                            sao_sdk_hook_token_t token) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (token == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_status(state) != SAO_SDK_OK) return SAO_SDK_ERR_UNSUPPORTED;
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::render_hook, token,
                           &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase_if(state->render_hooks,
                      [token](const RenderHookEntry& entry) {
                          return entry.token == token;
                      });
    }
    return normalize_unregister_status(
        unregister_provider_token(provider, registration));
}

sao_sdk_status_t provider_hotkey_register(
    ContextState* state, const char* binding_id_utf8,
    uint32_t virtual_key, uint32_t modifiers,
    sao_sdk_hotkey_callback_t callback, void* user_data,
    sao_sdk_hotkey_id_t* out_id) {
    if (out_id != nullptr) *out_id = 0;
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (binding_id_utf8 == nullptr || binding_id_utf8[0] == '\0' ||
        callback == nullptr || out_id == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    bool bound = false;
    const auto provider = provider_snapshot(state, &bound);
    if (!bound || provider.register_hotkey == nullptr ||
        provider.unregister_hotkey == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = allocate_capability_token(state);
    auto* bridge = new (std::nothrow) HotkeyCallbackBridge{
        sdk_token, callback, user_data};
    if (bridge == nullptr) return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    const auto status = provider.register_hotkey(
        provider.user_data, state->plugin_id.c_str(), binding_id_utf8,
        virtual_key, modifiers, hotkey_callback_bridge, bridge,
        &provider_token);
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    CapabilityRegistration registration{
        CapabilityKind::hotkey, sdk_token, provider_token, bridge,
        destroy_hotkey_provider_bridge};
    const auto add_status = add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)provider.unregister_hotkey(provider.user_data, provider_token);
        delete bridge;
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        HotkeyEntry entry{};
        entry.sdk_id = sdk_token;
        entry.plugin_cb = callback;
        entry.plugin_ud = user_data;
        entry.binding_id = binding_id_utf8;
        entry.bridge = bridge;
        state->hotkeys.push_back(std::move(entry));
    }
    *out_id = sdk_token;
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_hotkey_unregister(ContextState* state,
                                            sao_sdk_hotkey_id_t id) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (id == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_status(state) != SAO_SDK_OK) return SAO_SDK_ERR_UNSUPPORTED;
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::hotkey, id,
                           &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase_if(state->hotkeys, [id](const HotkeyEntry& entry) {
            return entry.sdk_id == id;
        });
    }
    return normalize_unregister_status(
        unregister_provider_token(provider, registration));
}

sao_sdk_status_t provider_overlay_set(
    ContextState* state, const SaoSdkOverlaySpec* spec,
    sao_sdk_overlay_token_t* out_overlay) {
    if (out_overlay != nullptr) *out_overlay = 0;
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (spec == nullptr || spec->surface_id_utf8 == nullptr ||
        spec->surface_id_utf8[0] == '\0' || out_overlay == nullptr ||
        (spec->spec_len != 0 && spec->spec_json_utf8 == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    bool bound = false;
    const auto provider = provider_snapshot(state, &bound);
    if (!bound || provider.set_overlay == nullptr ||
        provider.clear_overlay == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = allocate_capability_token(state);
    uint64_t provider_token = 0;
    const auto status = provider.set_overlay(
        provider.user_data, state->plugin_id.c_str(), spec, &provider_token);
    if (status != SAO_SDK_OK) return status;
    if (provider_token == 0) return SAO_SDK_ERR_HANDLE_INVALID;
    CapabilityRegistration registration{
        CapabilityKind::overlay, sdk_token, provider_token, nullptr, nullptr};
    const auto add_status = add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)provider.clear_overlay(provider.user_data, provider_token);
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->overlays[spec->surface_id_utf8] =
            reinterpret_cast<sao_sdk_ui_panel_t>(sdk_token);
    }
    *out_overlay = sdk_token;
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_overlay_clear(ContextState* state,
                                        sao_sdk_overlay_token_t overlay) {
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (overlay == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_status(state) != SAO_SDK_OK) return SAO_SDK_ERR_UNSUPPORTED;
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::overlay, overlay,
                           &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase_if(state->overlays,
                      [overlay](const auto& item) {
                          return reinterpret_cast<uint64_t>(item.second) ==
                                 overlay;
                      });
    }
    return normalize_unregister_status(
        unregister_provider_token(provider, registration));
}

}  // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_provider(SaoSdkContext* ctx,
                              const SaoSdkProviderVTable* provider) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::bind_provider(
        sao_sdk_internal::cast_ctx(ctx->ctx_impl), provider);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_platform_services(SaoSdkContext* ctx) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::bind_platform_provider(
        sao_sdk_internal::cast_ctx(ctx->ctx_impl));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_provider_status(const SaoSdkContext* ctx) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_status(
        sao_sdk_internal::cast_ctx(ctx->ctx_impl));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_timer_register(
    const SaoSdkContext* ctx, uint32_t interval_ms,
    sao_sdk_timer_callback_t callback, void* user_data,
    sao_sdk_timer_token_t* out_timer) {
    if (out_timer != nullptr) *out_timer = 0;
    if (ctx == nullptr || callback == nullptr || out_timer == nullptr ||
        interval_ms == 0) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    bool bound = false;
    const auto provider = sao_sdk_internal::provider_snapshot(state, &bound);
    if (!bound || provider.register_timer == nullptr ||
        provider.unregister_timer == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = sao_sdk_internal::allocate_capability_token(state);
    auto* bridge = new (std::nothrow) sao_sdk_internal::TimerCallbackBridge{
        sdk_token, callback, user_data};
    if (bridge == nullptr) return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    const auto status = provider.register_timer(
        provider.user_data, interval_ms, sao_sdk_internal::timer_callback_bridge,
        bridge, &provider_token);
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    sao_sdk_internal::CapabilityRegistration registration{
        sao_sdk_internal::CapabilityKind::timer, sdk_token, provider_token,
        bridge, sao_sdk_internal::destroy_timer_bridge};
    const auto add_status = sao_sdk_internal::add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)provider.unregister_timer(provider.user_data, provider_token);
        delete bridge;
        return add_status;
    }
    *out_timer = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_timer_unregister(
    const SaoSdkContext* ctx, sao_sdk_timer_token_t timer) {
    if (ctx == nullptr || timer == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (sao_sdk_internal::provider_status(state) != SAO_SDK_OK) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    sao_sdk_internal::CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!sao_sdk_internal::take_registration(
            state, sao_sdk_internal::CapabilityKind::timer, timer,
            &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    return sao_sdk_internal::normalize_unregister_status(
        sao_sdk_internal::unregister_provider_token(provider, registration));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_dialog_show(
    const SaoSdkContext* ctx, const SaoSdkDialogSpec* spec,
    sao_sdk_dialog_callback_t callback, void* user_data,
    sao_sdk_dialog_token_t* out_dialog) {
    if (out_dialog != nullptr) *out_dialog = 0;
    if (ctx == nullptr || spec == nullptr || out_dialog == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    bool bound = false;
    const auto provider = sao_sdk_internal::provider_snapshot(state, &bound);
    if (!bound || provider.show_dialog == nullptr ||
        provider.dismiss_dialog == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = sao_sdk_internal::allocate_capability_token(state);
    auto* bridge = new (std::nothrow) sao_sdk_internal::DialogCallbackBridge{
        sdk_token, callback, user_data};
    if (bridge == nullptr) return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    const auto status = provider.show_dialog(
        provider.user_data, state->plugin_id.c_str(), spec,
        sao_sdk_internal::dialog_callback_bridge, bridge, &provider_token);
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    sao_sdk_internal::CapabilityRegistration registration{
        sao_sdk_internal::CapabilityKind::dialog, sdk_token, provider_token,
        bridge, sao_sdk_internal::destroy_dialog_bridge};
    const auto add_status = sao_sdk_internal::add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)provider.dismiss_dialog(provider.user_data, provider_token);
        delete bridge;
        return add_status;
    }
    *out_dialog = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_dialog_dismiss(
    const SaoSdkContext* ctx, sao_sdk_dialog_token_t dialog) {
    if (ctx == nullptr || dialog == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (sao_sdk_internal::provider_status(state) != SAO_SDK_OK) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    sao_sdk_internal::CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!sao_sdk_internal::take_registration(
            state, sao_sdk_internal::CapabilityKind::dialog, dialog,
            &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    return sao_sdk_internal::normalize_unregister_status(
        sao_sdk_internal::unregister_provider_token(provider, registration));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_notify_show(
    const SaoSdkContext* ctx, const SaoSdkNotifySpec* spec,
    sao_sdk_notify_token_t* out_notify) {
    if (out_notify != nullptr) *out_notify = 0;
    if (ctx == nullptr || spec == nullptr || spec->text_utf8 == nullptr ||
        out_notify == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    bool bound = false;
    const auto provider = sao_sdk_internal::provider_snapshot(state, &bound);
    if (!bound || provider.show_notify == nullptr ||
        provider.dismiss_notify == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = sao_sdk_internal::allocate_capability_token(state);
    uint64_t provider_token = 0;
    const auto status = provider.show_notify(
        provider.user_data, state->plugin_id.c_str(), spec, &provider_token);
    if (status != SAO_SDK_OK) return status;
    if (provider_token == 0) return SAO_SDK_ERR_HANDLE_INVALID;
    sao_sdk_internal::CapabilityRegistration registration{
        sao_sdk_internal::CapabilityKind::notify, sdk_token, provider_token,
        nullptr, nullptr};
    const auto add_status = sao_sdk_internal::add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)provider.dismiss_notify(provider.user_data, provider_token);
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->banner_ids.push_back(sdk_token);
    }
    *out_notify = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_notify_dismiss(
    const SaoSdkContext* ctx, sao_sdk_notify_token_t notify) {
    if (ctx == nullptr || notify == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr) return SAO_SDK_ERR_HANDLE_INVALID;
    if (sao_sdk_internal::provider_status(state) != SAO_SDK_OK) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    sao_sdk_internal::CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!sao_sdk_internal::take_registration(
            state, sao_sdk_internal::CapabilityKind::notify, notify,
            &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase(state->banner_ids, notify);
    }
    return sao_sdk_internal::normalize_unregister_status(
        sao_sdk_internal::unregister_provider_token(provider, registration));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_overlay_set(
    const SaoSdkContext* ctx, const SaoSdkOverlaySpec* spec,
    sao_sdk_overlay_token_t* out_overlay) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_overlay_set(
        sao_sdk_internal::cast_ctx(ctx->ctx_impl), spec, out_overlay);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_overlay_clear(
    const SaoSdkContext* ctx, sao_sdk_overlay_token_t overlay) {
    if (ctx == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_overlay_clear(
        sao_sdk_internal::cast_ctx(ctx->ctx_impl), overlay);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_render_dispatch(
    const char* surface_id_utf8,
    int32_t hook_point,
    uint64_t monotonic_time_ns,
    int32_t viewport_x_px,
    int32_t viewport_y_px,
    int32_t viewport_width_px,
    int32_t viewport_height_px,
    uint32_t dispatch_flags) {
    constexpr uint32_t kAllowedSources =
        SAO_SDK_RENDER_DISPATCH_LOGICAL_TICK |
        SAO_SDK_RENDER_DISPATCH_COMPOSITOR_PRESENT;
    if ((dispatch_flags & ~kAllowedSources) != 0) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    auto& runtime = sao_sdk_internal::SharedRuntime::instance();
    runtime.ensure_started();
    return static_cast<sao_sdk_status_t>(sao_engine_render_clock_dispatch(
        runtime.render_registry, surface_id_utf8, hook_point,
        monotonic_time_ns, viewport_x_px, viewport_y_px, viewport_width_px,
        viewport_height_px, dispatch_flags));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_render_gpu_provider_status(void) {
    auto& runtime = sao_sdk_internal::SharedRuntime::instance();
    runtime.ensure_started();
    const sao_status_t status = sao_engine_render_hook_provider_status(
        runtime.render_registry);
    return status == SAO_STATUS_ERR_NOT_IMPLEMENTED
               ? SAO_SDK_ERR_UNSUPPORTED
               : static_cast<sao_sdk_status_t>(status);
}
