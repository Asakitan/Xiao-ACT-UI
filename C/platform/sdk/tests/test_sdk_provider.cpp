#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

namespace {

struct CallbackProbe {
    sao_sdk_timer_token_t timer = 0;
    sao_sdk_hotkey_id_t hotkey = 0;
    sao_sdk_dialog_token_t dialog = 0;
    int32_t dialog_button = -1;
    std::string dialog_input;
    int render_calls = 0;
};

struct ProviderFixture {
    uint64_t next_token = 101;
    std::vector<std::string> events;
    std::string last_plugin_id;
    sao_sdk_timer_callback_t timer_callback = nullptr;
    void* timer_user_data = nullptr;
    sao_sdk_hotkey_callback_t hotkey_callback = nullptr;
    void* hotkey_user_data = nullptr;
    sao_sdk_dialog_callback_t dialog_callback = nullptr;
    void* dialog_user_data = nullptr;
    sao_sdk_render_hook_callback_t render_callback = nullptr;
    void* render_user_data = nullptr;
    size_t live_gpu_sessions = 0;

    uint64_t allocate(const char* kind) {
        const uint64_t token = next_token++;
        events.push_back(std::string("register:") + kind + ":" +
                         std::to_string(token));
        return token;
    }

    void unregister(const char* kind, uint64_t token) {
        events.push_back(std::string("unregister:") + kind + ":" +
                         std::to_string(token));
    }
};

struct GpuSessionFixture {
    ProviderFixture* owner = nullptr;
    uint32_t pid = 0;
};

ProviderFixture* fixture(void* user_data) {
    return static_cast<ProviderFixture*>(user_data);
}

void SAO_SDK_CALL provider_retain(void* user_data) {
    fixture(user_data)->events.emplace_back("retain");
}

void SAO_SDK_CALL provider_release(void* user_data) {
    fixture(user_data)->events.emplace_back("release");
}

sao_sdk_status_t SAO_SDK_CALL provider_tts_speak(
    void* user_data, const char* text_utf8, float, float) {
    fixture(user_data)->events.push_back(
        std::string("tts:speak:") + (text_utf8 == nullptr ? "" : text_utf8));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_tts_stop(void* user_data) {
    fixture(user_data)->events.emplace_back("tts:stop");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_register_render(
    void* user_data, const char* plugin_id_utf8, int32_t,
    sao_sdk_render_hook_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    state->render_callback = callback;
    state->render_user_data = callback_user_data;
    *out_provider_token = state->allocate("render");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_unregister_render(
    void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("render", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_register_timer(
    void* user_data, uint32_t, sao_sdk_timer_callback_t callback,
    void* callback_user_data, uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->timer_callback = callback;
    state->timer_user_data = callback_user_data;
    *out_provider_token = state->allocate("timer");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_unregister_timer(
    void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("timer", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_register_hotkey(
    void* user_data, const char* plugin_id_utf8, const char*, uint32_t,
    uint32_t, sao_sdk_hotkey_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    state->hotkey_callback = callback;
    state->hotkey_user_data = callback_user_data;
    *out_provider_token = state->allocate("hotkey");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_unregister_hotkey(
    void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("hotkey", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_show_dialog(
    void* user_data, const char* plugin_id_utf8, const SaoSdkDialogSpec*,
    sao_sdk_dialog_callback_t callback, void* callback_user_data,
    uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    state->dialog_callback = callback;
    state->dialog_user_data = callback_user_data;
    *out_provider_token = state->allocate("dialog");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_dismiss_dialog(
    void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("dialog", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_show_notify(
    void* user_data, const char* plugin_id_utf8, const SaoSdkNotifySpec*,
    uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    *out_provider_token = state->allocate("notify");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_dismiss_notify(
    void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("notify", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_set_overlay(
    void* user_data, const char* plugin_id_utf8, const SaoSdkOverlaySpec*,
    uint64_t* out_provider_token) {
    auto* state = fixture(user_data);
    state->last_plugin_id = plugin_id_utf8;
    *out_provider_token = state->allocate("overlay");
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_clear_overlay(
    void* user_data, uint64_t provider_token) {
    fixture(user_data)->unregister("overlay", provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_open(
    void* user_data, const char* plugin_id_utf8, void** out_session) {
    if (out_session == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    auto* state = fixture(user_data);
    auto* session = new GpuSessionFixture{state, 0};
    ++state->live_gpu_sessions;
    state->events.push_back(std::string("gpu:open:") + plugin_id_utf8);
    *out_session = session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_close(
    void*, void* session_value) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->owner->events.emplace_back("gpu:close");
    --session->owner->live_gpu_sessions;
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_attach(
    void*, void* session_value, uint32_t pid) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr || pid == 0) return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->pid = pid;
    session->owner->events.push_back("gpu:attach:" + std::to_string(pid));
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_detach(
    void*, void* session_value) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr) return SAO_SDK_ERR_INVALID_ARGUMENT;
    session->owner->events.emplace_back("gpu:detach");
    session->pid = 0;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_enum_regions(
    void*, void* session_value, SaoSdkGpuHuntRegion* out_regions,
    size_t capacity, size_t* out_count) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr || out_count == nullptr || session->pid == 0) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    session->owner->events.emplace_back(
        out_regions == nullptr ? "gpu:enum:size" : "gpu:enum:fill");
    *out_count = 0;
    (void)capacity;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL provider_gpu_read(
    void*, void* session_value, uint64_t, uint8_t* out_buffer,
    size_t buffer_size, size_t* out_bytes_read) {
    auto* session = static_cast<GpuSessionFixture*>(session_value);
    if (session == nullptr || out_bytes_read == nullptr || session->pid == 0 ||
        (buffer_size != 0 && out_buffer == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    session->owner->events.emplace_back("gpu:read");
    for (size_t index = 0; index < buffer_size; ++index) {
        out_buffer[index] = 0;
    }
    *out_bytes_read = buffer_size;
    return SAO_SDK_OK;
}

SaoSdkProviderVTable make_provider(ProviderFixture* fixture_state) {
    SaoSdkProviderVTable provider{};
    provider.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = fixture_state;
    provider.retain = provider_retain;
    provider.release = provider_release;
    provider.tts_speak = provider_tts_speak;
    provider.tts_stop = provider_tts_stop;
    provider.register_render_hook = provider_register_render;
    provider.unregister_render_hook = provider_unregister_render;
    provider.register_timer = provider_register_timer;
    provider.unregister_timer = provider_unregister_timer;
    provider.register_hotkey = provider_register_hotkey;
    provider.unregister_hotkey = provider_unregister_hotkey;
    provider.show_dialog = provider_show_dialog;
    provider.dismiss_dialog = provider_dismiss_dialog;
    provider.show_notify = provider_show_notify;
    provider.dismiss_notify = provider_dismiss_notify;
    provider.set_overlay = provider_set_overlay;
    provider.clear_overlay = provider_clear_overlay;
    provider.gpu_hunt_open_session = provider_gpu_open;
    provider.gpu_hunt_close_session = provider_gpu_close;
    provider.gpu_hunt_attach = provider_gpu_attach;
    provider.gpu_hunt_detach = provider_gpu_detach;
    provider.gpu_hunt_enum_regions = provider_gpu_enum_regions;
    provider.gpu_hunt_read = provider_gpu_read;
    return provider;
}

void SAO_SDK_CALL timer_probe(sao_sdk_timer_token_t timer, void* user_data) {
    static_cast<CallbackProbe*>(user_data)->timer = timer;
}

void SAO_SDK_CALL hotkey_probe(sao_sdk_hotkey_id_t hotkey, void* user_data) {
    static_cast<CallbackProbe*>(user_data)->hotkey = hotkey;
}

void SAO_SDK_CALL dialog_probe(
    sao_sdk_dialog_token_t dialog, int32_t button,
    const char* input_text_utf8, size_t input_text_len, void* user_data) {
    auto* probe = static_cast<CallbackProbe*>(user_data);
    probe->dialog = dialog;
    probe->dialog_button = button;
    probe->dialog_input.assign(input_text_utf8, input_text_len);
}

sao_sdk_status_t SAO_SDK_CALL render_probe(
    int32_t, const SaoSdkRenderHookPayload*, void* user_data) {
    ++static_cast<CallbackProbe*>(user_data)->render_calls;
    return SAO_SDK_OK;
}

}  // namespace

TEST_CASE("SDK context ABI remains stable while provider is versioned",
          "[sdk][provider][abi]") {
    STATIC_REQUIRE(sizeof(SaoSdkContext) ==
                   (sizeof(void*) == 8u ? 104u : 56u));
    STATIC_REQUIRE(offsetof(SaoSdkContext, ctx_impl) == 8u);
    STATIC_REQUIRE(offsetof(SaoSdkContext, banner) ==
                   (sizeof(void*) == 8u ? 88u : 48u));
    STATIC_REQUIRE(offsetof(SaoSdkContext, gpu_hunt) ==
                   (sizeof(void*) == 8u ? 96u : 52u));

    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.abi", "1.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_provider_status(&ctx) == SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkProviderVTable bad{};
    bad.abi_version = 2u << 16;
    bad.struct_size = sizeof(bad);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &bad) ==
            SAO_SDK_ERR_ABI_MISMATCH);

    bad.abi_version = SAO_SDK_PROVIDER_ABI_VERSION;
    bad.struct_size = static_cast<uint32_t>(offsetof(SaoSdkProviderVTable, retain) - 1u);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &bad) ==
            SAO_SDK_ERR_ABI_MISMATCH);

    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_probe, nullptr, &timer) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(sao_sdk_timer_unregister(&ctx, 1) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(ctx.tts->speak(ctx.ctx_impl, "text", 1.0f, 0.0f) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_ERR_UNSUPPORTED);

    sao_sdk_hook_token_t hook = 0;
    REQUIRE(sao_sdk_register_render_hook(&ctx, SAO_SDK_HOOK_BEFORE_PRESENT,
                                         render_probe, nullptr, &hook) ==
            SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkHotkeySpec hotkey{};
    hotkey.binding_id_utf8 = "provider_test";
    hotkey.virtual_key = 0x41;
    sao_sdk_hotkey_id_t hotkey_id = 0;
    REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey, hotkey_probe, nullptr,
                                    &hotkey_id) == SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkDialogSpec dialog{};
    dialog.title_utf8 = "title";
    dialog.message_utf8 = "message";
    sao_sdk_dialog_token_t dialog_token = 0;
    REQUIRE(sao_sdk_dialog_show(&ctx, &dialog, dialog_probe, nullptr,
                                &dialog_token) == SAO_SDK_ERR_UNSUPPORTED);

    SaoSdkNotifySpec notify{"text", 10, 0xff00ff00u};
    sao_sdk_notify_token_t notify_token = 0;
    REQUIRE(sao_sdk_notify_show(&ctx, &notify, &notify_token) ==
            SAO_SDK_ERR_UNSUPPORTED);

    const uint8_t json[] = {'{', '}'};
    SaoSdkOverlaySpec overlay{"surface", json, sizeof(json)};
    sao_sdk_overlay_token_t overlay_token = 0;
    REQUIRE(sao_sdk_overlay_set(&ctx, &overlay, &overlay_token) ==
            SAO_SDK_ERR_UNSUPPORTED);

        ProviderFixture partial_state;
        auto partial = make_provider(&partial_state);
        partial.register_timer = nullptr;
        REQUIRE(sao_sdk_context_bind_provider(&ctx, &partial) == SAO_SDK_OK);
        REQUIRE(sao_sdk_timer_register(&ctx, 10, timer_probe, nullptr, &timer) ==
            SAO_SDK_ERR_UNSUPPORTED);
        REQUIRE(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_OK);

    sao_sdk_context_destroy(&ctx);
        REQUIRE(partial_state.events ==
            std::vector<std::string>{"retain", "tts:stop", "release"});
}

TEST_CASE("provider translates callback tokens and tears down in reverse order",
          "[sdk][provider][lifecycle]") {
    ProviderFixture provider_state;
    auto provider = make_provider(&provider_state);
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("provider.lifecycle", "1.0", &ctx) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_provider(&ctx, &provider) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_provider_status(&ctx) == SAO_SDK_OK);

    CallbackProbe probe;
    REQUIRE(ctx.tts->speak(ctx.ctx_impl, "hello", 0.5f, 1.0f) == SAO_SDK_OK);
    REQUIRE(ctx.tts->stop(ctx.ctx_impl) == SAO_SDK_OK);

    sao_sdk_timer_token_t timer = 0;
    REQUIRE(sao_sdk_timer_register(&ctx, 25, timer_probe, &probe, &timer) ==
            SAO_SDK_OK);

    SaoSdkHotkeySpec hotkey_spec{};
    hotkey_spec.binding_id_utf8 = "toggle";
    hotkey_spec.virtual_key = 0x54;
    sao_sdk_hotkey_id_t hotkey = 0;
    REQUIRE(sao_sdk_register_hotkey(&ctx, &hotkey_spec, hotkey_probe, &probe,
                                    &hotkey) == SAO_SDK_OK);

    sao_sdk_hook_token_t render = 0;
    REQUIRE(sao_sdk_register_render_hook(&ctx, SAO_SDK_HOOK_BEFORE_PRESENT,
                                         render_probe, &probe, &render) ==
            SAO_SDK_OK);

    SaoSdkDialogSpec dialog_spec{};
    dialog_spec.kind = SAO_SDK_DIALOG_INPUT;
    dialog_spec.title_utf8 = "title";
    dialog_spec.message_utf8 = "message";
    sao_sdk_dialog_token_t dialog = 0;
    REQUIRE(sao_sdk_dialog_show(&ctx, &dialog_spec, dialog_probe, &probe,
                                &dialog) == SAO_SDK_OK);

    SaoSdkNotifySpec notify_spec{"notice", 50, 0xffffffffu};
    sao_sdk_notify_token_t notify = 0;
    REQUIRE(sao_sdk_notify_show(&ctx, &notify_spec, &notify) == SAO_SDK_OK);

    const uint8_t json[] = {'{', '}'};
    SaoSdkOverlaySpec overlay_spec{"hud", json, sizeof(json)};
    sao_sdk_overlay_token_t overlay = 0;
    REQUIRE(sao_sdk_overlay_set(&ctx, &overlay_spec, &overlay) == SAO_SDK_OK);

    REQUIRE(provider_state.last_plugin_id == "provider.lifecycle");
    REQUIRE(timer != 101u);
    REQUIRE(hotkey != 102u);
    REQUIRE(render != 103u);
    REQUIRE(dialog != 104u);
    REQUIRE(notify != 105u);
    REQUIRE(overlay != 106u);

    provider_state.timer_callback(999, provider_state.timer_user_data);
    provider_state.hotkey_callback(999, provider_state.hotkey_user_data);
    constexpr char input[] = "value";
    provider_state.dialog_callback(999, SAO_SDK_DIALOG_BUTTON_OK, input,
                                   sizeof(input) - 1,
                                   provider_state.dialog_user_data);
    SaoSdkRenderHookPayload payload{};
    REQUIRE(provider_state.render_callback(
                SAO_SDK_HOOK_BEFORE_PRESENT, &payload,
                provider_state.render_user_data) == SAO_SDK_OK);

    REQUIRE(probe.timer == timer);
    REQUIRE(probe.hotkey == hotkey);
    REQUIRE(probe.dialog == dialog);
    REQUIRE(probe.dialog_button == SAO_SDK_DIALOG_BUTTON_OK);
    REQUIRE(probe.dialog_input == "value");
    REQUIRE(probe.render_calls == 1);

    REQUIRE(sao_sdk_notify_dismiss(&ctx, notify) == SAO_SDK_OK);
    REQUIRE(sao_sdk_notify_dismiss(&ctx, notify) == SAO_SDK_ERR_NOT_FOUND);
    sao_sdk_context_destroy(&ctx);

    const std::vector<std::string> expected{
        "retain",
        "tts:speak:hello",
        "tts:stop",
        "register:timer:101",
        "register:hotkey:102",
        "register:render:103",
        "register:dialog:104",
        "register:notify:105",
        "register:overlay:106",
        "unregister:notify:105",
        "unregister:overlay:106",
        "unregister:dialog:104",
        "unregister:render:103",
        "unregister:hotkey:102",
        "unregister:timer:101",
        "release",
    };
    REQUIRE(provider_state.events == expected);
}

    TEST_CASE("GPU provider sessions require attach and retain tracker lifetime",
          "[sdk][provider][gpu_hunt]") {
        ProviderFixture provider_state;
        auto provider = make_provider(&provider_state);
        SaoSdkContext owner{};
        SaoSdkContext sibling{};
        REQUIRE(sao_sdk_bind_context("provider.gpu", "1.0", &owner) ==
            SAO_SDK_OK);
        REQUIRE(sao_sdk_bind_context("provider.gpu.sibling", "1.0", &sibling) ==
            SAO_SDK_OK);
        REQUIRE(sao_sdk_context_bind_provider(&owner, &provider) == SAO_SDK_OK);

        sao_sdk_gpu_tracker_t tracker = 0;
        REQUIRE(owner.gpu_hunt->create_tracker(owner.ctx_impl, &tracker) ==
            SAO_SDK_OK);
        REQUIRE(tracker != 0);
        REQUIRE(provider_state.live_gpu_sessions == 1);

        REQUIRE(owner.gpu_hunt->tick(owner.ctx_impl, tracker) ==
            SAO_SDK_ERR_NOT_INITIALIZED);
        REQUIRE(sibling.gpu_hunt->attach_tracker(sibling.ctx_impl, tracker, 77) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
        REQUIRE(sibling.gpu_hunt->detach_tracker(sibling.ctx_impl, tracker) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
        REQUIRE(sibling.gpu_hunt->destroy_tracker(sibling.ctx_impl, tracker) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);

        REQUIRE(owner.gpu_hunt->attach_tracker(owner.ctx_impl, tracker, 4242) ==
            SAO_SDK_OK);
        REQUIRE(owner.gpu_hunt->tick(owner.ctx_impl, tracker) == SAO_SDK_OK);
        REQUIRE(owner.gpu_hunt->detach_tracker(owner.ctx_impl, tracker) ==
            SAO_SDK_OK);
        REQUIRE(owner.gpu_hunt->tick(owner.ctx_impl, tracker) ==
            SAO_SDK_ERR_NOT_INITIALIZED);

        REQUIRE(owner.gpu_hunt->attach_tracker(owner.ctx_impl, tracker, 4243) ==
            SAO_SDK_OK);
        sao_sdk_context_destroy(&owner);
        REQUIRE(provider_state.live_gpu_sessions == 0);
        sao_sdk_context_destroy(&sibling);

        const std::vector<std::string> expected{
        "retain",
        "retain",
        "gpu:open:provider.gpu",
        "gpu:attach:4242",
        "gpu:enum:size",
        "gpu:detach",
        "gpu:attach:4243",
        "release",
        "gpu:detach",
        "gpu:close",
        "release",
        };
        REQUIRE(provider_state.events == expected);
    }
