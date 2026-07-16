#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "sao/engine/event_bus.h"
#include "sao/engine/render_hook.h"
#include "sao/engine/runtime.h"
#include "sao/engine/state.h"

namespace {

struct EventContext {
    std::atomic<uint32_t> calls{0};
    std::string last_topic;
};

void SAO_ENGINE_CALL record_event(const char* topic,
                                  const uint8_t*,
                                  size_t,
                                  const SaoEngineEventHeader*,
                                  void* user_data) {
    auto* context = static_cast<EventContext*>(user_data);
    context->last_topic = topic;
    context->calls.fetch_add(1, std::memory_order_relaxed);
}

struct StateCallbackContext {
    sao_engine_state_handle_t state = nullptr;
    sao_engine_state_subscription_t subscription = 0;
    std::atomic<uint32_t> calls{0};
    std::atomic<sao_status_t> unsubscribe_status{SAO_STATUS_ERR_UNKNOWN};
};

void SAO_ENGINE_CALL state_callback(const char*,
                                    const SaoEngineStateValue*,
                                    const char*,
                                    uint32_t,
                                    void* user_data) {
    auto* context = static_cast<StateCallbackContext*>(user_data);
    context->calls.fetch_add(1, std::memory_order_relaxed);
    context->unsubscribe_status.store(
        sao_engine_state_unsubscribe(context->state, context->subscription),
        std::memory_order_relaxed);
}

struct RenderContext {
    std::vector<int>* order = nullptr;
    int marker = 0;
    sao_engine_render_hook_registry_handle_t registry = nullptr;
    sao_engine_hook_token_t unregister_token = 0;
};

struct ClockContext {
    std::vector<int>* order = nullptr;
    int marker = 0;
    sao_engine_render_hook_registry_handle_t registry = nullptr;
    sao_engine_hook_token_t unregister_token = 0;
    SaoEngineRenderClockPayload last_payload{};
};

sao_status_t SAO_ENGINE_CALL clock_callback(
    int32_t,
    const SaoEngineRenderClockPayload* payload,
    void* user_data) {
    auto* context = static_cast<ClockContext*>(user_data);
    context->order->push_back(context->marker);
    context->last_payload = *payload;
    if (context->unregister_token != 0) {
        (void)sao_engine_render_clock_unregister(
            context->registry, context->unregister_token);
        context->unregister_token = 0;
    }
    return SAO_STATUS_OK;
}

sao_status_t SAO_ENGINE_CALL render_callback(const char*,
                                             const uint8_t*,
                                             size_t,
                                             uint8_t*,
                                             size_t,
                                             size_t* out_written,
                                             void* user_data) {
    auto* context = static_cast<RenderContext*>(user_data);
    context->order->push_back(context->marker);
    *out_written = 0;
    if (context->unregister_token != 0) {
        (void)sao_engine_render_hook_unregister(context->registry,
                                                context->unregister_token);
    }
    return SAO_STATUS_OK;
}

}  // namespace

TEST_CASE("base event bus ABI uses production dispatch and retention") {
    sao_engine_event_bus_handle_t bus = nullptr;
    REQUIRE(sao_engine_event_bus_create(8, 0.0F, &bus) == SAO_STATUS_OK);

    EventContext exact;
    EventContext wildcard;
    sao_engine_subscription_t exact_token = 0;
    sao_engine_subscription_t wildcard_token = 0;
    REQUIRE(sao_engine_event_bus_subscribe(bus, "sample.topic", "owner-a",
                                           record_event, &exact,
                                           &exact_token) == SAO_STATUS_OK);
    REQUIRE(sao_engine_event_bus_subscribe(bus, "*", "owner-b", record_event,
                                           &wildcard, &wildcard_token) ==
            SAO_STATUS_OK);

    constexpr uint8_t payload[] = {'{', '}'};
    REQUIRE(sao_engine_event_bus_publish(bus, "sample.topic", payload,
                                         sizeof(payload), nullptr) ==
            SAO_STATUS_OK);
    CHECK(exact.calls.load() == 1);
    CHECK(wildcard.calls.load() == 1);
    CHECK(wildcard.last_topic == "sample.topic");

    uint32_t event_count = 0;
    size_t topics_used = 0;
    size_t jsons_used = 0;
    REQUIRE(sao_engine_event_bus_recent(bus, 0, nullptr, 0, nullptr, 0,
                                        nullptr, 0, &event_count, &topics_used,
                                        &jsons_used) == SAO_STATUS_OK);
    CHECK(event_count == 1);
    CHECK(topics_used == std::strlen("sample.topic"));
    CHECK(jsons_used == sizeof(payload));

    SaoEngineBusStats stats{};
    REQUIRE(sao_engine_event_bus_stats(bus, &stats) == SAO_STATUS_OK);
    CHECK(stats.published == 1);
    CHECK(stats.retained == 1);
    CHECK(stats.active_subscription_count == 2);

    uint32_t removed = 0;
    REQUIRE(sao_engine_event_bus_unsubscribe_owner(bus, "owner-a", &removed) ==
            SAO_STATUS_OK);
    CHECK(removed == 1);
    CHECK(sao_engine_event_bus_unsubscribe(bus, exact_token) ==
          SAO_STATUS_ERR_SUBSCRIPTION_GONE);
    REQUIRE(sao_engine_event_bus_unsubscribe(bus, wildcard_token) ==
            SAO_STATUS_OK);
    sao_engine_event_bus_destroy(bus);
}

TEST_CASE("state snapshot and callbacks are reentrant") {
    sao_engine_state_handle_t state = nullptr;
    REQUIRE(sao_engine_state_create(&state) == SAO_STATUS_OK);

    StateCallbackContext callback_context;
    callback_context.state = state;
    REQUIRE(sao_engine_state_subscribe(state, state_callback,
                                       &callback_context,
                                       &callback_context.subscription) ==
            SAO_STATUS_OK);

    REQUIRE(sao_engine_state_set_int64(state, "count", 42,
                                       SAO_ENGINE_SOURCE_MEMORY) ==
            SAO_STATUS_OK);
    CHECK(callback_context.calls.load() == 1);
    CHECK(callback_context.unsubscribe_status.load() == SAO_STATUS_OK);
    REQUIRE(sao_engine_state_set_string(state, "name", "Aldina",
                                        SAO_ENGINE_SOURCE_TCP) ==
            SAO_STATUS_OK);
    CHECK(callback_context.calls.load() == 1);

    uint32_t count = 0;
    size_t bytes = 0;
    REQUIRE(sao_engine_state_snapshot(state, nullptr, 0, nullptr, 0, &count,
                                      &bytes) == SAO_STATUS_OK);
    REQUIRE(count == 2);
    std::vector<SaoEngineStateSnapshotEntry> entries(count);
    std::vector<char> buffer(bytes);
    REQUIRE(sao_engine_state_snapshot(state, entries.data(), entries.size(),
                                      buffer.data(), buffer.size(), &count,
                                      &bytes) == SAO_STATUS_OK);
    CHECK(std::string(buffer.data() + entries[0].key_offset,
                      entries[0].key_length) == "count");
    CHECK(entries[0].value.as_int64 == 42);
    CHECK(std::string(buffer.data() + entries[1].string_offset,
                      entries[1].string_length) == "Aldina");

    SaoEngineStateValue value{};
    char text[16]{};
    REQUIRE(sao_engine_state_get(state, "name", &value, text,
                                 sizeof(text)) == SAO_STATUS_OK);
    CHECK(value.value_type == SAO_ENGINE_STATE_STRING);
    CHECK(std::string(text) == "Aldina");
    sao_engine_state_destroy(state);
}

TEST_CASE("render hooks use stable priority and support mid-dispatch removal") {
    sao_engine_render_hook_registry_handle_t registry = nullptr;
    REQUIRE(sao_engine_render_hook_registry_create(&registry) == SAO_STATUS_OK);
    // Wave 17c: reclassified from NOT_IMPLEMENTED to CAPABILITY_MISSING —
    // no external render provider is bound to the registry.
    CHECK(sao_engine_render_hook_provider_status(registry) ==
          SAO_STATUS_ERR_CAPABILITY_MISSING);

    std::vector<int> order;
    RenderContext low{&order, 1, registry, 0};
    RenderContext high{&order, 2, registry, 0};
    RenderContext wildcard{&order, 3, registry, 0};
    sao_engine_hook_token_t low_token = 0;
    sao_engine_hook_token_t high_token = 0;
    sao_engine_hook_token_t wildcard_token = 0;
    REQUIRE(sao_engine_render_hook_register(registry, "low", "hud", 1.0F,
                                             render_callback, &low,
                                             &low_token) == SAO_STATUS_OK);
    REQUIRE(sao_engine_render_hook_register(registry, "high", "hud", 10.0F,
                                             render_callback, &high,
                                             &high_token) == SAO_STATUS_OK);
    REQUIRE(sao_engine_render_hook_register(
                registry, "wildcard", SAO_ENGINE_ALL_SURFACES, 5.0F,
                render_callback, &wildcard, &wildcard_token) == SAO_STATUS_OK);
    high.unregister_token = low_token;

    constexpr uint8_t input[] = {'{', '}'};
    uint8_t output[sizeof(input)]{};
    size_t written = 0;
    REQUIRE(sao_engine_render_hook_dispatch(registry, "hud", input,
                                             sizeof(input), output,
                                             sizeof(output), &written) ==
            SAO_STATUS_OK);
    CHECK(order == std::vector<int>{2, 3});
    CHECK(written == sizeof(input));
    CHECK(std::memcmp(input, output, sizeof(input)) == 0);

    REQUIRE(sao_engine_render_hook_set_overlay(
                registry, "plugin", "hud", input, sizeof(input)) ==
            SAO_STATUS_OK);
    REQUIRE(sao_engine_render_hook_clear_overlay(registry, "plugin", "hud") ==
            SAO_STATUS_OK);
    CHECK(sao_engine_render_hook_unregister(registry, low_token) ==
          SAO_STATUS_ERR_SUBSCRIPTION_GONE);
    REQUIRE(sao_engine_render_hook_unregister(registry, high_token) ==
            SAO_STATUS_OK);
    REQUIRE(sao_engine_render_hook_unregister(registry, wildcard_token) ==
            SAO_STATUS_OK);
    sao_engine_render_hook_registry_destroy(registry);
}

TEST_CASE("runtime owns usable primitives and serializes monotonic ticks") {
    sao_engine_runtime_handle_t runtime = nullptr;
    REQUIRE(sao_engine_runtime_create(&runtime) == SAO_STATUS_OK);
    REQUIRE(sao_engine_runtime_event_bus(runtime) != nullptr);
    REQUIRE(sao_engine_runtime_state(runtime) != nullptr);
    REQUIRE(sao_engine_runtime_render_hooks(runtime) != nullptr);
    REQUIRE(sao_engine_runtime_tick(runtime, 100) == SAO_STATUS_OK);
    REQUIRE(sao_engine_runtime_tick(runtime, 101) == SAO_STATUS_OK);
    CHECK(sao_engine_runtime_tick(runtime, 99) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    auto* state = sao_engine_runtime_state(runtime);
    std::atomic<uint32_t> write_failures{0};
    std::vector<std::thread> writers;
    for (int index = 0; index < 4; ++index) {
        writers.emplace_back([state, index, &write_failures] {
            for (int value = 0; value < 100; ++value) {
                const std::string key = "thread-" + std::to_string(index);
                if (sao_engine_state_set_int64(
                        state, key.c_str(), value,
                        SAO_ENGINE_SOURCE_ESTIMATE) != SAO_STATUS_OK) {
                    write_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    CHECK(write_failures.load() == 0);
    uint32_t count = 0;
    size_t bytes = 0;
    REQUIRE(sao_engine_state_snapshot(state, nullptr, 0, nullptr, 0, &count,
                                      &bytes) == SAO_STATUS_OK);
    CHECK(count == 4);
    sao_engine_runtime_destroy(runtime);
}

TEST_CASE("render clock is explicitly pumped and remains GPU gated") {
    sao_engine_runtime_handle_t runtime = nullptr;
    REQUIRE(sao_engine_runtime_create(&runtime) == SAO_STATUS_OK);
    auto registry = sao_engine_runtime_render_hooks(runtime);

    std::vector<int> order;
    ClockContext low{&order, 1, registry};
    ClockContext high{&order, 2, registry};
    ClockContext wildcard{&order, 3, registry};
    sao_engine_hook_token_t low_token = 0;
    sao_engine_hook_token_t high_token = 0;
    sao_engine_hook_token_t wildcard_token = 0;
    REQUIRE(sao_engine_render_clock_register(
                registry, "clock.low", "clock.hud",
                SAO_ENGINE_RENDER_BEFORE_PRESENT, 1.0F, clock_callback, &low,
                nullptr, &low_token) == SAO_STATUS_OK);
    REQUIRE(sao_engine_render_clock_register(
                registry, "clock.high", "clock.hud",
                SAO_ENGINE_RENDER_BEFORE_PRESENT, 10.0F, clock_callback,
                &high, nullptr, &high_token) == SAO_STATUS_OK);
    REQUIRE(sao_engine_render_clock_register(
                registry, "clock.wildcard", SAO_ENGINE_ALL_SURFACES,
                SAO_ENGINE_RENDER_BEFORE_PRESENT, 5.0F, clock_callback,
                &wildcard, nullptr, &wildcard_token) == SAO_STATUS_OK);
    high.unregister_token = low_token;

    CHECK(order.empty());
    REQUIRE(sao_engine_runtime_render_dispatch(
                runtime, "clock.hud", SAO_ENGINE_RENDER_BEFORE_PRESENT,
                1'000'000u, 10, 20, 1280, 720,
                SAO_ENGINE_RENDER_DISPATCH_LOGICAL_TICK) == SAO_STATUS_OK);
    CHECK(order == std::vector<int>{2, 3});
    CHECK(high.last_payload.frame_time_us == 1000);
    CHECK(high.last_payload.frame_index == 1);
    CHECK(high.last_payload.frame_delta_us == 0);
    CHECK(high.last_payload.viewport_x_px == 10);
    CHECK(high.last_payload.viewport_height_px == 720);
    CHECK((high.last_payload.flags &
           SAO_ENGINE_RENDER_DISPATCH_REDRAW_REQUESTED) == 0);

    order.clear();
    REQUIRE(sao_engine_render_clock_request_redraw(registry, "clock.hud") ==
            SAO_STATUS_OK);
    REQUIRE(sao_engine_runtime_render_dispatch(
                runtime, "clock.hud", SAO_ENGINE_RENDER_BEFORE_PRESENT,
                2'500'000u, 0, 0, 1920, 1080,
                SAO_ENGINE_RENDER_DISPATCH_COMPOSITOR_PRESENT) ==
            SAO_STATUS_OK);
    CHECK(order == std::vector<int>{2, 3});
    CHECK(high.last_payload.frame_index == 2);
    CHECK(high.last_payload.frame_delta_us == 1500);
    CHECK((high.last_payload.flags &
           SAO_ENGINE_RENDER_DISPATCH_REDRAW_REQUESTED) != 0);

    order.clear();
    REQUIRE(sao_engine_runtime_render_dispatch(
                runtime, "clock.other", SAO_ENGINE_RENDER_BEFORE_PRESENT,
                3'000'000u, 0, 0, 640, 480,
                SAO_ENGINE_RENDER_DISPATCH_LOGICAL_TICK) == SAO_STATUS_OK);
    CHECK(order == std::vector<int>{3});
    CHECK(sao_engine_runtime_render_dispatch(
              runtime, "clock.hud", SAO_ENGINE_RENDER_BEFORE_PRESENT,
              2'999'999u, 0, 0, 640, 480,
              SAO_ENGINE_RENDER_DISPATCH_LOGICAL_TICK) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    // Wave 17c: GPU_PRESENT dispatch reclassified from NOT_IMPLEMENTED to
    // CAPABILITY_MISSING — the real GPU-present hook still requires an
    // OS-side D3D11/DXGI provider that this registry never installed.
    CHECK(sao_engine_runtime_render_dispatch(
              runtime, "clock.hud", SAO_ENGINE_RENDER_BEFORE_PRESENT,
              4'000'000u, 0, 0, 640, 480,
              SAO_ENGINE_RENDER_DISPATCH_GPU_PRESENT) ==
          SAO_STATUS_ERR_CAPABILITY_MISSING);

    CHECK(sao_engine_render_clock_unregister(registry, low_token) ==
          SAO_STATUS_ERR_SUBSCRIPTION_GONE);
    REQUIRE(sao_engine_render_clock_unregister(registry, high_token) ==
            SAO_STATUS_OK);
    REQUIRE(sao_engine_render_clock_unregister(registry, wildcard_token) ==
            SAO_STATUS_OK);
    sao_engine_runtime_destroy(runtime);
}
