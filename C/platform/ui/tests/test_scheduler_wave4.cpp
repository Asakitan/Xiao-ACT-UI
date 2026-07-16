// Wave 4 tests for the scheduler first-implementable slice (G1.5a).
//
// Coverage:
//   * scheduler_60fps_hits_target       - 500ms run, tick count ~= 30 ± 5.
//   * scheduler_stats_report_drops      - slow callback bumps pressure floor.
//   * scheduler_stop_joins_cleanly      - stop returns promptly, no hang.
//
// The scheduler runs in its own std::thread with a QueryPerformanceCounter
// deadline.  Tests never call into Tk / GLFW; the tick callback is a pure
// atomic counter increment (or a std::this_thread::sleep_for for the drop
// case).

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/scheduler.h"
#include "sao/core/status.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// One-shot tick callback that increments a counter on every invocation.
void SAO_UI_CALL count_tick(double /*now_sec*/, void* user_data) {
    auto* counter = reinterpret_cast<std::atomic<uint64_t>*>(user_data);
    counter->fetch_add(1);
}

// "Animating" predicate — always true so idle throttle does not fire.
bool SAO_UI_CALL always_animating(void* /*user_data*/) {
    return true;
}

// Slow tick callback — sleeps to force wall-time pressure floor to climb.
void SAO_UI_CALL slow_tick(double /*now_sec*/, void* user_data) {
    auto* counter = reinterpret_cast<std::atomic<uint64_t>*>(user_data);
    counter->fetch_add(1);
    // 25ms is above the 12ms threshold that trips pressure floor to >=1.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

SaoSchedulerConfig make_test_config() {
    SaoSchedulerConfig cfg{};
    cfg.target_hz = 60;
    cfg.max_idle_skip_n = 4;
    cfg.engage_time_period = true;
    cfg.enable_pressure_floor = true;
    return cfg;
}

}  // namespace

TEST_CASE("scheduler_60fps_hits_target",
          "[ui][scheduler][wave4]") {
    const SaoSchedulerConfig cfg = make_test_config();
    sao_ui_scheduler_handle_t s = nullptr;

    REQUIRE(sao_ui_scheduler_create(&cfg, &s) == SAO_STATUS_OK);
    REQUIRE(s != nullptr);

    std::atomic<uint64_t> ticks{0};
    REQUIRE(sao_ui_scheduler_register(
        s, "counter", &count_tick, &always_animating, nullptr, &ticks) == SAO_STATUS_OK);

    REQUIRE(sao_ui_scheduler_start(s) == SAO_STATUS_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(sao_ui_scheduler_stop(s) == SAO_STATUS_OK);

    // 60 Hz over 500ms ≈ 30 ticks.  Allow ± 8 for startup / drift on a
    // CI host — 22..38 keeps false negatives sensible while still catching
    // gross regressions (e.g. 15.6 ms scheduler quantum without
    // timeBeginPeriod would drop this to ~10 ticks).
    const uint64_t observed = ticks.load();
    INFO("observed=" << observed);
    CHECK(observed >= 22u);
    CHECK(observed <= 38u);

    SaoSchedulerStats stats{};
    REQUIRE(sao_ui_scheduler_get_stats(s, &stats) == SAO_STATUS_OK);
    CHECK(stats.target_hz == 60);
    CHECK(stats.frame_count == observed);
    CHECK(stats.job_count == 1u);

    sao_ui_scheduler_destroy(s);
}

TEST_CASE("scheduler_stats_report_drops",
          "[ui][scheduler][wave4]") {
    const SaoSchedulerConfig cfg = make_test_config();
    sao_ui_scheduler_handle_t s = nullptr;

    REQUIRE(sao_ui_scheduler_create(&cfg, &s) == SAO_STATUS_OK);
    REQUIRE(s != nullptr);

    std::atomic<uint64_t> ticks{0};
    REQUIRE(sao_ui_scheduler_register(
        s, "slow", &slow_tick, &always_animating, nullptr, &ticks) == SAO_STATUS_OK);

    REQUIRE(sao_ui_scheduler_start(s) == SAO_STATUS_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Snapshot mid-run — slow tick keeps last_frame_ms above 12ms.
    SaoSchedulerStats mid{};
    REQUIRE(sao_ui_scheduler_get_stats(s, &mid) == SAO_STATUS_OK);
    CHECK(mid.frame_count > 0u);
    CHECK(mid.last_frame_ms > 0.0);

    REQUIRE(sao_ui_scheduler_stop(s) == SAO_STATUS_OK);

    SaoSchedulerStats final{};
    REQUIRE(sao_ui_scheduler_get_stats(s, &final) == SAO_STATUS_OK);
    CHECK(final.frame_count >= mid.frame_count);
    // The 25ms sleep in each tick trips the wall-time pressure floor
    // (>12ms → 1, >25ms borderline → often bumps to 2).  We only
    // require the floor advanced past 0 at some point.
    CHECK(final.wall_pressure_floor >= 1);
    // Idle skip must have followed since animating is forced true — but
    // pressure floor should have been observable in current_idle_skip_n
    // any time it exceeded 1.  Verify avg tick time reflects the sleep.
    CHECK(final.avg_frame_ms > 10.0);

    sao_ui_scheduler_destroy(s);
}

TEST_CASE("scheduler_stop_joins_cleanly",
          "[ui][scheduler][wave4]") {
    const SaoSchedulerConfig cfg = make_test_config();
    sao_ui_scheduler_handle_t s = nullptr;

    REQUIRE(sao_ui_scheduler_create(&cfg, &s) == SAO_STATUS_OK);
    REQUIRE(s != nullptr);

    std::atomic<uint64_t> ticks{0};
    REQUIRE(sao_ui_scheduler_register(
        s, "cnt", &count_tick, &always_animating, nullptr, &ticks) == SAO_STATUS_OK);

    REQUIRE(sao_ui_scheduler_start(s) == SAO_STATUS_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto t_stop_begin = std::chrono::steady_clock::now();
    REQUIRE(sao_ui_scheduler_stop(s) == SAO_STATUS_OK);
    const auto t_stop_end = std::chrono::steady_clock::now();
    const double stop_ms = std::chrono::duration<double, std::milli>(
        t_stop_end - t_stop_begin).count();
    // Stop must return within one full frame + slack (100ms is generous).
    INFO("stop_ms=" << stop_ms);
    CHECK(stop_ms < 100.0);

    // Ticks should not advance after stop returns.  Sample and re-sample.
    const uint64_t sample1 = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t sample2 = ticks.load();
    CHECK(sample1 == sample2);

    // Second stop is idempotent.
    REQUIRE(sao_ui_scheduler_stop(s) == SAO_STATUS_OK);

    sao_ui_scheduler_destroy(s);
}

TEST_CASE("scheduler_detect_refresh_hz_returns_valid_range",
          "[ui][scheduler][wave4]") {
    const int32_t hz = sao_ui_scheduler_detect_refresh_hz();
    // Header docs: clamps 60..240 Hz; 60 on failure.
    CHECK(hz >= 60);
    CHECK(hz <= 240);
}
