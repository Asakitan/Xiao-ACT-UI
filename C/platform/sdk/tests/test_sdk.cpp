#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

TEST_CASE("sdk ABI version is queryable", "[sdk][abi]") {
    REQUIRE(sao_sdk_abi_version() == SAO_SDK_ABI_VERSION);
    REQUIRE(SAO_SDK_ABI_VERSION_MAJOR == 1u);
}

TEST_CASE("sdk_bind_context creates the unified owned context", "[sdk][bind]") {
    SaoSdkContext ctx;
    for (auto& b : reinterpret_cast<uint8_t (&)[sizeof(ctx)]>(ctx)) b = 0xEE;
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
    REQUIRE(sao_sdk_mem_read_u32(&ctx, 0, &value) == SAO_SDK_ERR_NOT_INITIALIZED);
    REQUIRE(sao_sdk_net_set_frame_callback(&ctx, nullptr, nullptr) ==
            SAO_SDK_ERR_NOT_INITIALIZED);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    sao_sdk_context_destroy(&ctx);
    REQUIRE(ctx.ctx_impl == nullptr);
    REQUIRE(ctx.ui == nullptr);
}

TEST_CASE("sdk_bind_context rejects null out_ctx", "[sdk][bind]") {
    REQUIRE(sao_sdk_bind_context("test", "0.0.0", nullptr) ==
            SAO_SDK_ERR_INVALID_ARGUMENT);
}

TEST_CASE("legacy JSON UI path is context-owned", "[sdk][legacy_ui]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("legacy.test", "1.0.0", &ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    sao_sdk_gpu_tracker_t tracker = 0;
    REQUIRE(ctx.gpu_hunt->create_tracker(ctx.ctx_impl, &tracker) ==
            SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(tracker == 0);

    constexpr char kSpec[] = R"({"kind":"panel","children":[{"kind":"canvas"}]})";
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_ui_register_panel(&ctx, "legacy.panel", "Legacy Panel",
                                      reinterpret_cast<const uint8_t*>(kSpec),
                                      sizeof(kSpec) - 1, nullptr, nullptr,
                                      &panel) == SAO_SDK_OK);
    REQUIRE(panel != nullptr);
    REQUIRE(sao_sdk_ui_set_panel_spec(&ctx, panel,
                                      reinterpret_cast<const uint8_t*>(kSpec),
                                      sizeof(kSpec) - 1) == SAO_SDK_OK);
    REQUIRE(sao_sdk_ui_set_overlay(&ctx, "legacy.surface",
                                   reinterpret_cast<const uint8_t*>(kSpec),
                                   sizeof(kSpec) - 1) == SAO_SDK_OK);

    sao_sdk_hook_token_t token = 0;
    REQUIRE(ctx.ui->register_render_hook(ctx.ctx_impl, "legacy.surface", 1.0f,
                                         reinterpret_cast<void*>(uintptr_t(1)), nullptr,
                                         &token) == SAO_SDK_ERR_UNSUPPORTED);
    REQUIRE(token == 0);

    REQUIRE(sao_sdk_unregister_ui_panel(&ctx, panel) == SAO_SDK_OK);
    sao_sdk_context_destroy(&ctx);
}
