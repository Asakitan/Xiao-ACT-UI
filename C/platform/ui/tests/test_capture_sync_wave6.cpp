// Wave 6 / Agent e tests for the render_capture_sync first slice.
//
// Coverage (3 test cases):
//   * capture_sync_begin_end_depth       — reentrant depth increments
//     match Python `begin_capture()`; `end_capture()` clamps at zero;
//     `is_active()` reflects depth > 0.
//   * capture_sync_wait_idle_blocks_and_wakes
//     — a background thread holding a capture keeps the waiter
//     blocked; releasing it flips `wait_until_idle` from timeout to
//     OK.  This is the "1-2 FPS under recognition load" contract.
//   * capture_sync_wait_idle_timeout      — with zero / positive
//     timeout when active, the waiter returns ERR_TIMEOUT rather
//     than blocking forever.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/capture_sync.h"
#include "sao/core/status.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// Helper: drive the process-wide state back to depth==0 so tests do
// not observe each other's captures.
void drain_capture_state() {
    while (sao_ui_capture_sync_is_active()) {
        sao_ui_capture_sync_end();
    }
}

}  // namespace

TEST_CASE("capture_sync_begin_end_depth", "[ui][capture_sync][wave6]") {
    drain_capture_state();
    REQUIRE_FALSE(sao_ui_capture_sync_is_active());
    REQUIRE(sao_ui_capture_sync_depth() == 0u);

    REQUIRE(sao_ui_capture_sync_begin() == SAO_STATUS_OK);
    CHECK(sao_ui_capture_sync_is_active());
    CHECK(sao_ui_capture_sync_depth() == 1u);

    REQUIRE(sao_ui_capture_sync_begin() == SAO_STATUS_OK);
    CHECK(sao_ui_capture_sync_depth() == 2u);
    CHECK(sao_ui_capture_sync_is_active());

    REQUIRE(sao_ui_capture_sync_end() == SAO_STATUS_OK);
    CHECK(sao_ui_capture_sync_depth() == 1u);
    CHECK(sao_ui_capture_sync_is_active());

    REQUIRE(sao_ui_capture_sync_end() == SAO_STATUS_OK);
    CHECK(sao_ui_capture_sync_depth() == 0u);
    CHECK_FALSE(sao_ui_capture_sync_is_active());

    // Extra end() must not underflow — matches Python `if depth > 0`.
    REQUIRE(sao_ui_capture_sync_end() == SAO_STATUS_OK);
    CHECK(sao_ui_capture_sync_depth() == 0u);
}

TEST_CASE("capture_sync_wait_idle_blocks_and_wakes",
          "[ui][capture_sync][wave6]") {
    drain_capture_state();

    REQUIRE(sao_ui_capture_sync_begin() == SAO_STATUS_OK);
    REQUIRE(sao_ui_capture_sync_is_active());

    // Immediate poll (timeout=0) must fail with TIMEOUT while active.
    REQUIRE(sao_ui_capture_sync_wait_until_idle(0.0) ==
            SAO_STATUS_ERR_TIMEOUT);

    // Background waiter blocks; foreground releases after ~50 ms.
    std::atomic<int32_t> waiter_result{123};
    std::thread waiter([&waiter_result]() {
        // 2-second timeout is generous — real release fires ~50 ms.
        waiter_result.store(
            sao_ui_capture_sync_wait_until_idle(2.0));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(sao_ui_capture_sync_end() == SAO_STATUS_OK);
    CHECK_FALSE(sao_ui_capture_sync_is_active());

    waiter.join();
    CHECK(waiter_result.load() == SAO_STATUS_OK);
}

TEST_CASE("capture_sync_wait_idle_timeout", "[ui][capture_sync][wave6]") {
    drain_capture_state();

    REQUIRE(sao_ui_capture_sync_begin() == SAO_STATUS_OK);
    REQUIRE(sao_ui_capture_sync_is_active());

    // 30 ms timeout — no release, so must return TIMEOUT promptly.
    const auto t0 = std::chrono::steady_clock::now();
    const auto st = sao_ui_capture_sync_wait_until_idle(0.030);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    REQUIRE(st == SAO_STATUS_ERR_TIMEOUT);
    // Allow a generous upper bound to absorb CI jitter.
    CHECK(elapsed_ms < 500);

    // Clean up.
    REQUIRE(sao_ui_capture_sync_end() == SAO_STATUS_OK);
    CHECK_FALSE(sao_ui_capture_sync_is_active());
}
