// Wave 3 tests for the NerveGear button first-implementable slice.
//
// Coverage (8 CASE):
//   * nervegear_starts_idle
//       Freshly created button reports IDLE.
//   * nervegear_hover_on_mouse_enter
//       on_mouse_enter transitions IDLE → HOVER.
//   * nervegear_press_release_starts_linking
//       HOVER → PRESSED (mouse down) → LINKING (mouse up).
//   * nervegear_link_progress_hits_1_at_3000ms
//       Using a custom 3s timeline, ticking 30×100ms drives progress
//       to 1.0 exactly.
//   * nervegear_linking_to_linked_at_full_progress
//       LINKING transitions to LINKED at progress≥1 during tick.
//   * nervegear_hit_test_inside_circle
//       Point inside the disc (r < kHitRadius) returns hit=true.
//   * nervegear_hit_test_outside_circle_misses
//       Corner of the 72×72 sprite bbox returns hit=false.
//   * nervegear_event_handler_fires_on_state_change
//       Registered callback receives HOVER_ENTER on mouse_enter and
//       LEFT_CLICK + LINK_STARTED on mouse_up.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/nervegear.h"

// ── Prototypes for the wave3 helper API implemented in nervegear.cpp
//    but not in nervegear.h.
extern "C" {

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_tick(
    sao_ui_nervegear_handle_t handle, int32_t dt_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_get_link_progress(
    sao_ui_nervegear_handle_t handle, float* out_progress);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_hit_test(
    sao_ui_nervegear_handle_t handle,
    int32_t px, int32_t py, bool* out_hit);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_on_mouse_enter(
    sao_ui_nervegear_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_on_mouse_leave(
    sao_ui_nervegear_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_on_mouse_down(
    sao_ui_nervegear_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_nervegear_on_mouse_up(
    sao_ui_nervegear_handle_t handle);

}  // extern "C"

namespace {

// Position the disc at (100, 100) so hit-test coordinates fit within
// a manageable integer range and don't tempt sign errors.
constexpr int32_t kInitX = 100;
constexpr int32_t kInitY = 100;

sao_ui_nervegear_handle_t make_button() {
    sao_ui_nervegear_handle_t btn = nullptr;
    const auto rc = sao_ui_nervegear_create(nullptr, nullptr,
                                            kInitX, kInitY,
                                            SAO_UI_NG_PALETTE_DARK, &btn);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(btn != nullptr);
    return btn;
}

// ── Callback capture ──────────────────────────────────────────────
struct EventRecord {
    SaoUiNerveGearEvent event;
    int32_t x, y;
};

struct EventSink {
    std::mutex               mtx;
    std::vector<EventRecord> events;
};

void SAO_UI_CALL capture_event(SaoUiNerveGearEvent event,
                               int32_t x, int32_t y, void* user_data) {
    auto* sink = static_cast<EventSink*>(user_data);
    if (sink == nullptr) return;
    std::lock_guard<std::mutex> lock(sink->mtx);
    sink->events.push_back(EventRecord{event, x, y});
}

}  // namespace

TEST_CASE("nervegear_starts_idle", "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    SaoUiNerveGearState st = SAO_UI_NG_STATE_LOGOUT;   // sentinel
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_IDLE);

    // Position round-trip.
    int32_t rx = 0, ry = 0;
    REQUIRE(sao_ui_nervegear_get_position(btn, &rx, &ry) == SAO_STATUS_OK);
    CHECK(rx == kInitX);
    CHECK(ry == kInitY);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_hover_on_mouse_enter", "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    REQUIRE(sao_ui_nervegear_on_mouse_enter(btn) == SAO_STATUS_OK);

    SaoUiNerveGearState st = SAO_UI_NG_STATE_IDLE;
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_HOVER);

    // Leaving flips back to IDLE.
    REQUIRE(sao_ui_nervegear_on_mouse_leave(btn) == SAO_STATUS_OK);
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_IDLE);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_press_release_starts_linking", "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    REQUIRE(sao_ui_nervegear_on_mouse_enter(btn) == SAO_STATUS_OK);
    REQUIRE(sao_ui_nervegear_on_mouse_down(btn) == SAO_STATUS_OK);

    SaoUiNerveGearState st = SAO_UI_NG_STATE_IDLE;
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_PRESSED);

    // Release → LINKING.
    REQUIRE(sao_ui_nervegear_on_mouse_up(btn) == SAO_STATUS_OK);
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_LINKING);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_link_progress_hits_1_at_3000ms",
          "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    // Custom 3s timeline so 30×100ms ticks == exactly 1.0 progress.
    // The default 10s timeline is documented on the header; this test
    // exercises the set_timeline override path.
    SaoUiLinkStartTimeline tl = *sao_ui_nervegear_default_timeline();
    tl.total_duration = 3.0f;    // 3 seconds
    REQUIRE(sao_ui_nervegear_set_timeline(btn, &tl) == SAO_STATUS_OK);

    // Drive IDLE → LINKING via the forced-transition path (the header
    // documents IDLE → LINKING as a legal forced target).
    REQUIRE(sao_ui_nervegear_transition(btn, SAO_UI_NG_STATE_LINKING)
            == SAO_STATUS_OK);

    // Advance 30 × 100 ms = 3000 ms.  Progress should sweep 0 → 1.
    float p = -1.0f;
    for (int i = 0; i < 29; ++i) {
        REQUIRE(sao_ui_nervegear_tick(btn, 100) == SAO_STATUS_OK);
        REQUIRE(sao_ui_nervegear_get_link_progress(btn, &p) == SAO_STATUS_OK);
        INFO("tick #" << (i + 1) << " progress=" << p);
        // Roughly (i+1)/30 — allow float slop.
        const float expected = static_cast<float>(i + 1) / 30.0f;
        CHECK(p > (expected - 0.02f));
        CHECK(p < (expected + 0.02f));
    }
    // Final tick — progress must reach 1.0 (state advances to LINKED,
    // which the next test case verifies).
    REQUIRE(sao_ui_nervegear_tick(btn, 100) == SAO_STATUS_OK);
    REQUIRE(sao_ui_nervegear_get_link_progress(btn, &p) == SAO_STATUS_OK);
    CHECK(p >= 0.999f);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_linking_to_linked_at_full_progress",
          "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    SaoUiLinkStartTimeline tl = *sao_ui_nervegear_default_timeline();
    tl.total_duration = 3.0f;
    REQUIRE(sao_ui_nervegear_set_timeline(btn, &tl) == SAO_STATUS_OK);

    REQUIRE(sao_ui_nervegear_transition(btn, SAO_UI_NG_STATE_LINKING)
            == SAO_STATUS_OK);

    // Tick a bit under the total, then over: state should flip on the
    // tick that crosses the threshold.
    REQUIRE(sao_ui_nervegear_tick(btn, 2000) == SAO_STATUS_OK);
    SaoUiNerveGearState st = SAO_UI_NG_STATE_IDLE;
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_LINKING);

    REQUIRE(sao_ui_nervegear_tick(btn, 1500) == SAO_STATUS_OK);  // now 3500 ms
    REQUIRE(sao_ui_nervegear_get_state(btn, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_NG_STATE_LINKED);

    // In LINKED, progress reports 1.0.
    float p = 0.0f;
    REQUIRE(sao_ui_nervegear_get_link_progress(btn, &p) == SAO_STATUS_OK);
    CHECK(p == 1.0f);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_hit_test_inside_circle", "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    // The disc centre = (kInitX + 36, kInitY + 36) = (136, 136).
    // Any pixel within ~30 px of centre is inside the visible disc.
    bool hit = false;
    REQUIRE(sao_ui_nervegear_hit_test(btn, 136, 136, &hit) == SAO_STATUS_OK);
    CHECK(hit);

    REQUIRE(sao_ui_nervegear_hit_test(btn, 136 + 20, 136 + 10, &hit)
            == SAO_STATUS_OK);
    CHECK(hit);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_hit_test_outside_circle_misses", "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    // Corner of the 72×72 sprite: (100, 100) is the top-left of the
    // bbox, well outside the visible disc (which is centred at 136,136
    // with a ~30 px radius).  Must return hit=false.
    bool hit = true;
    REQUIRE(sao_ui_nervegear_hit_test(btn, 100, 100, &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);

    // Bottom-right corner (172, 172) — same story.
    hit = true;
    REQUIRE(sao_ui_nervegear_hit_test(btn, 172, 172, &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);

    // Well outside the bbox — obviously miss.
    hit = true;
    REQUIRE(sao_ui_nervegear_hit_test(btn, 0, 0, &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);

    sao_ui_nervegear_destroy(btn);
}

TEST_CASE("nervegear_event_handler_fires_on_state_change",
          "[ui][nervegear][wave3]") {
    sao_ui_nervegear_handle_t btn = make_button();

    EventSink sink;
    REQUIRE(sao_ui_nervegear_set_event_callback(btn, &capture_event, &sink)
            == SAO_STATUS_OK);

    // Enter → HOVER_ENTER fires.
    REQUIRE(sao_ui_nervegear_on_mouse_enter(btn) == SAO_STATUS_OK);
    {
        std::lock_guard<std::mutex> lock(sink.mtx);
        REQUIRE(sink.events.size() == 1);
        CHECK(sink.events[0].event == SAO_UI_NG_EV_HOVER_ENTER);
    }

    // Down → no event fires (PRESSED is internal).
    REQUIRE(sao_ui_nervegear_on_mouse_down(btn) == SAO_STATUS_OK);
    {
        std::lock_guard<std::mutex> lock(sink.mtx);
        CHECK(sink.events.size() == 1);
    }

    // Up → LEFT_CLICK + LINK_STARTED fire in that order.
    REQUIRE(sao_ui_nervegear_on_mouse_up(btn) == SAO_STATUS_OK);
    {
        std::lock_guard<std::mutex> lock(sink.mtx);
        REQUIRE(sink.events.size() == 3);
        CHECK(sink.events[1].event == SAO_UI_NG_EV_LEFT_CLICK);
        CHECK(sink.events[2].event == SAO_UI_NG_EV_LINK_STARTED);
    }

    sao_ui_nervegear_destroy(btn);
}
