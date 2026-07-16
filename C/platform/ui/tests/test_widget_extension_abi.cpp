#include <catch2/catch_test_macros.hpp>

#include "sao/ui/widget_data.h"
#include "sao/ui/widget_kit.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Pixel {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
};

std::vector<Pixel> snapshot(sao_ui_offscreen_raster_handle_t raster) {
    size_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    REQUIRE(sao_ui_offscreen_raster_snapshot(
                raster, nullptr, 0, &bytes, &width, &height, &stride) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<Pixel> pixels(bytes / sizeof(Pixel));
    REQUIRE(sao_ui_offscreen_raster_snapshot(
                raster, reinterpret_cast<uint8_t*>(pixels.data()), bytes,
                &bytes, &width, &height, &stride) == SAO_STATUS_OK);
    REQUIRE(stride == width * sizeof(Pixel));
    return pixels;
}

struct EventRecorder {
    sao_ui_widget_handle_t widget{};
    uint64_t token_to_remove{};
    std::vector<int> order;
};

void SAO_UI_CALL append_one(
    int32_t, const uint8_t*, size_t, void* user_data) {
    static_cast<EventRecorder*>(user_data)->order.push_back(1);
}

void SAO_UI_CALL append_two(
    int32_t, const uint8_t*, size_t, void* user_data) {
    static_cast<EventRecorder*>(user_data)->order.push_back(2);
}

void SAO_UI_CALL append_three(
    int32_t, const uint8_t*, size_t, void* user_data) {
    static_cast<EventRecorder*>(user_data)->order.push_back(3);
}

void SAO_UI_CALL remove_next(
    int32_t, const uint8_t*, size_t, void* user_data) {
    auto* recorder = static_cast<EventRecorder*>(user_data);
    recorder->order.push_back(1);
    CHECK(sao_ui_widget_remove_event_handler(
              recorder->widget, recorder->token_to_remove) == SAO_STATUS_OK);
}

void SAO_UI_CALL dispatch_nested(
    int32_t, const uint8_t*, size_t, void* user_data) {
    auto* recorder = static_cast<EventRecorder*>(user_data);
    recorder->order.push_back(1);
    CHECK(sao_ui_widget_dispatch_event(
              recorder->widget, SAO_UI_EVT_VALUE_CHANGED, nullptr, 0) ==
          SAO_STATUS_OK);
    recorder->order.push_back(3);
}

void SAO_UI_CALL count_event(
    int32_t, const uint8_t*, size_t, void* user_data) {
    static_cast<std::atomic<uint32_t>*>(user_data)->fetch_add(
        1, std::memory_order_relaxed);
}

struct RendererProbe {
    sao_ui_widget_handle_t expected_widget{};
    uint32_t calls{};
};

sao_status_t SAO_UI_CALL render_red(
    sao_ui_widget_handle_t widget, sao_ui_paint_ctx_handle_t context,
    int32_t x, int32_t y, int32_t width, int32_t height, void* user_data) {
    auto* probe = static_cast<RendererProbe*>(user_data);
    if (widget != probe->expected_widget) return SAO_STATUS_ERR_HANDLE_INVALID;
    ++probe->calls;
    return sao_ui_paint_ctx_fill_rect(
        context, static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height), 0xffff0000U);
}

struct ProviderGuard {
    uint64_t token{};

    ~ProviderGuard() {
        if (token != 0) {
            (void)sao_ui_widget_unregister_renderer_provider(token);
        }
    }
};

}  // namespace

TEST_CASE("generic widget event handlers retain registration order",
          "[ui][widget_extension][events]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(
                SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);

    EventRecorder recorder{widget};
    uint64_t first = 0;
    uint64_t second = 0;
    uint64_t third = 0;
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, append_one, &recorder, &first) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, append_two, &recorder, &second) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, append_three, &recorder, &third) ==
            SAO_STATUS_OK);
    REQUIRE(first != 0);
    REQUIRE(first < second);
    REQUIRE(second < third);

    constexpr std::array<uint8_t, 2> payload{'{', '}'};
    REQUIRE(sao_ui_widget_dispatch_event(
                widget, SAO_UI_EVT_CLICK, payload.data(), payload.size()) ==
            SAO_STATUS_OK);
    CHECK(recorder.order == std::vector<int>{1, 2, 3});

    sao_ui_widget_destroy(widget);
}

TEST_CASE("generic widget removal during dispatch skips the removed handler",
          "[ui][widget_extension][events]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(
                SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);

    EventRecorder recorder{widget};
    uint64_t first = 0;
    uint64_t second = 0;
    uint64_t third = 0;
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, remove_next, &recorder, &first) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, append_two, &recorder, &second) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, append_three, &recorder, &third) ==
            SAO_STATUS_OK);
    recorder.token_to_remove = second;

    REQUIRE(sao_ui_widget_dispatch_event(
                widget, SAO_UI_EVT_CLICK, nullptr, 0) == SAO_STATUS_OK);
    CHECK(recorder.order == std::vector<int>{1, 3});
    CHECK(sao_ui_widget_remove_event_handler(widget, second) ==
          SAO_STATUS_ERR_SUBSCRIPTION_GONE);

    sao_ui_widget_destroy(widget);
}

TEST_CASE("generic widget dispatch is reentrant and owner cleanup is explicit",
          "[ui][widget_extension][events]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(
                SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);

    EventRecorder recorder{widget};
    uint64_t outer = 0;
    uint64_t nested = 0;
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, dispatch_nested, &recorder, &outer) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_VALUE_CHANGED, append_two, &recorder,
                &nested) == SAO_STATUS_OK);

    REQUIRE(sao_ui_widget_dispatch_event(
                widget, SAO_UI_EVT_CLICK, nullptr, 0) == SAO_STATUS_OK);
    CHECK(recorder.order == std::vector<int>{1, 2, 3});

    uint32_t removed = 0;
    REQUIRE(sao_ui_widget_release_event_handlers(widget, &removed) ==
            SAO_STATUS_OK);
    CHECK(removed == 2);
    recorder.order.clear();
    REQUIRE(sao_ui_widget_dispatch_event(
                widget, SAO_UI_EVT_CLICK, nullptr, 0) == SAO_STATUS_OK);
    CHECK(recorder.order.empty());

    sao_ui_widget_destroy(widget);
}

TEST_CASE("generic widget registry supports concurrent registration and removal",
          "[ui][widget_extension][events][threading]") {
    constexpr size_t thread_count = 4;
    constexpr size_t handlers_per_thread = 24;
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(
                SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);

    std::atomic<uint32_t> calls{0};
    std::atomic<bool> failed{false};
    std::array<std::array<uint64_t, handlers_per_thread>, thread_count> tokens{};
    std::vector<std::thread> workers;
    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            for (size_t index = 0; index < handlers_per_thread; ++index) {
                if (sao_ui_widget_add_event_handler(
                        widget, SAO_UI_EVT_CLICK, count_event, &calls,
                        &tokens[thread_index][index]) != SAO_STATUS_OK) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));

    REQUIRE(sao_ui_widget_dispatch_event(
                widget, SAO_UI_EVT_CLICK, nullptr, 0) == SAO_STATUS_OK);
    CHECK(calls.load(std::memory_order_relaxed) ==
          thread_count * handlers_per_thread);

    workers.clear();
    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            for (const uint64_t token : tokens[thread_index]) {
                if (sao_ui_widget_remove_event_handler(widget, token) !=
                    SAO_STATUS_OK) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    sao_ui_widget_destroy(widget);
}

TEST_CASE("paint_at uses extended renderer provider and premultiplies opacity",
          "[ui][widget_extension][paint][opacity]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.value = 25.0F;
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) ==
            SAO_STATUS_OK);

    RendererProbe probe{gauge};
    ProviderGuard provider;
    REQUIRE(sao_ui_widget_register_renderer_provider(
                SAO_UI_WIDGET_GAUGE, render_red, &probe, &provider.token) ==
            SAO_STATUS_OK);

    SaoUiOffscreenRasterDesc raster_desc{8, 8, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint_at(
                gauge, context, 1, 1, 6, 6, 0.25F) == SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    const Pixel pixel = pixels[2U * 8U + 2U];
    CHECK(pixel.b == 0);
    CHECK(pixel.g == 0);
    CHECK(pixel.r == 64);
    CHECK(pixel.a == 64);
    CHECK(probe.calls == 1);

    REQUIRE(sao_ui_widget_unregister_renderer_provider(provider.token) ==
            SAO_STATUS_OK);
    provider.token = 0;
    float ratio = 0.0F;
    REQUIRE(sao_ui_gauge_get_ratio(gauge, &ratio) == SAO_STATUS_OK);
    CHECK(ratio == 0.25F);

    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(gauge);
}

TEST_CASE("paint_at supplies a premultiplied default for extended widgets",
          "[ui][widget_extension][paint][opacity]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.value = 50.0F;
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) ==
            SAO_STATUS_OK);

    SaoUiOffscreenRasterDesc raster_desc{8, 4, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint_at(
                gauge, context, 0, 0, 8, 4, 0.5F) == SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    const Pixel background = pixels[1U * 8U + 6U];
    CHECK(background.a == 128);
    CHECK(background.b <= background.a);
    CHECK(background.g <= background.a);
    CHECK(background.r <= background.a);

    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(gauge);
}
