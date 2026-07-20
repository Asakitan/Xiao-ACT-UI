#include <catch2/catch_test_macros.hpp>

#include "sao/ui/widget_data.h"
#include "sao/ui/widget_chart.h"
#include "sao/ui/widget_input.h"
#include "sao/ui/widget_kit.h"
#include "sao/ui/widget_table.h"
#include "sao/ui/widget_text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
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

std::vector<Pixel> paint_snapshot(sao_ui_widget_handle_t widget, uint32_t width = 64,
                                  uint32_t height = 32) {
    SaoUiOffscreenRasterDesc raster_desc{width, height, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint_at(widget, context, 0, 0, static_cast<int32_t>(width),
                                   static_cast<int32_t>(height), 1.0F) == SAO_STATUS_OK);
    auto pixels = snapshot(raster);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    return pixels;
}

bool pixels_differ(const std::vector<Pixel>& left, const std::vector<Pixel>& right) {
    if (left.size() != right.size())
        return true;
    return !std::equal(left.begin(), left.end(), right.begin(), [](const Pixel& a, const Pixel& b) {
        return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
    });
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

sao_status_t SAO_UI_CALL throw_renderer(
    sao_ui_widget_handle_t, sao_ui_paint_ctx_handle_t,
    int32_t, int32_t, int32_t, int32_t, void*) {
    throw std::runtime_error("renderer callback failure");
}

struct BlockingRenderer {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool release{};
    bool unregister_self{};
    uint64_t token{};
    sao_status_t self_unregister_status{SAO_STATUS_ERR_UNKNOWN};
    uint32_t calls{};
};

sao_status_t SAO_UI_CALL block_renderer(
    sao_ui_widget_handle_t, sao_ui_paint_ctx_handle_t,
    int32_t, int32_t, int32_t, int32_t, void* user_data) {
    auto* renderer = static_cast<BlockingRenderer*>(user_data);
    std::unique_lock lock(renderer->mutex);
    ++renderer->calls;
    renderer->entered = true;
    renderer->cv.notify_all();
    renderer->cv.wait(lock, [renderer] { return renderer->release; });
    lock.unlock();
    if (renderer->unregister_self) {
        renderer->self_unregister_status =
            sao_ui_widget_unregister_renderer_provider(renderer->token);
    }
    return SAO_STATUS_OK;
}

struct SelfUnregisterRenderer {
    uint64_t token{};
    sao_status_t status{SAO_STATUS_ERR_UNKNOWN};
    uint32_t calls{};
};

sao_status_t SAO_UI_CALL unregister_renderer_from_callback(
    sao_ui_widget_handle_t, sao_ui_paint_ctx_handle_t,
    int32_t, int32_t, int32_t, int32_t, void* user_data) {
    auto* renderer = static_cast<SelfUnregisterRenderer*>(user_data);
    ++renderer->calls;
    renderer->status = sao_ui_widget_unregister_renderer_provider(renderer->token);
    return SAO_STATUS_OK;
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
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_widget_test_fail_next_renderer_kind_insertion();
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_widget_test_props_state(
    sao_ui_widget_handle_t handle, const char* color_key, uint32_t* out_color,
    char* out_text, size_t out_text_capacity);

TEST_CASE("generic widget props reject invalid JSON transactionally and decode RGBA colors",
          "[ui][widget_extension][d2d][props][transaction][color]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) ==
            SAO_STATUS_OK);

    constexpr char initial[] =
        R"({"text":"stable","fill":"#11223380","border":"#44556600","fg":"#778899"})";
    REQUIRE(sao_ui_widget_apply_props(
                widget, reinterpret_cast<const uint8_t*>(initial), sizeof(initial) - 1U) ==
            SAO_STATUS_OK);

    uint32_t color = 0;
    char text[32]{};
    REQUIRE(sao_ui_widget_test_props_state(widget, "fill", &color, text, sizeof(text)));
    CHECK(color == 0x80112233U);
    CHECK(std::string(text) == "stable");
    REQUIRE(sao_ui_widget_test_props_state(widget, "border", &color, text, sizeof(text)));
    CHECK(color == 0x00445566U);
    REQUIRE(sao_ui_widget_test_props_state(widget, "fg", &color, text, sizeof(text)));
    CHECK(color == 0xff778899U);

    constexpr char malformed[] = R"({"text":"changed","fill":"#abcdef")";
    CHECK(sao_ui_widget_apply_props(
              widget, reinterpret_cast<const uint8_t*>(malformed), sizeof(malformed) - 1U) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_widget_test_props_state(widget, "fill", &color, text, sizeof(text)));
    CHECK(color == 0x80112233U);
    CHECK(std::string(text) == "stable");

    constexpr char non_object[] = R"([{"fill":"#ffffff"}])";
    CHECK(sao_ui_widget_apply_props(
              widget, reinterpret_cast<const uint8_t*>(non_object), sizeof(non_object) - 1U) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_widget_test_props_state(widget, "border", &color, text, sizeof(text)));
    CHECK(color == 0x00445566U);
    CHECK(std::string(text) == "stable");

    constexpr char invalid_field[] = R"({"text":"changed","active":"yes"})";
    CHECK(sao_ui_widget_apply_props(
              widget, reinterpret_cast<const uint8_t*>(invalid_field),
              sizeof(invalid_field) - 1U) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_widget_test_props_state(widget, "fg", &color, text, sizeof(text)));
    CHECK(color == 0xff778899U);
    CHECK(std::string(text) == "stable");

    sao_ui_widget_destroy(widget);
}

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

    TEST_CASE("generic legacy widget APIs reject a retired handle",
            "[ui][widget_extension][abi][lifetime][stale]") {
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_widget_create(
                SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);

        uint64_t token = 0;
        REQUIRE(sao_ui_widget_add_event_handler(
                widget, SAO_UI_EVT_CLICK, append_one, nullptr, &token) ==
            SAO_STATUS_OK);
        constexpr char props[] = "{\"active\":true}";
        REQUIRE(sao_ui_widget_apply_props(
                    widget, reinterpret_cast<const uint8_t*>(props), sizeof(props) - 1U) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_widget_set_theme_token(widget, "fill", 0xffff0000U) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_widget_clear_theme_token(widget, "fill") == SAO_STATUS_OK);
        REQUIRE(sao_ui_widget_set_active(widget, true) == SAO_STATUS_OK);
        bool hit = false;
        REQUIRE(sao_ui_widget_hit_test(widget, 1.0F, 1.0F, &hit) == SAO_STATUS_OK);
        SaoUiWidgetSizeHint hint{};
        REQUIRE(sao_ui_widget_get_size_hint(widget, 100, 100, &hint) == SAO_STATUS_OK);

        SaoUiOffscreenRasterDesc desc{8, 8, 0x00000000U};
        sao_ui_offscreen_raster_handle_t raster = nullptr;
        sao_ui_paint_ctx_handle_t context = nullptr;
        REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
        REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
        REQUIRE(sao_ui_widget_paint(widget, context, 0, 0, 8, 8) == SAO_STATUS_OK);

        sao_ui_widget_destroy(widget);
        int32_t kind = -1;
        uint32_t removed = 0;
        CHECK(sao_ui_widget_get_kind(widget, &kind) == SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_apply_props(widget, nullptr, 0) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_set_theme_token(widget, "fill", 0xff00ff00U) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_clear_theme_token(widget, "fill") ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_set_active(widget, false) == SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_hit_test(widget, 1.0F, 1.0F, &hit) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_get_size_hint(widget, 100, 100, &hint) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_paint(widget, context, 0, 0, 8, 8) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_release_event_handlers(widget, &removed) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_dispatch_event(widget, SAO_UI_EVT_CLICK, nullptr, 0) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(sao_ui_widget_remove_event_handler(widget, token) ==
            SAO_STATUS_ERR_HANDLE_INVALID);

        sao_ui_paint_ctx_destroy(context);
        sao_ui_offscreen_raster_destroy(raster);
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
    uint64_t duplicate_token = 99;
    CHECK(sao_ui_widget_register_renderer_provider(
              SAO_UI_WIDGET_GAUGE, render_red, &probe, &duplicate_token) ==
          SAO_STATUS_ERR_ALREADY_EXISTS);
    CHECK(duplicate_token == 0);

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

TEST_CASE("paint_at contains renderer exceptions and restores paint state",
          "[ui][widget_extension][paint][exception]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.value = 25.0F;
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) ==
            SAO_STATUS_OK);

    ProviderGuard provider;
    REQUIRE(sao_ui_widget_register_renderer_provider(
                SAO_UI_WIDGET_GAUGE, throw_renderer, nullptr,
                &provider.token) == SAO_STATUS_OK);

    SaoUiOffscreenRasterDesc raster_desc{8, 8, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) ==
            SAO_STATUS_OK);

    CHECK(sao_ui_widget_paint_at(
              gauge, context, 2, 2, 4, 4, 0.25F) ==
          SAO_STATUS_ERR_UNKNOWN);
    REQUIRE(sao_ui_paint_ctx_fill_rect(
                context, 0.0F, 0.0F, 8.0F, 8.0F, 0xff00ff00U) ==
            SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    for (const Pixel pixel : pixels) {
        CHECK(pixel.b == 0);
        CHECK(pixel.g == 255);
        CHECK(pixel.r == 0);
        CHECK(pixel.a == 255);
    }
    CHECK(sao_ui_paint_ctx_pop_opacity(context) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_paint_ctx_pop_clip(context) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(gauge);
}

TEST_CASE("renderer provider unregister waits for an active paint callback",
          "[ui][widget_extension][provider][rundown]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) == SAO_STATUS_OK);

    BlockingRenderer renderer;
    ProviderGuard provider;
    REQUIRE(sao_ui_widget_register_renderer_provider(
                SAO_UI_WIDGET_GAUGE, block_renderer, &renderer, &provider.token) ==
            SAO_STATUS_OK);
    renderer.unregister_self = true;
    renderer.token = provider.token;

    SaoUiOffscreenRasterDesc raster_desc{8, 8, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);

    std::atomic<sao_status_t> paint_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread painter([&] {
        paint_status.store(sao_ui_widget_paint_at(gauge, context, 0, 0, 8, 8, 1.0F));
    });
    {
        std::unique_lock lock(renderer.mutex);
        REQUIRE(renderer.cv.wait_for(lock, 1s, [&renderer] { return renderer.entered; }));
    }

    std::atomic_bool unregister_done{false};
    std::atomic<sao_status_t> unregister_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread unregister([&] {
        unregister_status.store(sao_ui_widget_unregister_renderer_provider(provider.token));
        unregister_done.store(true);
    });
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(unregister_done.load());
    {
        std::lock_guard lock(renderer.mutex);
        renderer.release = true;
    }
    renderer.cv.notify_all();
    painter.join();
    unregister.join();

    CHECK(paint_status.load() == SAO_STATUS_OK);
    CHECK(unregister_status.load() == SAO_STATUS_OK);
    CHECK(unregister_done.load());
    CHECK(renderer.self_unregister_status == SAO_UI_STATUS_ERR_BUSY);
    CHECK(renderer.calls == 1);
    provider.token = 0;

    REQUIRE(sao_ui_widget_paint_at(gauge, context, 0, 0, 8, 8, 1.0F) == SAO_STATUS_OK);
    CHECK(renderer.calls == 1);

    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(gauge);
}

TEST_CASE("renderer provider self unregister returns busy",
          "[ui][widget_extension][provider][rundown][reentry]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) == SAO_STATUS_OK);

    SelfUnregisterRenderer renderer;
    ProviderGuard provider;
    REQUIRE(sao_ui_widget_register_renderer_provider(
                SAO_UI_WIDGET_GAUGE, unregister_renderer_from_callback, &renderer,
                &provider.token) == SAO_STATUS_OK);
    renderer.token = provider.token;

    SaoUiOffscreenRasterDesc raster_desc{8, 8, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&raster_desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);

    REQUIRE(sao_ui_widget_paint_at(gauge, context, 0, 0, 8, 8, 1.0F) == SAO_STATUS_OK);
    CHECK(renderer.calls == 1);
    CHECK(renderer.status == SAO_UI_STATUS_ERR_BUSY);
    REQUIRE(sao_ui_widget_unregister_renderer_provider(provider.token) == SAO_STATUS_OK);
    provider.token = 0;
    REQUIRE(sao_ui_widget_paint_at(gauge, context, 0, 0, 8, 8, 1.0F) == SAO_STATUS_OK);
    CHECK(renderer.calls == 1);

    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(gauge);
}

TEST_CASE("renderer provider registration rolls back the first map insertion",
          "[ui][widget_extension][provider][rollback]") {
    SaoUiGaugeSpec gauge_spec{};
    gauge_spec.value = 25.0F;
    gauge_spec.max_value = 100.0F;
    sao_ui_widget_handle_t gauge = nullptr;
    REQUIRE(sao_ui_gauge_create(nullptr, &gauge_spec, &gauge) == SAO_STATUS_OK);

    RendererProbe probe{gauge};
    REQUIRE(sao_ui_widget_test_fail_next_renderer_kind_insertion() == SAO_STATUS_OK);
    uint64_t failed_token = 99;
    CHECK(sao_ui_widget_register_renderer_provider(
              SAO_UI_WIDGET_GAUGE, render_red, &probe, &failed_token) ==
          SAO_STATUS_ERR_UNKNOWN);
    CHECK(failed_token == 0);

    ProviderGuard provider;
    REQUIRE(sao_ui_widget_register_renderer_provider(
                SAO_UI_WIDGET_GAUGE, render_red, &probe, &provider.token) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_unregister_renderer_provider(provider.token) == SAO_STATUS_OK);
    provider.token = 0;
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

TEST_CASE("typed default painters change pixels with live widget state",
          "[ui][widget_extension][paint][typed][state]") {
    SECTION("button active state") {
        SaoUiButtonSpec spec{};
        spec.text_utf8 = "State";
        spec.kind = SAO_UI_BTN_NORMAL;
        spec.colors.fill_argb = 0xff102030U;
        spec.colors.active_fill_argb = 0xffd06020U;
        spec.colors.fg_argb = 0xffffffffU;
        spec.colors.active_fg_argb = 0xffffffffU;
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_button_create(nullptr, &spec, &widget) == SAO_STATUS_OK);
        const auto before = paint_snapshot(widget);
        REQUIRE(sao_ui_button_set_active(widget, true) == SAO_STATUS_OK);
        CHECK(pixels_differ(before, paint_snapshot(widget)));
        sao_ui_widget_destroy(widget);
    }

    SECTION("progress value") {
        SaoUiProgressBarSpec spec{};
        spec.value = 0.0F;
        spec.max_value = 100.0F;
        spec.style = SAO_UI_PROGRESS_FLAT;
        spec.bg_argb = 0xff111111U;
        spec.fill_argb = 0xff20d060U;
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_progress_bar_create(nullptr, &spec, &widget) == SAO_STATUS_OK);
        const auto before = paint_snapshot(widget);
        REQUIRE(sao_ui_progress_bar_set_value(widget, 75.0F) == SAO_STATUS_OK);
        CHECK(pixels_differ(before, paint_snapshot(widget)));
        sao_ui_widget_destroy(widget);
    }

    SECTION("bar samples") {
        SaoUiBarChartBar initial[] = {{"first", 1.0, 0xff4090e0U, 0, 0.0, 0},
                                      {"second", 4.0, 0xff4090e0U, 0, 0.0, 0}};
        SaoUiBarChartSpec spec{};
        spec.bars = initial;
        spec.bar_count = 2;
        spec.bg_argb = 0xff111111U;
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_bar_chart_create(nullptr, &spec, &widget) == SAO_STATUS_OK);
        const auto before = paint_snapshot(widget);
        SaoUiBarChartBar changed[] = {{"first", 4.0, 0xff4090e0U, 0, 0.0, 0},
                                      {"second", 1.0, 0xff4090e0U, 0, 0.0, 0}};
        REQUIRE(sao_ui_bar_chart_set_bars(widget, changed, 2) == SAO_STATUS_OK);
        CHECK(pixels_differ(before, paint_snapshot(widget)));
        sao_ui_widget_destroy(widget);
    }

    SECTION("table row highlight") {
        SaoUiTableColumn column{};
        column.key_utf8 = "name";
        column.title_utf8 = "Name";
        column.type = SAO_UI_COL_TEXT;
        SaoUiTableSpec spec{};
        spec.columns = &column;
        spec.column_count = 1;
        spec.row_height_px = 24;
        spec.show_header = false;
        spec.body_bg_argb = 0xff111111U;
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_table_create(nullptr, &spec, &widget) == SAO_STATUS_OK);
        SaoUiCellValue cell{};
        cell.kind = SAO_UI_CELL_STRING;
        cell.v.s_utf8 = "row";
        SaoUiTableRow row{};
        row.row_id = 1;
        row.cells = &cell;
        row.cell_count = 1;
        REQUIRE(sao_ui_table_set_rows(widget, &row, 1) == SAO_STATUS_OK);
        const auto before = paint_snapshot(widget);
        row.highlight = true;
        REQUIRE(sao_ui_table_set_rows(widget, &row, 1) == SAO_STATUS_OK);
        CHECK(pixels_differ(before, paint_snapshot(widget)));
        sao_ui_widget_destroy(widget);
    }

    SECTION("tree selection") {
        SaoUiTreeViewSpec spec{};
        spec.row_height_px = 16;
        spec.indent_px = 8;
        spec.caret_width_px = 4;
        spec.body_bg_argb = 0xff111111U;
        SaoUiTreeNode nodes[] = {{1, 0, "root", "", -1, true, true, {}, 0xffffffffU, 0},
                                 {2, 1, "child", "", -1, false, true, {}, 0xffffffffU, 0}};
        sao_ui_widget_handle_t widget = nullptr;
        REQUIRE(sao_ui_tree_view_create(nullptr, &spec, &widget) == SAO_STATUS_OK);
        REQUIRE(sao_ui_tree_view_set_nodes(widget, nodes, 2) == SAO_STATUS_OK);
        const auto before = paint_snapshot(widget);
        REQUIRE(sao_ui_tree_view_select_node(widget, 2) == SAO_STATUS_OK);
        CHECK(pixels_differ(before, paint_snapshot(widget)));
        sao_ui_widget_destroy(widget);
    }
}
