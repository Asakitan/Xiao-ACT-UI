#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

#include <cstdint>
#include <stdexcept>

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_panel_state_insertion(void);
extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_fail_next_panel_unregister(sao_sdk_status_t status);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_widget_count(const SaoSdkContext* ctx, sao_sdk_ui_panel_t panel);
extern "C" SAO_SDK_API size_t SAO_SDK_CALL
sao_sdk_test_panel_cleanup_pending_count(const SaoSdkContext* ctx);

namespace {

SaoSdkPanelDescriptor test_panel_descriptor(const char* panel_id) {
    SaoSdkPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = panel_id;
    descriptor.title_utf8 = "SDK UI transaction test";
    descriptor.default_width_px = 320;
    descriptor.default_height_px = 240;
    descriptor.min_width_px = 100;
    descriptor.min_height_px = 80;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = true;
    descriptor.initial_opacity = 1.0F;
    return descriptor;
}

sao_sdk_status_t SAO_SDK_CALL throwing_register_panel(void*, const SaoSdkPanelDescriptor*,
                                                       sao_sdk_ui_panel_t*) {
    throw std::runtime_error("UI callback fixture");
}

} // namespace

TEST_CASE("sdk ABI version is queryable", "[sdk][abi]") {
    REQUIRE(sao_sdk_abi_version() == SAO_SDK_ABI_VERSION);
    REQUIRE(SAO_SDK_ABI_VERSION_MAJOR == 1u);
}

TEST_CASE("sdk_bind_context creates the unified owned context", "[sdk][bind]") {
    SaoSdkContext ctx;
    for (auto& b : reinterpret_cast<uint8_t (&)[sizeof(ctx)]>(ctx))
        b = 0xEE;
    const auto rc = sao_sdk_bind_context("test", "0.0.0", &ctx);
    REQUIRE(rc == SAO_SDK_OK);
    REQUIRE(ctx.abi_version == SAO_SDK_ABI_VERSION);
    REQUIRE(ctx.ctx_impl != nullptr);
    REQUIRE(ctx.ui != nullptr);
    REQUIRE(ctx.event != nullptr);
    REQUIRE(ctx.mem != nullptr);
    REQUIRE(ctx.net != nullptr);
    REQUIRE(ctx.config != nullptr);
    REQUIRE(ctx.hotkey != nullptr);
    REQUIRE(ctx.tts != nullptr);
    REQUIRE(ctx.banner != nullptr);
    REQUIRE(ctx.gpu_hunt != nullptr);

    bool enabled = false;
    REQUIRE(sao_sdk_config_set_bool(&ctx, "enabled", true) == SAO_SDK_OK);
    REQUIRE(sao_sdk_config_get_bool(&ctx, "enabled", &enabled) == SAO_SDK_OK);
    REQUIRE(enabled);

    uint32_t value = 0;
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0, &value) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(sao_sdk_net_set_frame_callback(&ctx, nullptr, nullptr) == SAO_SDK_ERR_UNSUPPORTED);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    sao_sdk_context_destroy(&ctx);
    REQUIRE(ctx.ctx_impl == nullptr);
    REQUIRE(ctx.ui == nullptr);
}

TEST_CASE("sdk_bind_context rejects null out_ctx", "[sdk][bind]") {
    REQUIRE(sao_sdk_bind_context("test", "0.0.0", nullptr) == SAO_SDK_ERR_INVALID_ARGUMENT);
}

TEST_CASE("legacy JSON UI path is context-owned", "[sdk][legacy_ui]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("legacy.test", "1.0.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    constexpr char kSpec[] = R"({"kind":"panel","children":[{"kind":"canvas"}]})";
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_ui_register_panel(&ctx, "legacy.panel", "Legacy Panel",
                                      reinterpret_cast<const uint8_t*>(kSpec), sizeof(kSpec) - 1,
                                      nullptr, nullptr, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_sdk_ui_set_panel_spec(&ctx, panel, reinterpret_cast<const uint8_t*>(kSpec),
                                      sizeof(kSpec) - 1) == SAO_SDK_OK);
    REQUIRE(sao_sdk_ui_set_overlay(&ctx, "legacy.surface", reinterpret_cast<const uint8_t*>(kSpec),
                                   sizeof(kSpec) - 1) == SAO_SDK_OK);

    sao_sdk_hook_token_t token = 0;
    REQUIRE(ctx.ui->register_render_hook(ctx.ctx_impl, "legacy.surface", 1.0f,
                                         reinterpret_cast<void*>(uintptr_t(1)), nullptr,
                                         &token) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(token == 0);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    sao_sdk_context_destroy(&ctx);
}

TEST_CASE("panel unregister failure preserves SDK ownership for retry",
          "[sdk][ui][panel][unregister][retry]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.unregister.retry", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.unregister.retry");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);

    SaoSdkWidgetSpec widget{};
    widget.kind = SAO_SDK_UI_WIDGET_LABEL;
    widget.widget_id_utf8 = "retained-widget";
    widget.text_utf8 = "retained";
    sao_sdk_ui_widget_t widget_handle = nullptr;
    REQUIRE(sao_sdk_panel_add_widget(&ctx, panel, &widget, &widget_handle) == SAO_SDK_OK);
    REQUIRE(sao_sdk_test_panel_widget_count(&ctx, panel) == 1);

    sao_sdk_test_fail_next_panel_unregister(SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_ERR_INTERNAL);
    CHECK(sao_sdk_test_panel_widget_count(&ctx, panel) == 1);
    sao_sdk_ui_panel_t duplicate = nullptr;
    CHECK(sao_sdk_register_ui_panel(&ctx, &descriptor, &duplicate) ==
          SAO_SDK_ERR_ALREADY_EXISTS);
    CHECK(duplicate == nullptr);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    CHECK(sao_sdk_test_panel_widget_count(&ctx, panel) == 0);
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("panel registration rolls native ownership back when SDK insertion fails",
          "[sdk][ui][panel][register][rollback]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.register.rollback", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.register.rollback");

    sao_sdk_test_fail_next_panel_state_insertion();
    sao_sdk_ui_panel_t panel = reinterpret_cast<sao_sdk_ui_panel_t>(uintptr_t{1});
    CHECK(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_ERR_INTERNAL);
    CHECK(panel == nullptr);

    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("panel registration quarantines native ownership when insertion and rollback fail",
          "[sdk][ui][panel][register][rollback][cleanup-pending]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.register.double_failure", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.register.double_failure");

    sao_sdk_test_fail_next_panel_state_insertion();
    sao_sdk_test_fail_next_panel_unregister(SAO_SDK_ERR_INTERNAL);
    sao_sdk_ui_panel_t panel = reinterpret_cast<sao_sdk_ui_panel_t>(uintptr_t{1});
    CHECK(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_ERR_INTERNAL);
    CHECK(panel == nullptr);
    CHECK(sao_sdk_test_panel_cleanup_pending_count(&ctx) == 1);

    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
    CHECK(ctx.ctx_impl == nullptr);

    SaoSdkContext retry{};
    REQUIRE(sao_sdk_bind_context("ui.register.double_failure.retry", "1.0", &retry) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_register_ui_panel(&retry, &descriptor, &panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_unregister_ui_panel(&retry, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&retry) == SAO_SDK_OK);
}

TEST_CASE("unknown widget kind is rejected without panel side effects",
          "[sdk][ui][widget][validation]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.widget.validation", "1.0", &ctx) == SAO_SDK_OK);
    const auto descriptor = test_panel_descriptor("sdk.ui.widget.validation");
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&ctx, &descriptor, &panel) == SAO_SDK_OK);

    SaoSdkWidgetSpec widget{};
    widget.kind = 777;
    widget.widget_id_utf8 = "unknown-widget";
    sao_sdk_ui_widget_t widget_handle = reinterpret_cast<sao_sdk_ui_widget_t>(uintptr_t{1});
    CHECK(sao_sdk_panel_add_widget(&ctx, panel, &widget, &widget_handle) ==
          SAO_SDK_ERR_INVALID_ARGUMENT);
    CHECK(widget_handle == nullptr);
    CHECK(sao_sdk_test_panel_widget_count(&ctx, panel) == 0);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}

TEST_CASE("public UI C ABI contains callback exceptions", "[sdk][ui][abi][exception]") {
    SaoSdkUiTable table{};
    table.register_ui_panel = throwing_register_panel;
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("ui.throwing.callback", "1.0", &ctx) == SAO_SDK_OK);
    ctx.ui = &table;
    const auto descriptor = test_panel_descriptor("sdk.ui.throwing.callback");
    sao_sdk_ui_panel_t panel = nullptr;
    sao_sdk_status_t status = SAO_SDK_OK;

    CHECK_NOTHROW(status = sao_sdk_register_ui_panel(&ctx, &descriptor, &panel));
    CHECK(status == SAO_SDK_ERR_INTERNAL);
    CHECK(panel == nullptr);
    REQUIRE(sao_sdk_context_try_destroy(&ctx) == SAO_SDK_OK);
}
