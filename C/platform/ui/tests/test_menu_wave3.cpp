// Wave 3 tests for the SAO main menu first-implementable slice.
//
// Coverage (7 CASE):
//   * menu_create_ring_mode_layout_correct
//       6 buttons on a ring; button 0 sits at 12 o'clock, y ≈
//       center_y - outer_radius (± button_size/2 slop for the bbox top).
//   * menu_hit_test_ring_center_returns_no_button
//       Clicking dead-centre of the ring must return -1 (that's the
//       NerveGear button real estate, not a menu item).
//   * menu_hit_test_ring_button_position_returns_correct_index
//       Cursor placed at each button's centre must round-trip through
//       the hit-test back to the same index.
//   * menu_show_transitions_to_opening
//       show() from CLOSED goes to OPENING (not straight to OPEN).
//   * menu_tick_progresses_to_open_after_450ms
//       OPENING+tick(450ms) → OPEN.
//   * menu_set_button_state_hover_and_query
//       set_button_state(HOVER) round-trips through get_button_state.
//   * menu_hide_transitions_to_closing
//       hide() from OPEN goes to CLOSING (not straight to CLOSED).
//
// All tests are pure state-machine + geometry — no compositor, no
// overlay host.  The compositor handle is passed as nullptr because
// the first slice tolerates it.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "sao/core/status.h"
#include "sao/ui/menu.h"

// ── Prototypes for the wave3 helper API implemented in menu.cpp but
//    not in menu.h (see comment block at the bottom of menu.cpp).
extern "C" {

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_compute_button_layout(
    sao_ui_menu_handle_t handle,
    int32_t button_index,
    int32_t* out_x, int32_t* out_y,
    int32_t* out_w, int32_t* out_h);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_tick(
    sao_ui_menu_handle_t handle, int32_t dt_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_set_button_state(
    sao_ui_menu_handle_t handle,
    int32_t button_index,
    SaoUiMenuBtnState state);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_get_button_state(
    sao_ui_menu_handle_t handle,
    int32_t button_index,
    SaoUiMenuBtnState* out_state);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_dispatch_event(
    sao_ui_menu_handle_t handle,
    SaoUiMenuEvent event_type,
    const void* data_ptr);

}  // extern "C"

namespace {

// A canonical 6-item ring — matches the SAO NerveGear default menu
// (party / social / options / …), no icons, all activateable.
constexpr size_t kRingCount = 6;
const SaoUiMenuItem kRingItems[kRingCount] = {
    {"one",   "1", 1, true, {0,0,0}},
    {"two",   "2", 2, true, {0,0,0}},
    {"three", "3", 3, true, {0,0,0}},
    {"four",  "4", 4, true, {0,0,0}},
    {"five",  "5", 5, true, {0,0,0}},
    {"six",   "6", 6, true, {0,0,0}},
};

// Layout deliberately picked so buttons don't overlap and the ring
// clearly sits above the centre for a Y-down coordinate system.
SaoUiMenuLayout make_ring_layout() {
    SaoUiMenuLayout l{};
    l.center_x          = 400;
    l.center_y          = 300;
    l.inner_radius      = 60;
    l.outer_radius      = 200;   // wide enough that adjacent buttons
    l.child_ring_radius = 300;   // (60° apart at r=200) sit ~200 px apart
    l.button_size       = 54;
    l.button_max_size   = 70;
    l.slot_size         = 70;
    l.max_visible       = 9;
    return l;
}

// Convenience: build a fully-populated RING menu with 6 items.
sao_ui_menu_handle_t make_ring_menu() {
    sao_ui_menu_handle_t menu = nullptr;
    const auto rc = sao_ui_menu_create(nullptr, nullptr,
                                       SAO_UI_MENU_MODE_RING, &menu);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(menu != nullptr);
    const SaoUiMenuLayout l = make_ring_layout();
    REQUIRE(sao_ui_menu_set_layout(menu, &l) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_set_items(menu, kRingItems, kRingCount) == SAO_STATUS_OK);
    return menu;
}

struct MenuCallbackProbe {
    sao_ui_menu_handle_t menu{};
    int32_t calls{};
    sao_status_t reentry_status{SAO_STATUS_ERR_UNKNOWN};
    bool throw_exception{};
};

void SAO_UI_CALL menu_callback(SaoUiMenuEvent,
                               int32_t,
                               int32_t,
                               int32_t,
                               void* user_data) {
    auto* probe = static_cast<MenuCallbackProbe*>(user_data);
    ++probe->calls;
    SaoUiMenuPhase phase = SAO_UI_MENU_PHASE_CLOSED;
    probe->reentry_status = sao_ui_menu_get_phase(probe->menu, &phase);
    if (probe->throw_exception) {
        throw std::runtime_error("menu callback");
    }
}

}  // namespace

TEST_CASE("menu_create_ring_mode_layout_correct", "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();
    const SaoUiMenuLayout l = make_ring_layout();

    // Button 0 must sit at 12 o'clock — its bbox centre lands exactly
    // at (center_x, center_y - outer_radius).  Because the bbox is
    // top-left + size, the top-left y = center_y - outer_radius - size/2.
    int32_t x = 0, y = 0, w = 0, h = 0;
    REQUIRE(sao_ui_menu_compute_button_layout(menu, 0, &x, &y, &w, &h)
            == SAO_STATUS_OK);
    CHECK(w == l.button_size);
    CHECK(h == l.button_size);
    // x centred on center_x.
    CHECK(x == l.center_x - l.button_size / 2);
    // y = center_y - outer_radius - size/2 (button 0 at top).
    CHECK(y == l.center_y - l.outer_radius - l.button_size / 2);

    // Button 3 must sit at 6 o'clock (opposite side).
    REQUIRE(sao_ui_menu_compute_button_layout(menu, 3, &x, &y, &w, &h)
            == SAO_STATUS_OK);
    CHECK(x == l.center_x - l.button_size / 2);
    CHECK(y == l.center_y + l.outer_radius - l.button_size / 2);

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu_hit_test_ring_center_returns_no_button", "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();
    const SaoUiMenuLayout l = make_ring_layout();

    // Dead centre — the NerveGear disc lives here, not a menu button.
    int32_t menu_idx = 42;    // sentinel: hit_test must overwrite
    int32_t child_idx = 42;
    REQUIRE(sao_ui_menu_hit_test(menu, l.center_x, l.center_y,
                                 &menu_idx, &child_idx) == SAO_STATUS_OK);
    CHECK(menu_idx == -1);
    CHECK(child_idx == -1);

    // Also verify: inside the inner_radius disc but still off any
    // button ring (r < outer_radius - button_size/2) returns -1.
    REQUIRE(sao_ui_menu_hit_test(menu,
                                 l.center_x, l.center_y - 30,
                                 &menu_idx, &child_idx) == SAO_STATUS_OK);
    CHECK(menu_idx == -1);

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu_hit_test_ring_button_position_returns_correct_index",
          "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();

    // For each button, compute its bbox, aim the cursor at the centre,
    // and confirm hit_test returns that index.
    for (int32_t i = 0; i < static_cast<int32_t>(kRingCount); ++i) {
        int32_t x = 0, y = 0, w = 0, h = 0;
        REQUIRE(sao_ui_menu_compute_button_layout(menu, i, &x, &y, &w, &h)
                == SAO_STATUS_OK);
        const int32_t cx = x + w / 2;
        const int32_t cy = y + h / 2;
        int32_t menu_idx = -1, child_idx = -1;
        INFO("button i=" << i << " centre=(" << cx << "," << cy << ")");
        REQUIRE(sao_ui_menu_hit_test(menu, cx, cy, &menu_idx, &child_idx)
                == SAO_STATUS_OK);
        CHECK(menu_idx == i);
    }

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu_show_transitions_to_opening", "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();

    // Freshly created menu is CLOSED.
    SaoUiMenuPhase phase = SAO_UI_MENU_PHASE_CLOSED;
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_CLOSED);

    // show() from CLOSED transitions to OPENING (not straight to OPEN).
    REQUIRE(sao_ui_menu_show(menu, 400, 300) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_OPENING);

    // is_visible() also flips to true (any non-CLOSED phase is visible).
    bool visible = false;
    REQUIRE(sao_ui_menu_is_visible(menu, &visible) == SAO_STATUS_OK);
    CHECK(visible);

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu_tick_progresses_to_open_after_450ms", "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();

    REQUIRE(sao_ui_menu_show(menu, 400, 300) == SAO_STATUS_OK);

    SaoUiMenuPhase phase = SAO_UI_MENU_PHASE_CLOSED;
    // At t=0 we're OPENING.
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_OPENING);

    // Tick just under the threshold — still OPENING.
    REQUIRE(sao_ui_menu_tick(menu, 300) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_OPENING);

    // Cross the 450 ms threshold — advance to OPEN.
    REQUIRE(sao_ui_menu_tick(menu, 150) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_OPEN);

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu_set_button_state_hover_and_query", "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();

    SaoUiMenuBtnState st = SAO_UI_MENU_BTN_DISABLED;
    REQUIRE(sao_ui_menu_get_button_state(menu, 2, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_MENU_BTN_IDLE);

    REQUIRE(sao_ui_menu_set_button_state(menu, 2, SAO_UI_MENU_BTN_HOVER)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_button_state(menu, 2, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_MENU_BTN_HOVER);

    // Range-checking: an out-of-range button index is rejected.
    CHECK(sao_ui_menu_set_button_state(menu, 99, SAO_UI_MENU_BTN_HOVER)
          == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // ACTIVE state promotes to active_idx (verified indirectly via
    // dispatch_event → EV_ITEM_ACTIVATED path in a smoke check).
    REQUIRE(sao_ui_menu_set_button_state(menu, 4, SAO_UI_MENU_BTN_ACTIVE)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_button_state(menu, 4, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_MENU_BTN_ACTIVE);

    REQUIRE(sao_ui_menu_set_items(menu, kRingItems, kRingCount) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_button_state(menu, 2, &st) == SAO_STATUS_OK);
    CHECK(st == SAO_UI_MENU_BTN_IDLE);

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu callbacks allow reentry and isolate exceptions",
          "[ui][menu][wave3][callback]") {
    sao_ui_menu_handle_t menu = make_ring_menu();
    MenuCallbackProbe probe{menu};
    REQUIRE(sao_ui_menu_set_event_callback(menu, &menu_callback, &probe) ==
            SAO_STATUS_OK);

    REQUIRE(sao_ui_menu_set_hover(menu, 2) == SAO_STATUS_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.reentry_status == SAO_STATUS_OK);

    probe.throw_exception = true;
    REQUIRE(sao_ui_menu_activate(menu, 1) == SAO_STATUS_OK);
    CHECK(probe.calls == 2);
    CHECK(probe.reentry_status == SAO_STATUS_OK);

    REQUIRE(sao_ui_menu_show(menu, 400, 300) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_tick(menu, 450) == SAO_STATUS_OK);
    CHECK(probe.calls == 3);
    CHECK(probe.reentry_status == SAO_STATUS_OK);

    sao_ui_menu_destroy(menu);
}

TEST_CASE("menu_hide_transitions_to_closing", "[ui][menu][wave3]") {
    sao_ui_menu_handle_t menu = make_ring_menu();

    // Drive to OPEN first.
    REQUIRE(sao_ui_menu_show(menu, 400, 300) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_tick(menu, 450) == SAO_STATUS_OK);
    SaoUiMenuPhase phase = SAO_UI_MENU_PHASE_CLOSED;
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    REQUIRE(phase == SAO_UI_MENU_PHASE_OPEN);

    // hide() flips to CLOSING (not straight to CLOSED).
    REQUIRE(sao_ui_menu_hide(menu) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_CLOSING);

    // Ticking through the close animation lands on CLOSED.
    REQUIRE(sao_ui_menu_tick(menu, 300) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
    CHECK(phase == SAO_UI_MENU_PHASE_CLOSED);

    sao_ui_menu_destroy(menu);
}
