// SAO Auto — platform/engine/tests/test_trigger_engine_wave6.cpp
//
// Wave 6 / Phase 5 — game-agnostic trigger engine coverage.
//
// Twelve scenarios exercise the built-in condition families without
// referring to any specific game concept:
//   * event_match with payload substring filter
//   * timer tick fires periodically until unregistered
//   * combo of event_match AND state_enter
//   * register / unregister lifecycle round trip
//   * concurrent unregister while a callback is in flight
//   * callback-triggered unregister during timer dispatch
//   * timer interval validation at numeric boundaries
//   * concurrent tick serialization for one trigger
//   * throwing callback isolation and continued dispatch
//   * UINT64_MAX tick advancement in constant time
//   * evaluate() reports action ids in registration order
//   * tick() one_shot latches after firing once
//
// The trigger specs pass condition_params as compact JSON strings;
// the engine itself remains ignorant of any game-specific vocabulary.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sao/engine/trigger_engine.h"

namespace {

struct CallbackRecord {
    std::vector<uint64_t> action_ids;
};

struct BlockingCallbackState {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool released{false};
};

struct UnregisteringCallbackState {
    sao_engine_trigger_engine_handle_t engine = nullptr;
    sao_engine_trigger_handle_t trigger = 0;
    sao_status_t unregister_status = SAO_STATUS_ERR_UNKNOWN;
    uint32_t calls = 0;
};

struct CountingCallbackState {
    std::atomic_uint32_t calls{0};
};

struct DispatchResult {
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    uint64_t action_id = 0;
    uint32_t count = 0;
};

struct CallbackDispatchResult {
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    std::array<uint64_t, 2> action_ids{};
    uint32_t count = 0;
};

void SAO_ENGINE_CALL captureCallback(uint64_t action_id,
                                     const SaoEngineTriggerEvent* /*event*/,
                                     void* user_data) {
    auto* rec = static_cast<CallbackRecord*>(user_data);
    rec->action_ids.push_back(action_id);
}

void SAO_ENGINE_CALL blockingCallback(uint64_t,
                                      const SaoEngineTriggerEvent*,
                                      void* user_data) {
    auto* state = static_cast<BlockingCallbackState*>(user_data);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state] { return state->released; });
}

void SAO_ENGINE_CALL unregisteringCallback(uint64_t,
                                           const SaoEngineTriggerEvent*,
                                           void* user_data) {
    auto* state = static_cast<UnregisteringCallbackState*>(user_data);
    ++state->calls;
    state->unregister_status =
        sao_engine_trigger_unregister(state->engine, state->trigger);
}

void SAO_ENGINE_CALL countingCallback(uint64_t,
                                      const SaoEngineTriggerEvent*,
                                      void* user_data) {
    auto* state = static_cast<CountingCallbackState*>(user_data);
    state->calls.fetch_add(1, std::memory_order_relaxed);
}

void SAO_ENGINE_CALL throwingCallback(uint64_t,
                                      const SaoEngineTriggerEvent*,
                                      void*) {
    throw std::runtime_error("trigger callback fixture");
}

SaoEngineTriggerSpec makeSpec(int32_t type, const char* params_json,
                              uint64_t action_id) {
    SaoEngineTriggerSpec spec{};
    spec.condition_type        = type;
    spec.condition_params_json = params_json;
    spec.action_id             = action_id;
    return spec;
}

}  // namespace

TEST_CASE("trigger_event_match_fires_on_payload_substring",
          "[engine][trigger][wave6]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    CallbackRecord rec;
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                         R"({"event_type": "topic.a", "payload_contains": ["needle"]})",
                         1001);
    sao_engine_trigger_handle_t handle = 0;
    REQUIRE(sao_engine_trigger_register(eng, &spec, &captureCallback, &rec, &handle)
            == SAO_STATUS_OK);
    REQUIRE(handle != 0);

    SaoEngineTriggerEvent evt{};
    evt.event_type_utf8 = "topic.a";
    evt.payload_utf8    = "haystack with needle inside";
    evt.timestamp_ms    = 500;

    uint64_t buf[4] = {0};
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(buf[0] == 1001);
    REQUIRE(rec.action_ids.size() == 1);
    REQUIRE(rec.action_ids[0] == 1001);

    // A payload without "needle" must not fire.
    evt.payload_utf8 = "innocuous text";
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_timer_ticks_periodically",
          "[engine][trigger][wave6]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    auto spec = makeSpec(SAO_ENGINE_TRIGGER_TIMER,
                         R"({"interval_ms": 1000})",
                         2002);
    sao_engine_trigger_handle_t handle = 0;
    REQUIRE(sao_engine_trigger_register(eng, &spec, nullptr, nullptr, &handle)
            == SAO_STATUS_OK);

    uint64_t buf[4] = {0};
    uint32_t count = 0;
    // 500 ms elapsed — timer does not fire yet.
    REQUIRE(sao_engine_trigger_tick(eng, 500, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);
    // Another 500 ms — timer crosses 1000 ms and fires.
    REQUIRE(sao_engine_trigger_tick(eng, 500, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(buf[0] == 2002);

    // Another full interval — fires again.
    REQUIRE(sao_engine_trigger_tick(eng, 1000, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_timer_rejects_non_integer_non_finite_and_out_of_range_intervals",
          "[engine][trigger][wave6][timer]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    const std::vector<const char*> invalid_params = {
        R"({"interval_ms": 0.5})",
        R"({"interval_ms": NaN})",
        R"({"interval_ms": Infinity})",
        R"({"interval_ms": 1e999})",
        R"({"interval_ms": 18446744073709551616})",
    };
    for (const auto* params : invalid_params) {
        CAPTURE(params);
        auto spec = makeSpec(SAO_ENGINE_TRIGGER_TIMER, params, 2003);
        sao_engine_trigger_handle_t trigger = 0;
        REQUIRE(sao_engine_trigger_register(
                    eng, &spec, nullptr, nullptr, &trigger) ==
                SAO_STATUS_ERR_INVALID_ARGUMENT);
        REQUIRE(trigger == 0);
    }

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_concurrent_ticks_serialize_condition_state",
          "[engine][trigger][wave6][timer][concurrency]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    CountingCallbackState state;
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_TIMER,
                         R"({"interval_ms": 2})", 2100);
    sao_engine_trigger_handle_t trigger = 0;
    REQUIRE(sao_engine_trigger_register(
                eng, &spec, &countingCallback, &state, &trigger) ==
            SAO_STATUS_OK);

    std::barrier start{3};
    const auto tick = [&] {
        start.arrive_and_wait();
        DispatchResult result;
        result.status = sao_engine_trigger_tick(
            eng, 1, &result.action_id, 1, &result.count);
        return result;
    };
    auto first = std::async(std::launch::async, tick);
    auto second = std::async(std::launch::async, tick);
    start.arrive_and_wait();

    const auto first_result = first.get();
    const auto second_result = second.get();
    REQUIRE(first_result.status == SAO_STATUS_OK);
    REQUIRE(second_result.status == SAO_STATUS_OK);
    CHECK(first_result.count + second_result.count == 1);
    if (first_result.count == 1) CHECK(first_result.action_id == 2100);
    if (second_result.count == 1) CHECK(second_result.action_id == 2100);
    CHECK(state.calls.load(std::memory_order_relaxed) == 1);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_throwing_callback_is_contained_and_dispatch_continues",
          "[engine][trigger][wave6][callback][exception]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    auto throwing_spec = makeSpec(
        SAO_ENGINE_TRIGGER_EVENT_MATCH,
        R"({"event_type": "topic.throwing"})", 2200);
    auto continuing_spec = makeSpec(
        SAO_ENGINE_TRIGGER_EVENT_MATCH,
        R"({"event_type": "topic.throwing"})", 2201);
    sao_engine_trigger_handle_t throwing_trigger = 0;
    sao_engine_trigger_handle_t continuing_trigger = 0;
    CountingCallbackState continuing_state;
    REQUIRE(sao_engine_trigger_register(
                eng, &throwing_spec, &throwingCallback, nullptr,
                &throwing_trigger) == SAO_STATUS_OK);
    REQUIRE(sao_engine_trigger_register(
                eng, &continuing_spec, &countingCallback, &continuing_state,
                &continuing_trigger) == SAO_STATUS_OK);

    auto evaluation = std::async(std::launch::async, [eng] {
        SaoEngineTriggerEvent event{};
        event.event_type_utf8 = "topic.throwing";
        CallbackDispatchResult result;
        result.status = sao_engine_trigger_evaluate(
            eng, &event, result.action_ids.data(),
            static_cast<uint32_t>(result.action_ids.size()), &result.count);
        return result;
    });
    CallbackDispatchResult result;
    REQUIRE_NOTHROW(result = evaluation.get());
    CHECK(result.status == SAO_STATUS_ERR_UNKNOWN);
    CHECK(result.count == 2);
    CHECK((result.action_ids == std::array<uint64_t, 2>{2200, 2201}));
    CHECK(continuing_state.calls.load(std::memory_order_relaxed) == 1);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_timer_handles_uint64_max_tick_without_loop_or_wrap",
          "[engine][trigger][wave6][timer][overflow]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    CountingCallbackState state;
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_TIMER,
                         R"({"interval_ms": 3})", 2300);
    sao_engine_trigger_handle_t trigger = 0;
    REQUIRE(sao_engine_trigger_register(
                eng, &spec, &countingCallback, &state, &trigger) ==
            SAO_STATUS_OK);

    uint64_t action_id = 0;
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_tick(
                eng, (std::numeric_limits<uint64_t>::max)(), &action_id, 1,
                &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    CHECK(action_id == 2300);
    CHECK(state.calls.load(std::memory_order_relaxed) == 1);

    REQUIRE(sao_engine_trigger_tick(eng, 2, &action_id, 1, &count) ==
            SAO_STATUS_OK);
    CHECK(count == 0);
    REQUIRE(sao_engine_trigger_tick(eng, 1, &action_id, 1, &count) ==
            SAO_STATUS_OK);
    CHECK(count == 1);
    CHECK(state.calls.load(std::memory_order_relaxed) == 2);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_combo_all_children_must_match",
          "[engine][trigger][wave6]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    // event_match AND state_enter — both must be true on the same event.
    const char* combo_params = R"({
        "conditions": [
            {"condition_type": 1, "condition_params": {"event_type": "boss.event"}},
            {"condition_type": 2, "condition_params": {"state": "phase_two"}}
        ]
    })";
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_COMBO, combo_params, 3003);
    sao_engine_trigger_handle_t handle = 0;
    REQUIRE(sao_engine_trigger_register(eng, &spec, nullptr, nullptr, &handle)
            == SAO_STATUS_OK);

    uint64_t buf[4] = {0};
    uint32_t count = 0;

    // Right event, wrong state — no fire.
    SaoEngineTriggerEvent evt{};
    evt.event_type_utf8 = "boss.event";
    evt.state_to_utf8   = "phase_one";
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);

    // Right event, right state — combo fires.
    evt.state_to_utf8 = "phase_two";
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(buf[0] == 3003);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_combo_timer_children_are_order_independent",
          "[engine][trigger][wave6][timer][combo]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    const char* slow_then_fast = R"({
        "conditions": [
            {"condition_type": 5, "condition_params": {"interval_ms": 200}},
            {"condition_type": 5, "condition_params": {"interval_ms": 100}}
        ]
    })";
    const char* fast_then_slow = R"({
        "conditions": [
            {"condition_type": 5, "condition_params": {"interval_ms": 100}},
            {"condition_type": 5, "condition_params": {"interval_ms": 200}}
        ]
    })";
    sao_engine_trigger_handle_t slow_first_handle = 0;
    sao_engine_trigger_handle_t fast_first_handle = 0;
    auto slow_first = makeSpec(SAO_ENGINE_TRIGGER_COMBO, slow_then_fast, 6101);
    auto fast_first = makeSpec(SAO_ENGINE_TRIGGER_COMBO, fast_then_slow, 6102);
    REQUIRE(sao_engine_trigger_register(eng, &slow_first, nullptr, nullptr,
                                        &slow_first_handle) == SAO_STATUS_OK);
    REQUIRE(sao_engine_trigger_register(eng, &fast_first, nullptr, nullptr,
                                        &fast_first_handle) == SAO_STATUS_OK);

    uint64_t actions[4] = {};
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_tick(eng, 100, actions, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);

    REQUIRE(sao_engine_trigger_tick(eng, 100, actions, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 2);
    CHECK(actions[0] == 6101);
    CHECK(actions[1] == 6102);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_unregister_removes_from_dispatch",
          "[engine][trigger][wave6]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    auto spec = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                         R"({"event_type": "topic.gone"})",
                         4004);
    sao_engine_trigger_handle_t handle = 0;
    REQUIRE(sao_engine_trigger_register(eng, &spec, nullptr, nullptr, &handle)
            == SAO_STATUS_OK);

    SaoEngineTriggerEvent evt{};
    evt.event_type_utf8 = "topic.gone";
    uint64_t buf[4] = {0};
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);

    // Unregister and re-evaluate.
    REQUIRE(sao_engine_trigger_unregister(eng, handle) == SAO_STATUS_OK);
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);

    // Double unregister -> NOT_FOUND (not a crash).
    REQUIRE(sao_engine_trigger_unregister(eng, handle) == SAO_STATUS_ERR_NOT_FOUND);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_unregister_keeps_in_flight_callback_storage_alive",
          "[engine][trigger][wave6][concurrency]") {
    using namespace std::chrono_literals;

    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    BlockingCallbackState state;
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                         R"({"event_type": "topic.concurrent"})",
                         4100);
    sao_engine_trigger_handle_t trigger = 0;
    REQUIRE(sao_engine_trigger_register(
                eng, &spec, &blockingCallback, &state, &trigger) ==
            SAO_STATUS_OK);

    auto evaluator = std::async(std::launch::async, [eng] {
        SaoEngineTriggerEvent event{};
        event.event_type_utf8 = "topic.concurrent";
        uint64_t action_id = 0;
        uint32_t count = 0;
        const auto status =
            sao_engine_trigger_evaluate(eng, &event, &action_id, 1, &count);
        return std::make_pair(status, count);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait(lock, [&state] { return state.entered; });
    }

    auto unregister = std::async(std::launch::async, [eng, trigger] {
        return sao_engine_trigger_unregister(eng, trigger);
    });
    const auto unregister_wait = unregister.wait_for(1s);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.released = true;
        state.condition.notify_all();
    }
    REQUIRE(unregister_wait == std::future_status::ready);
    REQUIRE(unregister.get() == SAO_STATUS_OK);
    const auto [status, count] = evaluator.get();
    REQUIRE(status == SAO_STATUS_OK);
    REQUIRE(count == 1);

    SaoEngineTriggerEvent event{};
    event.event_type_utf8 = "topic.concurrent";
    uint32_t next_count = 0;
    REQUIRE(sao_engine_trigger_evaluate(
                eng, &event, nullptr, 0, &next_count) == SAO_STATUS_OK);
    REQUIRE(next_count == 0);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_timer_callback_can_unregister_itself",
          "[engine][trigger][wave6][timer][lifecycle]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    UnregisteringCallbackState state;
    state.engine = eng;
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_TIMER,
                         R"({"interval_ms": 1})", 4200);
    REQUIRE(sao_engine_trigger_register(
                eng, &spec, &unregisteringCallback, &state, &state.trigger) ==
            SAO_STATUS_OK);

    uint64_t action_id = 0;
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_tick(eng, 1, &action_id, 1, &count) ==
            SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(action_id == 4200);
    REQUIRE(state.calls == 1);
    REQUIRE(state.unregister_status == SAO_STATUS_OK);

    REQUIRE(sao_engine_trigger_tick(eng, 1, &action_id, 1, &count) ==
            SAO_STATUS_OK);
    REQUIRE(count == 0);
    REQUIRE(state.calls == 1);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_evaluate_reports_action_ids_in_registration_order",
          "[engine][trigger][wave6]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    // Register three event_match triggers on the same topic in a
    // specific order — evaluate must emit action_ids in that order.
    auto s1 = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                       R"({"event_type": "topic.x"})", 100);
    auto s2 = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                       R"({"event_type": "topic.x"})", 200);
    auto s3 = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                       R"({"event_type": "topic.x"})", 300);
    sao_engine_trigger_handle_t h1 = 0, h2 = 0, h3 = 0;
    REQUIRE(sao_engine_trigger_register(eng, &s1, nullptr, nullptr, &h1) == SAO_STATUS_OK);
    REQUIRE(sao_engine_trigger_register(eng, &s2, nullptr, nullptr, &h2) == SAO_STATUS_OK);
    REQUIRE(sao_engine_trigger_register(eng, &s3, nullptr, nullptr, &h3) == SAO_STATUS_OK);

    SaoEngineTriggerEvent evt{};
    evt.event_type_utf8 = "topic.x";
    uint64_t buf[8] = {0};
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_evaluate(eng, &evt, buf, 8, &count) == SAO_STATUS_OK);
    REQUIRE(count == 3);
    REQUIRE(buf[0] == 100);
    REQUIRE(buf[1] == 200);
    REQUIRE(buf[2] == 300);

    sao_engine_trigger_destroy(eng);
}

TEST_CASE("trigger_timer_one_shot_fires_only_once",
          "[engine][trigger][wave6]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    auto spec = makeSpec(SAO_ENGINE_TRIGGER_TIMER,
                         R"({"interval_ms": 500, "one_shot": true})",
                         5005);
    sao_engine_trigger_handle_t handle = 0;
    REQUIRE(sao_engine_trigger_register(eng, &spec, nullptr, nullptr, &handle)
            == SAO_STATUS_OK);

    uint64_t buf[4] = {0};
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_tick(eng, 250, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);
    // Cross the interval — fires.
    REQUIRE(sao_engine_trigger_tick(eng, 300, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(buf[0] == 5005);

    // Subsequent ticks must not re-fire even if elapsed >> interval.
    REQUIRE(sao_engine_trigger_tick(eng, 5000, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);
    REQUIRE(sao_engine_trigger_tick(eng, 5000, buf, 4, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);

    sao_engine_trigger_destroy(eng);
}
