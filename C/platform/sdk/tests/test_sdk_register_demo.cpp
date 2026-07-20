// SAO Auto — Wave 7 SDK register demo tests.
//
// These tests demonstrate that "any plugin can register a panel +
// widgets + event subs + hotkeys through the SDK, receive typed
// events, and be cleanly unloaded" — the entire Phase 7 register
// pathway end-to-end.
//
// Key constraints proven here:
//   * The test file does NOT include any `platform/*` internals — it
//     speaks only to `sao/sdk/sao_sdk.h` (the public plugin ABI).
//   * The "plugin" is a plain function that receives a SaoSdkContext*
//     and calls sao_sdk_* free functions on it; no dlopen / real DLL
//     is required for the demo to be meaningful.
//   * Two independent plugins run in parallel without stepping on
//     each other's panels / hotkeys / events.

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// The only SDK header a plugin should ever need to include.
#include "sao/sdk/sao_sdk.h"

// ─── Test-only introspection ─────────────────────────────────────────
//
// These live inside `sao_platform_sdk.dll` but are not part of the
// plugin ABI; the tests import them to inspect state without grubbing
// through ctx_impl.  Real plugins never use them.
extern "C" SAO_SDK_API size_t   SAO_SDK_CALL sao_sdk_test_panel_widget_count(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);
extern "C" SAO_SDK_API uint64_t SAO_SDK_CALL sao_sdk_test_panel_redraw_count(
    const struct SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);
extern "C" SAO_SDK_API size_t   SAO_SDK_CALL sao_sdk_test_render_hook_count(
    const struct SaoSdkContext* ctx);
extern "C" SAO_SDK_API void     SAO_SDK_CALL sao_sdk_test_fire_render_hook(
    int32_t hook_point, const struct SaoSdkRenderHookPayload* payload);
extern "C" SAO_SDK_API size_t   SAO_SDK_CALL sao_sdk_test_hotkey_count(
    const struct SaoSdkContext* ctx);
extern "C" SAO_SDK_API size_t   SAO_SDK_CALL sao_sdk_test_hotkey_fire_by_id(
    const struct SaoSdkContext* ctx, const char* binding_id_utf8);

// ─── Fixture ────────────────────────────────────────────────────────

namespace {

SaoSdkContext* make_ctx(const char* plugin_id) {
    SaoSdkContext* ctx = nullptr;
    const auto rc = sao_sdk_context_create("C:/tmp/test_base", plugin_id, &ctx);
    REQUIRE(rc == SAO_SDK_OK);
    REQUIRE(ctx != nullptr);
    REQUIRE(sao_sdk_context_bind_platform_services(ctx) == SAO_SDK_OK);
    return ctx;
}

SaoSdkPanelDescriptor default_panel(const char* id, const char* title) {
    SaoSdkPanelDescriptor d{};
    d.panel_id_utf8      = id;
    d.title_utf8         = title;
    d.default_x_px       = 100;
    d.default_y_px       = 200;
    d.default_width_px   = 320;
    d.default_height_px  = 240;
    d.min_width_px       = 100;
    d.min_height_px      = 80;
    d.movable            = true;
    d.resizable          = true;
    d.show_titlebar      = true;
    d.show_close_button  = true;
    d.visible            = true;
    d.remember_geometry  = true;
    d.modal              = false;
    d.overlay_style      = false;
    d.z_class            = 0;   // NORMAL
    d.z_within_class     = 0;
    d.initial_opacity    = 1.0f;
    return d;
}

SaoSdkWidgetSpec label_widget(const char* id, const char* text) {
    SaoSdkWidgetSpec w{};
    w.kind             = SAO_SDK_UI_WIDGET_LABEL;
    w.widget_id_utf8   = id;
    w.text_utf8        = text;
    w.props_json_utf8  = nullptr;
    w.props_len        = 0;
    w.x_px             = 12;
    w.y_px             = 12;
    w.width_px         = 200;
    w.height_px        = 24;
    w.z_order          = 0;
    return w;
}

SaoSdkWidgetSpec progress_widget(const char* id, float value, float maxv) {
    SaoSdkWidgetSpec w{};
    w.kind             = SAO_SDK_UI_WIDGET_PROGRESS_BAR;
    w.widget_id_utf8   = id;
    w.text_utf8        = nullptr;
    w.x_px             = 12;
    w.y_px             = 48;
    w.width_px         = 200;
    w.height_px        = 12;
    w.value            = value;
    w.max_value        = maxv;
    w.z_order          = 1;
    return w;
}

}  // namespace

// ─── CASE 1: register panel with label + progress bar ────────────────

TEST_CASE("demo_plugin_registers_panel_with_label_and_progress_bar",
          "[sdk][real_plugins][register]") {
    auto* ctx = make_ctx("demo.plugin.a");

    // "Plugin" side: build descriptor + register.
    const auto d = default_panel("demo.panel.metrics", "Live Metrics");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx, &d, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);

    // Add two widgets.
    const auto lw = label_widget("row.title", "Primary Meter");
    sao_sdk_ui_widget_t label = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &lw, &label) == SAO_SDK_OK);
    REQUIRE(label != nullptr);

    const auto pw = progress_widget("row.bar", 0.62f, 1.0f);
    sao_sdk_ui_widget_t bar = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &pw, &bar) == SAO_SDK_OK);
    REQUIRE(bar != nullptr);

    // Both widgets visible in the panel.
    REQUIRE(sao_sdk_test_panel_widget_count(ctx, panel) == 2u);

    // Duplicate widget_id must be rejected.
    sao_sdk_ui_widget_t dupe = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &lw, &dupe) ==
            SAO_SDK_ERR_ALREADY_EXISTS);

    sao_sdk_context_destroy(ctx);
}

// ─── CASE 2: widget updates through the SDK ─────────────────────────

TEST_CASE("demo_plugin_updates_widget_via_sdk_call",
          "[sdk][real_plugins][update]") {
    auto* ctx = make_ctx("demo.plugin.b");
    const auto d = default_panel("demo.panel.update", "Update Test");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx, &d, &panel) == SAO_SDK_OK);

    auto lw = label_widget("row.value", "initial");
    sao_sdk_ui_widget_t label = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &lw, &label) == SAO_SDK_OK);

    // Update the widget's text.
    lw.text_utf8 = "updated";
    REQUIRE(sao_sdk_panel_update_widget(ctx, panel, label, &lw) == SAO_SDK_OK);

    // Still exactly one widget.
    REQUIRE(sao_sdk_test_panel_widget_count(ctx, panel) == 1u);

    // Update on unknown widget handle → NOT_FOUND.
    auto fake_widget = reinterpret_cast<sao_sdk_ui_widget_t>(uintptr_t(0xdeadbeef));
    REQUIRE(sao_sdk_panel_update_widget(ctx, panel, fake_widget, &lw) ==
            SAO_SDK_ERR_NOT_FOUND);

    sao_sdk_context_destroy(ctx);
}

// ─── CASE 3: remove widget then re-register ──────────────────────────

TEST_CASE("demo_plugin_removes_widget_and_reregisters",
          "[sdk][real_plugins][remove]") {
    auto* ctx = make_ctx("demo.plugin.c");
    const auto d = default_panel("demo.panel.remove", "Remove Test");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx, &d, &panel) == SAO_SDK_OK);

    const auto lw = label_widget("row.a", "one");
    sao_sdk_ui_widget_t label = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &lw, &label) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_panel_widget_count(ctx, panel) == 1u);

    REQUIRE(sao_sdk_panel_remove_widget(ctx, panel, label) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_panel_widget_count(ctx, panel) == 0u);

    // Removing again → NOT_FOUND.
    REQUIRE(sao_sdk_panel_remove_widget(ctx, panel, label) ==
            SAO_SDK_ERR_NOT_FOUND);

    // Re-register the same id — should now succeed.
    sao_sdk_ui_widget_t label2 = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &lw, &label2) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_panel_widget_count(ctx, panel) == 1u);

    sao_sdk_context_destroy(ctx);
}

// ─── CASE 4: subscribe event + receive publish ───────────────────────

namespace {
struct EventProbe {
    std::atomic<int>          fired{0};
    std::string               last_topic;
    std::vector<uint8_t>      last_payload;
};

void SAO_SDK_CALL event_probe_cb(const char* topic_utf8,
                                  const uint8_t* payload,
                                  size_t payload_len,
                                  void* user_data) {
    auto* p = static_cast<EventProbe*>(user_data);
    p->fired.fetch_add(1);
    p->last_topic.assign(topic_utf8 == nullptr ? "" : topic_utf8);
    p->last_payload.assign(payload, payload + payload_len);
}
}  // namespace

TEST_CASE("demo_plugin_subscribes_event_and_receives_publish",
          "[sdk][real_plugins][event]") {
    auto* ctx = make_ctx("demo.plugin.d");

    EventProbe probe;
    sao_sdk_subscription_t sub = 0;
    REQUIRE(sao_sdk_subscribe_event(ctx, "demo.metrics.tick",
                                     event_probe_cb, &probe, &sub) == SAO_SDK_OK);
    REQUIRE(sub != 0);

    const char payload[] = "{\"tick\":42}";
    REQUIRE(sao_sdk_publish_event(ctx, "demo.metrics.tick",
                                   reinterpret_cast<const uint8_t*>(payload),
                                   std::strlen(payload)) == SAO_SDK_OK);

    REQUIRE(probe.fired.load() == 1);
    REQUIRE(probe.last_topic == "demo.metrics.tick");
    REQUIRE(probe.last_payload.size() == std::strlen(payload));

    // Publish to an unrelated topic → probe unchanged.
    REQUIRE(sao_sdk_publish_event(ctx, "other.topic",
                                   nullptr, 0) == SAO_SDK_OK);
    REQUIRE(probe.fired.load() == 1);

    // Unsubscribe and publish again — no additional callback.
    REQUIRE(sao_sdk_unsubscribe_event(ctx, sub) == SAO_SDK_OK);
    REQUIRE(sao_sdk_publish_event(ctx, "demo.metrics.tick",
                                   reinterpret_cast<const uint8_t*>(payload),
                                   std::strlen(payload)) == SAO_SDK_OK);
    REQUIRE(probe.fired.load() == 1);

    // Unsubscribing again → NOT_FOUND.
    REQUIRE(sao_sdk_unsubscribe_event(ctx, sub) == SAO_SDK_ERR_NOT_FOUND);

    sao_sdk_context_destroy(ctx);
}

// ─── CASE 5: hotkey register + router matches ────────────────────────

namespace {
struct HotkeyProbe {
    std::atomic<int>            fired{0};
    sao_sdk_hotkey_id_t         last_id{0};
};

void SAO_SDK_CALL hotkey_probe_cb(sao_sdk_hotkey_id_t hotkey_id,
                                   void* user_data) {
    auto* p = static_cast<HotkeyProbe*>(user_data);
    p->fired.fetch_add(1);
    p->last_id = hotkey_id;
}

// Modifier bits — mirrors sao/ui/input_router.h SAO_UI_MOD_CTRL_BIT.
constexpr uint32_t MOD_CTRL_BIT = 1u << 0;
}  // namespace

TEST_CASE("demo_plugin_registers_hotkey_and_router_matches",
          "[sdk][real_plugins][hotkey]") {
    auto* ctx = make_ctx("demo.plugin.e");

    HotkeyProbe probe;
    SaoSdkHotkeySpec spec{};
    spec.binding_id_utf8      = "demo_toggle";
    spec.virtual_key          = 0x35;      // VK_5
    spec.modifiers            = MOD_CTRL_BIT;
    spec.enforce_ctrl_prefix  = true;
    spec.prevent_default      = false;
    spec.allow_repeat         = false;

    sao_sdk_hotkey_id_t id = 0;
    REQUIRE(sao_sdk_register_hotkey(ctx, &spec, hotkey_probe_cb, &probe, &id) ==
            SAO_SDK_OK);
    REQUIRE(id != 0);
    REQUIRE(sao_sdk_test_hotkey_count(ctx) == 1u);

    // Fire the router match — the SDK bridge should invoke our
    // callback with the correct id.
    const size_t hits = sao_sdk_test_hotkey_fire_by_id(ctx, "demo_toggle");
    REQUIRE(hits == 1u);
    REQUIRE(probe.fired.load() == 1);
    REQUIRE(probe.last_id == id);

    REQUIRE(sao_sdk_unregister_hotkey(ctx, id) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_hotkey_count(ctx) == 0u);

    // Fire after unregister → 0 hits, probe unchanged.
    const size_t hits2 = sao_sdk_test_hotkey_fire_by_id(ctx, "demo_toggle");
    REQUIRE(hits2 == 0u);
    REQUIRE(probe.fired.load() == 1);

    sao_sdk_context_destroy(ctx);
}

// ─── CASE 6: render hook fires at correct clock point ───────────────

namespace {
struct HookProbe {
    std::atomic<int>          before_present{0};
    std::atomic<int>          after_compositor{0};
    std::atomic<int>          before_compositor{0};
    std::atomic<int>          after_present{0};
    std::atomic<uint32_t>     last_frame_index{0};
};

sao_sdk_status_t SAO_SDK_CALL hook_probe_cb(
    int32_t hook_point,
    const SaoSdkRenderHookPayload* payload,
    void* user_data) {
    auto* p = static_cast<HookProbe*>(user_data);
    if (payload != nullptr) {
        p->last_frame_index.store(payload->frame_index);
    }
    switch (hook_point) {
        case SAO_SDK_HOOK_BEFORE_COMPOSITOR: p->before_compositor.fetch_add(1); break;
        case SAO_SDK_HOOK_AFTER_COMPOSITOR:  p->after_compositor.fetch_add(1);  break;
        case SAO_SDK_HOOK_BEFORE_PRESENT:    p->before_present.fetch_add(1);    break;
        case SAO_SDK_HOOK_AFTER_PRESENT:     p->after_present.fetch_add(1);     break;
        default: break;
    }
    return SAO_SDK_OK;
}
}  // namespace

TEST_CASE("demo_plugin_render_hook_fires_at_correct_clock_point",
          "[sdk][real_plugins][render_hook]") {
    auto* ctx = make_ctx("demo.plugin.f");

    HookProbe probe;
    sao_sdk_hook_token_t tok_present  = 0;
    sao_sdk_hook_token_t tok_after    = 0;
    REQUIRE(sao_sdk_register_render_hook(ctx, SAO_SDK_HOOK_BEFORE_PRESENT,
                                          hook_probe_cb, &probe,
                                          &tok_present) == SAO_SDK_OK);
    REQUIRE(sao_sdk_register_render_hook(ctx, SAO_SDK_HOOK_AFTER_COMPOSITOR,
                                          hook_probe_cb, &probe,
                                          &tok_after) == SAO_SDK_OK);
    REQUIRE(tok_present != 0);
    REQUIRE(tok_after != 0);
    REQUIRE(sao_sdk_test_render_hook_count(ctx) == 2u);

    SaoSdkRenderHookPayload payload{};
    payload.frame_index = 17;
    sao_sdk_test_fire_render_hook(SAO_SDK_HOOK_BEFORE_PRESENT, &payload);
    REQUIRE(probe.before_present.load() == 1);
    REQUIRE(probe.last_frame_index.load() == 17u);

    sao_sdk_context_destroy(ctx);
}

// ─── CASE 7: unregister-all-on-shutdown clean ────────────────────────

TEST_CASE("demo_plugin_unregister_all_on_shutdown_clean",
          "[sdk][real_plugins][shutdown]") {
    auto* ctx = make_ctx("demo.plugin.g");

    // Register: 1 panel + 3 widgets + 2 hotkeys + 1 event sub +
    // 1 render hook.  Every registration is context-scoped so
    // context_destroy should sweep them all with no leaks.
    const auto d = default_panel("demo.panel.shutdown", "Shutdown Test");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx, &d, &panel) == SAO_SDK_OK);

    for (int i = 0; i < 3; ++i) {
        const std::string id = "widget." + std::to_string(i);
        const std::string text = "row " + std::to_string(i);
        const auto lw = label_widget(id.c_str(), text.c_str());
        sao_sdk_ui_widget_t w = nullptr;
        REQUIRE(sao_sdk_panel_add_widget(ctx, panel, &lw, &w) == SAO_SDK_OK);
    }

    for (int i = 0; i < 2; ++i) {
        HotkeyProbe probe;   // ignored; we only care about registration
        SaoSdkHotkeySpec spec{};
        const std::string id = "hk_" + std::to_string(i);
        spec.binding_id_utf8      = id.c_str();
        spec.virtual_key          = 0x50 + i;
        spec.modifiers            = MOD_CTRL_BIT;
        spec.enforce_ctrl_prefix  = true;
        sao_sdk_hotkey_id_t hid = 0;
        REQUIRE(sao_sdk_register_hotkey(ctx, &spec, hotkey_probe_cb,
                                         &probe, &hid) == SAO_SDK_OK);
    }

    EventProbe evprobe;
    sao_sdk_subscription_t sub = 0;
    REQUIRE(sao_sdk_subscribe_event(ctx, "demo.shutdown.tick",
                                     event_probe_cb, &evprobe, &sub) == SAO_SDK_OK);

    HookProbe hkprobe;
    sao_sdk_hook_token_t hook_tok = 0;
    REQUIRE(sao_sdk_register_render_hook(ctx, SAO_SDK_HOOK_AFTER_COMPOSITOR,
                                          hook_probe_cb, &hkprobe,
                                          &hook_tok) == SAO_SDK_OK);

    // Snapshot counters before shutdown.
    REQUIRE(sao_sdk_test_panel_widget_count(ctx, panel) == 3u);
    REQUIRE(sao_sdk_test_hotkey_count(ctx) == 2u);
    REQUIRE(sao_sdk_test_render_hook_count(ctx) == 1u);

    // Now destroy — every registration should get swept.
    sao_sdk_context_destroy(ctx);
    // No further calls on ctx allowed.

    // Firing the render hook after context destroy must not crash and
    // must not call the probe (context unregistered from the fire
    // registry).
    SaoSdkRenderHookPayload payload{};
    sao_sdk_test_fire_render_hook(SAO_SDK_HOOK_AFTER_COMPOSITOR, &payload);
    REQUIRE(hkprobe.after_compositor.load() == 0);
}

// ─── CASE 8: two plugins isolated / z-ordering ───────────────────────

TEST_CASE("demo_two_plugins_isolated_z_ordering",
          "[sdk][real_plugins][isolation]") {
    auto* ctx_a = make_ctx("demo.plugin.h1");
    auto* ctx_b = make_ctx("demo.plugin.h2");

    // Plugin A registers a normal-z panel.
    SaoSdkPanelDescriptor da = default_panel("demo.h1.panel", "Plugin A");
    da.z_class            = 0;    // NORMAL
    da.z_within_class     = 10;
    sao_sdk_ui_panel_t panel_a = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx_a, &da, &panel_a) == SAO_SDK_OK);

    // Plugin B registers a TOPMOST-z panel (as an overlay would).
    SaoSdkPanelDescriptor db = default_panel("demo.h2.panel", "Plugin B");
    db.z_class            = 1;    // TOPMOST
    db.z_within_class     = 5;
    db.overlay_style      = true;
    sao_sdk_ui_panel_t panel_b = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx_b, &db, &panel_b) == SAO_SDK_OK);

    // The two panels co-exist without stepping on each other.
    REQUIRE(sao_sdk_test_panel_widget_count(ctx_a, panel_a) == 0u);
    REQUIRE(sao_sdk_test_panel_widget_count(ctx_b, panel_b) == 0u);

    // Cross-context queries return 0 (panel B not owned by ctx_a).
    REQUIRE(sao_sdk_test_panel_widget_count(ctx_a, panel_b) == 0u);
    REQUIRE(sao_sdk_test_panel_widget_count(ctx_b, panel_a) == 0u);

    // Add different widgets in each plugin's panel.
    const auto lw_a = label_widget("a.header", "Plugin A row");
    sao_sdk_ui_widget_t w_a = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx_a, panel_a, &lw_a, &w_a) == SAO_SDK_OK);

    const auto lw_b = label_widget("b.overlay", "Boss Timer");
    sao_sdk_ui_widget_t w_b = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(ctx_b, panel_b, &lw_b, &w_b) == SAO_SDK_OK);

    REQUIRE(sao_sdk_test_panel_widget_count(ctx_a, panel_a) == 1u);
    REQUIRE(sao_sdk_test_panel_widget_count(ctx_b, panel_b) == 1u);

    // Plugin A cannot update plugin B's widget through its own ctx.
    REQUIRE(sao_sdk_panel_update_widget(ctx_a, panel_b, w_b, &lw_b) ==
            SAO_SDK_ERR_NOT_FOUND);

    // Duplicate panel id (already registered by A) must be rejected
    // for B via the global platform panel registry.
    SaoSdkPanelDescriptor conflict = default_panel("demo.h1.panel", "conflict");
    sao_sdk_ui_panel_t clash = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(ctx_b, &conflict, &clash) ==
            SAO_SDK_ERR_ALREADY_EXISTS);

    // Request redraw fans out only within the owning context.
    REQUIRE(sao_sdk_request_redraw(ctx_a, panel_a) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_panel_redraw_count(ctx_a, panel_a) == 1u);
    REQUIRE(sao_sdk_test_panel_redraw_count(ctx_b, panel_b) == 0u);

    // Also verify context accessors return the right identity.
    const char* pid_a = nullptr;
    const char* pid_b = nullptr;
    REQUIRE(sao_sdk_context_get_plugin_id(ctx_a, &pid_a) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_get_plugin_id(ctx_b, &pid_b) == SAO_SDK_OK);
    REQUIRE(pid_a != nullptr);
    REQUIRE(pid_b != nullptr);
    REQUIRE(std::string(pid_a) == "demo.plugin.h1");
    REQUIRE(std::string(pid_b) == "demo.plugin.h2");

    const char* base_a = nullptr;
    REQUIRE(sao_sdk_context_get_base_dir(ctx_a, &base_a) == SAO_SDK_OK);
    REQUIRE(base_a != nullptr);
    REQUIRE(std::string(base_a) == "C:/tmp/test_base");

    sao_sdk_context_destroy(ctx_a);
    sao_sdk_context_destroy(ctx_b);
}

// ─── Sanity: context lifecycle basics ────────────────────────────────

TEST_CASE("sdk_context_create_rejects_null_plugin_id",
          "[sdk][real_plugins][context]") {
    SaoSdkContext* ctx = nullptr;
    REQUIRE(sao_sdk_context_create("C:/tmp", nullptr, &ctx) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(ctx == nullptr);

    REQUIRE(sao_sdk_context_create("C:/tmp", "", &ctx) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
    REQUIRE(ctx == nullptr);

    REQUIRE(sao_sdk_context_create("C:/tmp", "ok", nullptr) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
}
