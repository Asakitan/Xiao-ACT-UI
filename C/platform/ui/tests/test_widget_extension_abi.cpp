#include <catch2/catch_test_macros.hpp>

#include "sao/ui/widget_data.h"
#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"
#include "sao/ui/widget_table.h"
#include "sao/ui/widget_text.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

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

struct BlockingEvent {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool release{};
};

void SAO_UI_CALL block_event(int32_t, const uint8_t*, size_t, void* user_data) {
    auto* event = static_cast<BlockingEvent*>(user_data);
    std::unique_lock lock(event->mutex);
    event->entered = true;
    event->cv.notify_all();
    event->cv.wait(lock, [event] { return event->release; });
}

struct SelfRemoveEvent {
    sao_ui_widget_handle_t widget{};
    uint64_t token{};
    sao_status_t remove_status{SAO_STATUS_ERR_UNKNOWN};
    size_t calls{};
};

void SAO_UI_CALL remove_self(int32_t, const uint8_t*, size_t, void* user_data) {
    auto* event = static_cast<SelfRemoveEvent*>(user_data);
    ++event->calls;
    event->remove_status = sao_ui_widget_remove_event_handler(event->widget, event->token);
}

void SAO_UI_CALL throw_event(int32_t, const uint8_t*, size_t, void*) {
    throw std::runtime_error("generic callback failure");
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

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_text_family_destroy(sao_ui_widget_handle_t handle);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_input_family_destroy(sao_ui_widget_handle_t handle);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_chart_family_destroy(sao_ui_widget_handle_t handle);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_widget_table_family_destroy(sao_ui_widget_handle_t handle);

TEST_CASE("unified widget ABI rejects cross-family and stale handles",
        "[ui][widget_extension][abi][lifetime]") {
    SaoUiLabelSpec label_spec{};
    label_spec.text_utf8 = "label";
    sao_ui_widget_handle_t text = nullptr;
    REQUIRE(sao_ui_label_create(nullptr, &label_spec, &text) == SAO_STATUS_OK);

    SaoUiButtonSpec button_spec{};
    button_spec.text_utf8 = "button";
    button_spec.kind = SAO_UI_BTN_NORMAL;
    sao_ui_widget_handle_t input = nullptr;
    REQUIRE(sao_ui_button_create(nullptr, &button_spec, &input) == SAO_STATUS_OK);

    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t data = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &data) == SAO_STATUS_OK);

    SaoUiTreeViewSpec tree_spec{};
    sao_ui_widget_handle_t table = nullptr;
    REQUIRE(sao_ui_tree_view_create(nullptr, &tree_spec, &table) == SAO_STATUS_OK);

    SaoUiSparklineSpec sparkline_spec{};
    sparkline_spec.max_points = 8;
    sparkline_spec.line_width_px = 1.0F;
    sao_ui_widget_handle_t chart = nullptr;
    REQUIRE(sao_ui_sparkline_create(nullptr, &sparkline_spec, &chart) == SAO_STATUS_OK);

    using FamilyDestroy = void(SAO_UI_CALL*)(sao_ui_widget_handle_t);
    const std::array<FamilyDestroy, 5> family_destroys{
        sao_ui_widget_text_family_destroy,
        sao_ui_widget_input_family_destroy,
        sao_ui_widget_data_family_destroy,
        sao_ui_widget_table_family_destroy,
        sao_ui_widget_chart_family_destroy};
    const std::array<sao_ui_widget_handle_t, 5> handles{
        text, input, data, table, chart};
    for (size_t owner = 0; owner < handles.size(); ++owner) {
        for (size_t family = 0; family < family_destroys.size(); ++family) {
            if (family != owner) family_destroys[family](handles[owner]);
        }
        int32_t kind = -1;
        CHECK(sao_ui_widget_get_kind(handles[owner], &kind) == SAO_STATUS_OK);
    }

    CHECK(sao_ui_label_set_text(text, "still-live") == SAO_STATUS_OK);
    CHECK(sao_ui_gauge_set_value(data, 25.0F) == SAO_STATUS_OK);
    size_t visible_count = 1;
    CHECK(sao_ui_tree_view_get_visible_count(table, &visible_count) == SAO_STATUS_OK);
    CHECK(sao_ui_button_set_text(input, "still-live") == SAO_STATUS_OK);
    CHECK(sao_ui_sparkline_append(chart, 1.0) == SAO_STATUS_OK);

    for (const auto handle : handles) {
      int32_t kind = -1;
      REQUIRE(sao_ui_widget_get_kind(handle, &kind) == SAO_STATUS_OK);
      sao_ui_widget_destroy(handle);
      sao_ui_widget_destroy(handle);
      CHECK(sao_ui_widget_get_kind(handle, &kind) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    }

    CHECK(sao_ui_label_set_text(text, "stale") ==
        SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_button_set_text(input, "stale") ==
        SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_gauge_set_value(data, 1.0F) ==
        SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_tree_view_get_visible_count(table, &visible_count) ==
        SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_sparkline_append(chart, 2.0) ==
        SAO_STATUS_ERR_HANDLE_INVALID);

    for (const auto handle : handles) {
        for (const auto destroy : family_destroys) destroy(handle);
    }
}

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

TEST_CASE("generic widget callback removal waits for the active generation",
          "[ui][widget_extension][events][rundown]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) ==
            SAO_STATUS_OK);
    BlockingEvent event;
    uint64_t token = 0;
    REQUIRE(sao_ui_widget_add_event_handler(widget, SAO_UI_EVT_CLICK, block_event, &event,
                                            &token) == SAO_STATUS_OK);
    std::atomic<sao_status_t> dispatch_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread dispatch([&] {
        dispatch_status.store(sao_ui_widget_dispatch_event(widget, SAO_UI_EVT_CLICK, nullptr, 0));
    });
    {
        std::unique_lock lock(event.mutex);
        REQUIRE(event.cv.wait_for(lock, 1s, [&event] { return event.entered; }));
    }
    std::atomic_bool removed{false};
    std::atomic<sao_status_t> remove_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread remove([&] {
        remove_status.store(sao_ui_widget_remove_event_handler(widget, token));
        removed.store(true);
    });
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(removed.load());
    {
        std::lock_guard lock(event.mutex);
        event.release = true;
    }
    event.cv.notify_all();
    dispatch.join();
    remove.join();
    CHECK(dispatch_status.load() == SAO_STATUS_OK);
    CHECK(remove_status.load() == SAO_STATUS_OK);
    CHECK(removed.load());
    sao_ui_widget_destroy(widget);
}

TEST_CASE("generic widget release and destroy wait for active callbacks",
          "[ui][widget_extension][events][rundown]") {
    for (const bool destroy_owner : {false, true}) {
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) ==
                SAO_STATUS_OK);
        BlockingEvent event;
        uint64_t token = 0;
        REQUIRE(sao_ui_widget_add_event_handler(widget, SAO_UI_EVT_CLICK, block_event, &event,
                                                &token) == SAO_STATUS_OK);
        std::thread dispatch(
            [&] { CHECK(sao_ui_widget_dispatch_event(widget, SAO_UI_EVT_CLICK, nullptr, 0) ==
                       SAO_STATUS_OK); });
        {
            std::unique_lock lock(event.mutex);
            REQUIRE(event.cv.wait_for(lock, 1s, [&event] { return event.entered; }));
        }
        std::atomic_bool teardown_done{false};
        std::thread teardown([&] {
            if (destroy_owner) {
                sao_ui_widget_destroy(widget);
            } else {
                uint32_t removed = 0;
                CHECK(sao_ui_widget_release_event_handlers(widget, &removed) == SAO_STATUS_OK);
                CHECK(removed == 1);
            }
            teardown_done.store(true);
        });
        std::this_thread::sleep_for(30ms);
        CHECK_FALSE(teardown_done.load());
        {
            std::lock_guard lock(event.mutex);
            event.release = true;
        }
        event.cv.notify_all();
        dispatch.join();
        teardown.join();
        CHECK(teardown_done.load());
        if (!destroy_owner)
            sao_ui_widget_destroy(widget);
    }
}

TEST_CASE("generic widget self removal and callback exceptions stay inside the C ABI",
          "[ui][widget_extension][events][rundown]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) ==
            SAO_STATUS_OK);
    SelfRemoveEvent self{widget};
    REQUIRE(sao_ui_widget_add_event_handler(widget, SAO_UI_EVT_CLICK, remove_self, &self,
                                            &self.token) == SAO_STATUS_OK);
    CHECK(sao_ui_widget_dispatch_event(widget, SAO_UI_EVT_CLICK, nullptr, 0) == SAO_STATUS_OK);
    CHECK(self.remove_status == SAO_STATUS_OK);
    CHECK(self.calls == 1);
    CHECK(sao_ui_widget_dispatch_event(widget, SAO_UI_EVT_CLICK, nullptr, 0) == SAO_STATUS_OK);
    CHECK(self.calls == 1);

    uint64_t throwing_token = 0;
    REQUIRE(sao_ui_widget_add_event_handler(widget, SAO_UI_EVT_CLICK, throw_event, nullptr,
                                            &throwing_token) == SAO_STATUS_OK);
    CHECK(sao_ui_widget_dispatch_event(widget, SAO_UI_EVT_CLICK, nullptr, 0) ==
          SAO_STATUS_ERR_UNKNOWN);
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
