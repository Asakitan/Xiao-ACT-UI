#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/sdk/sao_sdk.h"

namespace {

struct RenderProbe {
    std::vector<int>* order = nullptr;
    int marker = 0;
    int calls = 0;
    SaoSdkRenderHookPayload payload{};
};

sao_sdk_status_t SAO_SDK_CALL render_probe(
    int32_t,
    const SaoSdkRenderHookPayload* payload,
    void* user_data) {
    auto* probe = static_cast<RenderProbe*>(user_data);
    probe->order->push_back(probe->marker);
    ++probe->calls;
    probe->payload = *payload;
    return SAO_SDK_OK;
}

uint64_t monotonic_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

TEST_CASE("engine dispatch drives production SDK render clock",
          "[sdk][provider][render_clock][integration]") {
    SaoSdkContext ctx{};
    REQUIRE(sao_sdk_bind_context("render.clock.integration", "1.0", &ctx) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_context_bind_platform_services(&ctx) == SAO_SDK_OK);
    REQUIRE(sao_sdk_platform_render_gpu_provider_status() ==
            SAO_SDK_ERR_UNSUPPORTED);

    std::vector<int> order;
    RenderProbe low{&order, 1};
    RenderProbe high{&order, 2};
    RenderProbe wildcard{&order, 3};
    const SaoSdkRenderHookSpec low_spec{
        "integration.hud", SAO_SDK_HOOK_BEFORE_PRESENT, 1.0F};
    const SaoSdkRenderHookSpec high_spec{
        "integration.hud", SAO_SDK_HOOK_BEFORE_PRESENT, 10.0F};
    const SaoSdkRenderHookSpec wildcard_spec{
        "*", SAO_SDK_HOOK_BEFORE_PRESENT, 5.0F};
    sao_sdk_hook_token_t low_token = 0;
    sao_sdk_hook_token_t high_token = 0;
    sao_sdk_hook_token_t wildcard_token = 0;
    REQUIRE(sao_sdk_register_render_hook_ex(
                &ctx, &low_spec, render_probe, &low, &low_token) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_register_render_hook_ex(
                &ctx, &high_spec, render_probe, &high, &high_token) ==
            SAO_SDK_OK);
    REQUIRE(sao_sdk_register_render_hook_ex(
                &ctx, &wildcard_spec, render_probe, &wildcard,
                &wildcard_token) == SAO_SDK_OK);

    CHECK(order.empty());
    const uint64_t first_ns = monotonic_now_ns();
    REQUIRE(sao_sdk_platform_render_dispatch(
                "integration.hud", SAO_SDK_HOOK_BEFORE_PRESENT, first_ns,
                4, 8, 1600, 900, SAO_SDK_RENDER_DISPATCH_LOGICAL_TICK) ==
            SAO_SDK_OK);
    CHECK(order == std::vector<int>{2, 3, 1});
    CHECK(high.payload.frame_time_us ==
          static_cast<int64_t>(first_ns / 1000u));
    CHECK(high.payload.frame_index == 1u);
    CHECK(high.payload.viewport_x_px == 4);
    CHECK(high.payload.viewport_height_px == 900);

    order.clear();
    REQUIRE(sao_sdk_unregister_render_hook(&ctx, low_token) == SAO_SDK_OK);
    REQUIRE(sao_sdk_request_redraw_surface(&ctx, "integration.hud") ==
            SAO_SDK_OK);
    const uint64_t second_ns = first_ns + 2'000'000u;
    REQUIRE(sao_sdk_platform_render_dispatch(
                "integration.hud", SAO_SDK_HOOK_BEFORE_PRESENT, second_ns,
                0, 0, 1920, 1080,
                SAO_SDK_RENDER_DISPATCH_COMPOSITOR_PRESENT) == SAO_SDK_OK);
    CHECK(order == std::vector<int>{2, 3});
    CHECK(high.payload.frame_index == 2u);
    CHECK(high.payload.frame_delta_us == 2000u);
    CHECK((high.payload.dispatch_flags &
           SAO_SDK_RENDER_DISPATCH_REDRAW_REQUESTED) != 0);

    order.clear();
    REQUIRE(sao_sdk_platform_render_dispatch(
                "integration.other", SAO_SDK_HOOK_BEFORE_PRESENT,
                second_ns + 1'000'000u, 0, 0, 800, 600,
                SAO_SDK_RENDER_DISPATCH_LOGICAL_TICK) == SAO_SDK_OK);
    CHECK(order == std::vector<int>{3});
    CHECK(sao_sdk_platform_render_dispatch(
              "integration.hud", SAO_SDK_HOOK_BEFORE_PRESENT,
              second_ns + 2'000'000u, 0, 0, 800, 600,
              SAO_SDK_RENDER_DISPATCH_GPU_PRESENT) ==
          SAO_SDK_ERR_UNSUPPORTED);

    const int high_calls_before_destroy = high.calls;
    const int wildcard_calls_before_destroy = wildcard.calls;
    sao_sdk_context_destroy(&ctx);
    order.clear();
    REQUIRE(sao_sdk_platform_render_dispatch(
                "integration.hud", SAO_SDK_HOOK_BEFORE_PRESENT,
                second_ns + 3'000'000u, 0, 0, 800, 600,
                SAO_SDK_RENDER_DISPATCH_LOGICAL_TICK) == SAO_SDK_OK);
    CHECK(order.empty());
    CHECK(high.calls == high_calls_before_destroy);
    CHECK(wildcard.calls == wildcard_calls_before_destroy);
}
