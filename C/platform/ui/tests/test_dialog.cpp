// SAO Auto - dialog state-machine, focus, and callback tests.
//
// Covers the state machine documented in dialog.h:
//   * create/show enters EXPANDING (200ms) → CLIP_REVEALED
//   * dispatch_key: TAB advances, SHIFT+TAB reverses, ENTER dismisses
//     with the focused button's kind, ESC dismisses with DISMISS
//   * dismiss transitions to SHRINKING; the callback fires exactly
//     once at SHRINKING → IDLE
//
// This slice is renderless — no compositor / theme handles are needed,
// so we pass nullptr for both.

#include <atomic>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/dialog.h"

namespace {

struct DialogCtx {
    std::atomic<int32_t> callback_calls{0};
    std::atomic<int32_t> last_pressed{-999};
    std::atomic<bool>    has_input{false};
};

void SAO_UI_CALL on_dialog_result(
    SaoUiDialogButton pressed,
    const char* input_text_utf8,
    size_t /*input_text_len*/, void* user_data) {
    auto* ctx = static_cast<DialogCtx*>(user_data);
    ctx->callback_calls.fetch_add(1);
    ctx->last_pressed.store(static_cast<int32_t>(pressed));
    ctx->has_input.store(input_text_utf8 != nullptr);
}

// Convenience: allocate + show INFO with OK+Cancel (using explicit
// buttons array instead of relying on the ASK kind default).
sao_ui_dialog_handle_t make_info_two_buttons(DialogCtx& ctx) {
    sao_ui_dialog_handle_t h = nullptr;
    REQUIRE(sao_ui_dialog_create(nullptr, nullptr, &h) == SAO_STATUS_OK);
    REQUIRE(h != nullptr);
    SaoUiDialogButtonSpec btns[2] = {};
    btns[0].kind = SAO_UI_DIALOG_BTN_OK;
    btns[0].label_utf8 = nullptr;   // fall through to canonical "OK"
    btns[0].color_argb = 0;
    btns[1].kind = SAO_UI_DIALOG_BTN_CANCEL;
    btns[1].label_utf8 = nullptr;
    btns[1].color_argb = 0;
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_INFO;
    spec.title_utf8 = "Test Title";
    spec.message_utf8 = "Test Message";
    spec.buttons = btns;
    spec.button_count = 2;
    spec.expand_ms = 500;
    spec.clip_reveal_ms_title = 400;
    spec.clip_reveal_ms_message = 350;
    spec.shrink_ms = 350;
    spec.dismiss_on_esc = true;
    spec.draggable = true;
    REQUIRE(sao_ui_dialog_show(h, &spec, &on_dialog_result, &ctx) == SAO_STATUS_OK);
    return h;
}

}  // namespace

TEST_CASE("dialog_create_info_kind_2_buttons", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    int32_t count = -1;
    REQUIRE(sao_ui_dialog_get_button_count(h, &count) == SAO_STATUS_OK);
    REQUIRE(count == 2);
    // Check both buttons resolved with canonical labels + colors.
    SaoUiDialogButtonInfo bi{};
    REQUIRE(sao_ui_dialog_get_button_at(h, 0, &bi) == SAO_STATUS_OK);
    REQUIRE(bi.kind == SAO_UI_DIALOG_BTN_OK);
    REQUIRE(std::strcmp(bi.label_utf8, "OK") == 0);
    REQUIRE(bi.color_argb == 0xFF428CE6u);   // OK_BLUE
    REQUIRE(bi.is_focused == true);          // default focus on first
    REQUIRE(sao_ui_dialog_get_button_at(h, 1, &bi) == SAO_STATUS_OK);
    REQUIRE(bi.kind == SAO_UI_DIALOG_BTN_CANCEL);
    REQUIRE(std::strcmp(bi.label_utf8, "Cancel") == 0);
    REQUIRE(bi.color_argb == 0xFFD13D4Fu);   // CLOSE_RED
    REQUIRE(bi.is_focused == false);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_show_transitions_to_expanding", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    SaoUiDialogState st = SAO_UI_DIALOG_STATE_IDLE;
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_EXPANDING);
    bool visible = false;
    REQUIRE(sao_ui_dialog_is_visible(h, &visible) == SAO_STATUS_OK);
    REQUIRE(visible == true);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_tick_progresses_to_clip_reveal_after_500ms",
          "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    // Tick 499ms — still EXPANDING.
    REQUIRE(sao_ui_dialog_tick(h, 499) == SAO_STATUS_OK);
    SaoUiDialogState st = SAO_UI_DIALOG_STATE_IDLE;
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_EXPANDING);
    // Tick 1 more ms — crossed 500ms boundary.
    REQUIRE(sao_ui_dialog_tick(h, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_CLIP_REVEALED);
    // Callback must NOT have fired yet.
    REQUIRE(ctx.callback_calls.load() == 0);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_dispatch_tab_advances_focus", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    int32_t focus = -1;
    REQUIRE(sao_ui_dialog_get_button_focus(h, &focus) == SAO_STATUS_OK);
    REQUIRE(focus == 0);   // default: first button (OK)
    // TAB advances to Cancel (index 1).
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x09u, /*shift_held=*/false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_button_focus(h, &focus) == SAO_STATUS_OK);
    REQUIRE(focus == 1);
    // Another TAB wraps back to OK.
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x09u, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_button_focus(h, &focus) == SAO_STATUS_OK);
    REQUIRE(focus == 0);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_dispatch_shift_tab_reverses_focus", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    // Start at OK (0); SHIFT+TAB should wrap to last (Cancel, 1).
    int32_t focus = -1;
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x09u, /*shift_held=*/true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_button_focus(h, &focus) == SAO_STATUS_OK);
    REQUIRE(focus == 1);
    // SHIFT+TAB again wraps back to 0.
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x09u, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_button_focus(h, &focus) == SAO_STATUS_OK);
    REQUIRE(focus == 0);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_dispatch_enter_returns_button_result", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    // Advance focus to Cancel then ENTER — expect callback with CANCEL,
    // but only after SHRINKING completes.
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x09u, false) == SAO_STATUS_OK);   // TAB → Cancel
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x0Du, false) == SAO_STATUS_OK);   // ENTER
    SaoUiDialogState st = SAO_UI_DIALOG_STATE_IDLE;
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_SHRINKING);
    REQUIRE(ctx.callback_calls.load() == 0);   // not yet
    // Tick past shrink duration.
    REQUIRE(sao_ui_dialog_tick(h, 350) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_IDLE);
    REQUIRE(ctx.callback_calls.load() == 1);
    REQUIRE(ctx.last_pressed.load() == SAO_UI_DIALOG_BTN_CANCEL);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_dispatch_escape_returns_cancel", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    // ESC transitions to SHRINKING with pending DISMISS.
    REQUIRE(sao_ui_dialog_dispatch_key(h, 0x1Bu, false) == SAO_STATUS_OK);
    SaoUiDialogState st = SAO_UI_DIALOG_STATE_IDLE;
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_SHRINKING);
    // Tick past shrink → callback fires with DISMISS.
    REQUIRE(sao_ui_dialog_tick(h, 350) == SAO_STATUS_OK);
    REQUIRE(ctx.callback_calls.load() == 1);
    REQUIRE(ctx.last_pressed.load() == SAO_UI_DIALOG_BTN_DISMISS);
    sao_ui_dialog_destroy(h);
}

TEST_CASE("dialog_dismiss_transitions_to_shrinking", "[ui][dialog][runtime]") {
    DialogCtx ctx;
    auto* h = make_info_two_buttons(ctx);
    // Explicit dismiss(OK) mid-EXPANDING should short-circuit into
    // SHRINKING immediately and fire the callback on completion.
    REQUIRE(sao_ui_dialog_dismiss(h, SAO_UI_DIALOG_BTN_OK) == SAO_STATUS_OK);
    SaoUiDialogState st = SAO_UI_DIALOG_STATE_IDLE;
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_SHRINKING);
    REQUIRE(ctx.callback_calls.load() == 0);
    // Halfway through shrink — still SHRINKING.
    REQUIRE(sao_ui_dialog_tick(h, 175) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_SHRINKING);
    REQUIRE(ctx.callback_calls.load() == 0);
    // Past 350ms — IDLE, callback fired.
    REQUIRE(sao_ui_dialog_tick(h, 176) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_state(h, &st) == SAO_STATUS_OK);
    REQUIRE(st == SAO_UI_DIALOG_STATE_IDLE);
    REQUIRE(ctx.callback_calls.load() == 1);
    REQUIRE(ctx.last_pressed.load() == SAO_UI_DIALOG_BTN_OK);
    // Extra tick after IDLE returns NOT_INITIALIZED (defensive - and
    // the callback must NOT re-fire).
    REQUIRE(sao_ui_dialog_tick(h, 50) == SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(ctx.callback_calls.load() == 1);
    sao_ui_dialog_destroy(h);
}
