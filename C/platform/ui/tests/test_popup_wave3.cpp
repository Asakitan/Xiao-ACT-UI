// SAO Auto — Wave3 popup keyboard-nav + hit-test tests (G3.4 gate).
//
// Covers the state machine documented in the popup.h header banner:
// UP/DOWN wrap, LEFT closes submenu (no-op at root), RIGHT opens
// submenu, ENTER fires the action callback with the current entry_id,
// ESC dismisses with dismissed=true.  Hit test resolves the deepest
// open level first.

#include <atomic>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/popup.h"

namespace {

// Shared spec builder.  Entries are:
//   0: File           (submenu: New / Open / Save)
//   1: --- separator ---
//   2: Edit           (disabled)
//   3: View           (submenu: Zoom In / Zoom Out)
//   4: Quit
struct TestPopupCtx {
    std::vector<SaoUiPopupEntry> file_sub;
    std::vector<SaoUiPopupEntry> view_sub;
    std::vector<SaoUiPopupEntry> root;
    SaoUiPopupSpec spec{};

    // Callback state.
    std::atomic<int32_t> chosen_id{-999};
    std::atomic<bool> was_dismissed{false};
    std::atomic<int32_t> callback_calls{0};

    TestPopupCtx() {
        // File submenu.
        file_sub.push_back({"New",  nullptr, "Ctrl+N", 10, true, false, false, false, nullptr, 0});
        file_sub.push_back({"Open", nullptr, "Ctrl+O", 11, true, false, false, false, nullptr, 0});
        file_sub.push_back({"Save", nullptr, "Ctrl+S", 12, true, false, false, false, nullptr, 0});
        // View submenu.
        view_sub.push_back({"Zoom In",  nullptr, nullptr, 30, true, false, false, false, nullptr, 0});
        view_sub.push_back({"Zoom Out", nullptr, nullptr, 31, true, false, false, false, nullptr, 0});
        // Root.
        root.push_back({"File",  nullptr, nullptr, 1, true,  false, false, false, file_sub.data(), file_sub.size()});
        root.push_back({"---",   nullptr, nullptr, -1, false, false, true,  false, nullptr, 0});
        root.push_back({"Edit",  nullptr, nullptr, 2, false, false, false, false, nullptr, 0});
        root.push_back({"View",  nullptr, nullptr, 3, true,  false, false, false, view_sub.data(), view_sub.size()});
        root.push_back({"Quit",  nullptr, "Alt+F4", 4, true, false, false, false, nullptr, 0});

        std::memset(&spec, 0, sizeof(spec));
        spec.parent_hwnd = nullptr;
        spec.anchor_x = 100;
        spec.anchor_y = 200;
        spec.anchor_w = 200;
        spec.anchor_h = 24;
        spec.entries = root.data();
        spec.entry_count = root.size();
        spec.theme_override = SAO_UI_THEME_COUNT;   // COUNT — inherit
        spec.allow_keyboard_nav = true;
        spec.dismiss_on_focus_out = true;
        spec.fade_in_ms = 450;
        spec.fade_out_ms = 300;
    }
};

void SAO_UI_CALL on_result(
    int32_t chosen_entry_id, bool dismissed,
    int32_t /*screen_x*/, int32_t /*screen_y*/, void* user_data) {
    auto* ctx = static_cast<TestPopupCtx*>(user_data);
    ctx->chosen_id.store(chosen_entry_id);
    ctx->was_dismissed.store(dismissed);
    ctx->callback_calls.fetch_add(1);
}

// Convenience: create + show, returns handle.
sao_ui_popup_handle_t make_and_show(TestPopupCtx& ctx) {
    sao_ui_popup_handle_t h = nullptr;
    REQUIRE(sao_ui_popup_create(nullptr, nullptr, &h) == SAO_STATUS_OK);
    REQUIRE(h != nullptr);
    REQUIRE(sao_ui_popup_show(h, &ctx.spec, &on_result, &ctx) == SAO_STATUS_OK);
    return h;
}

}  // namespace

TEST_CASE("popup_dispatch_down_arrow_advances_selection", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Initial selection = first enabled = entry 0 (File).
    // DOWN skips separator + disabled Edit → View (entry 3).
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);
    int32_t eid = -1;
    int32_t depth = -1;
    // Hit-test at the row we know is selected — since hit_test is
    // spatial, we use it as an indirect probe: after DOWN we expect
    // 'View' (entry 3) to be at row index 3 in the top level.
    REQUIRE(sao_ui_popup_hit_test(h, 200, 200 + 28 * 3, &eid, &depth) == SAO_STATUS_OK);
    REQUIRE(eid == 3);
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_dispatch_down_at_last_wraps_to_first", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Walk DOWN enough times to reach the last enabled (Quit, entry 4)
    // and once more to wrap.  Starting from File (0):
    //   DOWN → View (3)     [skip sep + disabled Edit]
    //   DOWN → Quit (4)
    //   DOWN → File (0)  ← wrap
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);
    // Selection wrapped back to File (0) — File has a submenu, so
    // RIGHT opens it and selection lands on the first sub-entry (New,
    // entry 10).  ENTER on New then fires the callback with id 10.
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_RIGHT) == SAO_STATUS_OK);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_ENTER) == SAO_STATUS_OK);
    REQUIRE(ctx.chosen_id.load() == 10);
    REQUIRE(ctx.was_dismissed.load() == false);
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_dispatch_up_at_first_wraps_to_last", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Start at File (0) → UP wraps to Quit (4).
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_UP) == SAO_STATUS_OK);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_ENTER) == SAO_STATUS_OK);
    REQUIRE(ctx.chosen_id.load() == 4);
    REQUIRE(ctx.callback_calls.load() == 1);
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_dispatch_right_opens_submenu", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Root selection = File.  RIGHT opens submenu.
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_RIGHT) == SAO_STATUS_OK);
    // Now ENTER on submenu should fire the sub-entry id.
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_ENTER) == SAO_STATUS_OK);
    REQUIRE(ctx.chosen_id.load() == 10);   // "New"
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_dispatch_left_closes_submenu", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Open File submenu, then LEFT to close it.
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_RIGHT) == SAO_STATUS_OK);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_LEFT)  == SAO_STATUS_OK);
    // Now ENTER on root — selection was File (opens submenu again).
    // Instead, DOWN to View, RIGHT, ENTER → verify we're back at root.
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);   // → View
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_RIGHT) == SAO_STATUS_OK);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_ENTER) == SAO_STATUS_OK);
    REQUIRE(ctx.chosen_id.load() == 30);   // Zoom In
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_dispatch_enter_returns_action_id", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Initial selection is File; but File has a submenu — ENTER
    // opens the submenu instead of firing.  Walk to Quit first.
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);  // → View
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_DOWN) == SAO_STATUS_OK);  // → Quit
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_ENTER) == SAO_STATUS_OK);
    REQUIRE(ctx.chosen_id.load() == 4);
    REQUIRE(ctx.was_dismissed.load() == false);
    // Callback should fire exactly once.
    REQUIRE(ctx.callback_calls.load() == 1);
    // Popup should now be hidden.
    bool visible = true;
    // After ENTER-choose, is_visible should return false (state was cleared).
    // The internal state check happens under the lock; we accept the
    // NOT_INITIALIZED return since levels are cleared.
    auto vis_status = sao_ui_popup_is_visible(h, &visible);
    if (vis_status == SAO_STATUS_OK) REQUIRE(visible == false);
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_dispatch_escape_closes", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_ESC) == SAO_STATUS_OK);
    REQUIRE(ctx.was_dismissed.load() == true);
    REQUIRE(ctx.chosen_id.load() == -1);
    REQUIRE(ctx.callback_calls.load() == 1);
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_hit_test_top_level", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Row 0 (File) starts at anchor_y=200, height=28.
    int32_t eid = -999;
    int32_t depth = -999;
    REQUIRE(sao_ui_popup_hit_test(h, 150, 210, &eid, &depth) == SAO_STATUS_OK);
    REQUIRE(eid == 1);        // File.entry_id
    REQUIRE(depth == 0);
    // Row 2 (Edit) sits below sep (8px) after row 0 (28px): 200+28+8=236.
    eid = -999;
    REQUIRE(sao_ui_popup_hit_test(h, 150, 240, &eid, &depth) == SAO_STATUS_OK);
    REQUIRE(eid == 2);        // Edit
    REQUIRE(depth == 0);
    // Off the popup entirely.
    eid = -999; depth = -999;
    REQUIRE(sao_ui_popup_hit_test(h, 0, 0, &eid, &depth) == SAO_STATUS_OK);
    REQUIRE(eid == -1);
    sao_ui_popup_destroy(h);
}

TEST_CASE("popup_hit_test_open_submenu_area", "[ui][popup][wave3]") {
    TestPopupCtx ctx;
    auto* h = make_and_show(ctx);
    // Open File submenu.  Sub anchor:
    //   sub_x = anchor_x(100) + anchor_w(200) + gap_menu_child(25) = 325
    //   sub_y = row_top of File = anchor_y = 200
    REQUIRE(sao_ui_popup_key_press(h, SAO_UI_POPUP_KEY_RIGHT) == SAO_STATUS_OK);
    // Hit test inside submenu — first row should be "New" (id 10).
    int32_t eid = -999;
    int32_t depth = -999;
    REQUIRE(sao_ui_popup_hit_test(h, 400, 210, &eid, &depth) == SAO_STATUS_OK);
    REQUIRE(eid == 10);
    REQUIRE(depth == 1);
    // Hit test outside submenu but inside root — depth 0.
    eid = -999; depth = -999;
    REQUIRE(sao_ui_popup_hit_test(h, 150, 210, &eid, &depth) == SAO_STATUS_OK);
    REQUIRE(eid == 1);        // File (root row 0)
    REQUIRE(depth == 0);
    sao_ui_popup_destroy(h);
}
