// SAO Auto — platform/engine/tests/test_trigger_engine_wave6.cpp
//
// Wave 6 / Phase 5 — game-agnostic trigger engine coverage.
//
// Six scenarios exercise the built-in condition families without
// referring to any specific game concept:
//   * event_match with payload substring filter
//   * timer tick fires periodically until unregistered
//   * combo of event_match AND state_enter
//   * register / unregister lifecycle round trip
//   * evaluate() reports action ids in registration order
//   * tick() one_shot latches after firing once
//
// The trigger specs pass condition_params as compact JSON strings;
// the engine itself remains ignorant of any game-specific vocabulary.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
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
    REQUIRE(unregister.wait_for(1s) == std::future_status::ready);
    REQUIRE(unregister.get() == SAO_STATUS_OK);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.released = true;
        state.condition.notify_all();
    }
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

TEST_CASE("trigger_callback_can_unregister_itself",
          "[engine][trigger][wave6][lifecycle]") {
    sao_engine_trigger_engine_handle_t eng = nullptr;
    REQUIRE(sao_engine_trigger_create(&eng) == SAO_STATUS_OK);

    UnregisteringCallbackState state;
    state.engine = eng;
    auto spec = makeSpec(SAO_ENGINE_TRIGGER_EVENT_MATCH,
                         R"({"event_type": "topic.self_unregister"})",
                         4200);
    REQUIRE(sao_engine_trigger_register(
                eng, &spec, &unregisteringCallback, &state, &state.trigger) ==
            SAO_STATUS_OK);

    SaoEngineTriggerEvent event{};
    event.event_type_utf8 = "topic.self_unregister";
    uint64_t action_id = 0;
    uint32_t count = 0;
    REQUIRE(sao_engine_trigger_evaluate(
                eng, &event, &action_id, 1, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(action_id == 4200);
    REQUIRE(state.calls == 1);
    REQUIRE(state.unregister_status == SAO_STATUS_OK);

    REQUIRE(sao_engine_trigger_evaluate(
                eng, &event, &action_id, 1, &count) == SAO_STATUS_OK);
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
