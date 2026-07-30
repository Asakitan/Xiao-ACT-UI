// SAO Auto — Phase 14.6 cross-surface UX tests.
//
// Toast stacking/dismiss, focus traversal order, badge animation,
// filter matching.  Local-only tests — never committed.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/widget_badge.h"
#include "sao/ui/widget_filter_row.h"
#include "sao/ui/widget_kit.h"
#include "sao/ui/focus_manager.h"
#include "sao/ui/widget_toast.h"

extern "C" {

SAO_UI_API void SAO_UI_CALL sao_ui_widget_data_family_destroy(sao_ui_widget_handle_t handle);

}  // extern "C"

namespace {

// ── Toast ─────────────────────────────────────────────────────────
struct ToastCtx {
    std::atomic<int32_t> dismissed_calls{0};
    std::atomic<bool> last_cancelled{false};
};

void SAO_UI_CALL on_toast_dismissed(bool cancelled, void* user_data) {
    auto* ctx = static_cast<ToastCtx*>(user_data);
    ctx->dismissed_calls.fetch_add(1);
    ctx->last_cancelled.store(cancelled);
}

SaoUiToastSpec toast_spec(const char* text, int32_t severity, int32_t duration_ms = 3000) {
    SaoUiToastSpec spec{};
    spec.text_utf8 = text;
    spec.severity = severity;
    spec.duration_ms = duration_ms;
    spec.fade_ms = 240;
    spec.theme_override = SAO_UI_THEME_COUNT;
    return spec;
}

}  // namespace

// ── Toast tests ────────────────────────────────────────────────────

TEST_CASE("toast_stacks_and_reports_visible_count", "[ui][toast][stack]") {
    sao_ui_toast_handle_t handle = nullptr;
    REQUIRE(sao_ui_toast_create(nullptr, &handle) == SAO_STATUS_OK);
    ToastCtx ctx;
    SaoUiToastSpec first = toast_spec("first", SAO_UI_TOAST_INFO);
    REQUIRE(sao_ui_toast_show(handle, &first, on_toast_dismissed, &ctx) == SAO_STATUS_OK);
    SaoUiToastSpec second = toast_spec("second", SAO_UI_TOAST_SUCCESS);
    REQUIRE(sao_ui_toast_show(handle, &second, on_toast_dismissed, &ctx) == SAO_STATUS_OK);
    int32_t count = 0;
    REQUIRE(sao_ui_toast_visible_count(handle, &count) == SAO_STATUS_OK);
    REQUIRE(count == 2);

    char text[64] = {};
    size_t written = 0;
    REQUIRE(sao_ui_toast_stack_top_text(handle, text, sizeof(text), &written) == SAO_STATUS_OK);
    REQUIRE(std::string(text) == "second");
    sao_ui_toast_destroy(handle);
}

TEST_CASE("toast_evicts_oldest_at_capacity_4", "[ui][toast][stack][evict]") {
    sao_ui_toast_handle_t handle = nullptr;
    REQUIRE(sao_ui_toast_create(nullptr, &handle) == SAO_STATUS_OK);
    ToastCtx ctx;
    for (int i = 0; i < 5; ++i) {
        SaoUiToastSpec spec = toast_spec(("t" + std::to_string(i)).c_str(), SAO_UI_TOAST_INFO);
        REQUIRE(sao_ui_toast_show(handle, &spec, on_toast_dismissed, &ctx) == SAO_STATUS_OK);
    }
    int32_t count = 0;
    REQUIRE(sao_ui_toast_visible_count(handle, &count) == SAO_STATUS_OK);
    REQUIRE(count == 4);  // max 4 visible
    // Oldest ("t0") was evicted with cancelled=true.
    REQUIRE(ctx.dismissed_calls.load() == 1);
    REQUIRE(ctx.last_cancelled.load() == true);
    sao_ui_toast_destroy(handle);
}

TEST_CASE("toast_auto_dismiss_after_duration", "[ui][toast][dismiss]") {
    sao_ui_toast_handle_t handle = nullptr;
    REQUIRE(sao_ui_toast_create(nullptr, &handle) == SAO_STATUS_OK);
    ToastCtx ctx;
    SaoUiToastSpec spec = toast_spec("ephemeral", SAO_UI_TOAST_WARN, 1000);
    REQUIRE(sao_ui_toast_show(handle, &spec, on_toast_dismissed, &ctx) == SAO_STATUS_OK);
    // Tick past duration → fires dismiss with cancelled=false.
    REQUIRE(sao_ui_toast_tick(handle, 1100) == SAO_STATUS_ERR_NOT_FOUND);
    REQUIRE(ctx.dismissed_calls.load() == 1);
    REQUIRE(ctx.last_cancelled.load() == false);
    int32_t count = 99;
    REQUIRE(sao_ui_toast_visible_count(handle, &count) == SAO_STATUS_OK);
    REQUIRE(count == 0);
    sao_ui_toast_destroy(handle);
}

TEST_CASE("toast_manual_dismiss_fires_cancelled", "[ui][toast][dismiss]") {
    sao_ui_toast_handle_t handle = nullptr;
    REQUIRE(sao_ui_toast_create(nullptr, &handle) == SAO_STATUS_OK);
    ToastCtx ctx;
    SaoUiToastSpec spec = toast_spec("manual", SAO_UI_TOAST_ERROR, 5000);
    REQUIRE(sao_ui_toast_show(handle, &spec, on_toast_dismissed, &ctx) == SAO_STATUS_OK);
    REQUIRE(sao_ui_toast_dismiss(handle) == SAO_STATUS_OK);
    REQUIRE(ctx.dismissed_calls.load() == 1);
    REQUIRE(ctx.last_cancelled.load() == true);
    sao_ui_toast_destroy(handle);
}

// ── Focus tests ───────────────────────────────────────────────────
//
// Focus manager drives sao_ui_widget_set_focused on real widget handles.
// We create generic widgets (which back onto the legacy d2d_widgets
// registry) so the focused flag plumbing is exercised end-to-end.

TEST_CASE("focus_tab_traversal_wraps_forward_and_reverse", "[ui][focus][traversal]") {
    sao_ui_focus_handle_t focus = nullptr;
    REQUIRE(sao_ui_focus_create(&focus) == SAO_STATUS_OK);

    std::vector<sao_ui_widget_handle_t> widgets;
    for (int i = 0; i < 3; ++i) {
        sao_ui_widget_handle_t w = nullptr;
        REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &w) == SAO_STATUS_OK);
        widgets.push_back(w);
        REQUIRE(sao_ui_focus_register(focus, w) == SAO_STATUS_OK);
    }

    size_t count = 0;
    REQUIRE(sao_ui_focus_order_count(focus, &count) == SAO_STATUS_OK);
    REQUIRE(count == 3);

    // Tab forward: 0 -> 1 -> 2 -> 0.
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_focus_tab_next(focus, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == widgets[0]);
    REQUIRE(sao_ui_focus_tab_next(focus, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == widgets[1]);
    REQUIRE(sao_ui_focus_tab_next(focus, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == widgets[2]);
    // Wrap.
    REQUIRE(sao_ui_focus_tab_next(focus, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == widgets[0]);

    // Shift-Tab reverse from 0 -> 2.
    REQUIRE(sao_ui_focus_tab_next(focus, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == widgets[2]);

    for (auto w : widgets)
        sao_ui_widget_destroy(w);
    sao_ui_focus_destroy(focus);
}

TEST_CASE("focus_set_clears_previous_and_unregisters", "[ui][focus][set][unregister]") {
    sao_ui_focus_handle_t focus = nullptr;
    REQUIRE(sao_ui_focus_create(&focus) == SAO_STATUS_OK);

    sao_ui_widget_handle_t a = nullptr;
    sao_ui_widget_handle_t b = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_register(focus, a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_register(focus, b) == SAO_STATUS_OK);

    REQUIRE(sao_ui_focus_set(focus, a) == SAO_STATUS_OK);
    sao_ui_widget_handle_t focused = nullptr;
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == a);

    // Set to b clears a.
    REQUIRE(sao_ui_focus_set(focus, b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == b);

    // Unregister b clears focus.
    REQUIRE(sao_ui_focus_unregister(focus, b) == SAO_STATUS_OK);
    REQUIRE(sao_ui_focus_get(focus, &focused) == SAO_STATUS_OK);
    REQUIRE(focused == nullptr);

    sao_ui_widget_destroy(a);
    sao_ui_widget_destroy(b);
    sao_ui_focus_destroy(focus);
}

// ── Animated badge tests ──────────────────────────────────────────

TEST_CASE("animated_badge_count_get_set", "[ui][badge][count]") {
    SaoUiAnimatedBadgeSpec spec{};
    spec.count = 5;
    spec.dot_radius_px = 6;
    spec.pulse_ms = 0;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_animated_badge_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t count = -1;
    REQUIRE(sao_ui_animated_badge_get_count(h, &count) == SAO_STATUS_OK);
    REQUIRE(count == 5);
    REQUIRE(sao_ui_animated_badge_set_count(h, 12) == SAO_STATUS_OK);
    REQUIRE(sao_ui_animated_badge_get_count(h, &count) == SAO_STATUS_OK);
    REQUIRE(count == 12);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("animated_badge_pulse_envelope_rises_then_falls", "[ui][badge][pulse]") {
    SaoUiAnimatedBadgeSpec spec{};
    spec.count = -1;  // dot only
    spec.pulse_ms = 1000;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_animated_badge_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    bool pulsing = false;
    REQUIRE(sao_ui_animated_badge_is_pulsing(h, &pulsing) == SAO_STATUS_OK);
    REQUIRE(pulsing);

    // At phase 0 → scale 1.0.
    float scale = 0.0F;
    REQUIRE(sao_ui_animated_badge_pulse_scale(h, &scale) == SAO_STATUS_OK);
    REQUIRE(std::fabs(scale - 1.0F) < 1e-4F);

    // Advance to mid-phase (500ms) → peak ~1.18.
    REQUIRE(sao_ui_animated_badge_tick(h, 500) == SAO_STATUS_OK);
    REQUIRE(sao_ui_animated_badge_pulse_scale(h, &scale) == SAO_STATUS_OK);
    REQUIRE(scale > 1.10F);
    REQUIRE(scale <= 1.18F);

    // Advance to end (1000ms) → back to 1.0.
    REQUIRE(sao_ui_animated_badge_tick(h, 500) == SAO_STATUS_OK);
    REQUIRE(sao_ui_animated_badge_pulse_scale(h, &scale) == SAO_STATUS_OK);
    REQUIRE(std::fabs(scale - 1.0F) < 1e-4F);

    // Stop pulse.
    REQUIRE(sao_ui_animated_badge_set_pulse(h, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_animated_badge_is_pulsing(h, &pulsing) == SAO_STATUS_OK);
    REQUIRE(!pulsing);
    REQUIRE(sao_ui_animated_badge_pulse_scale(h, &scale) == SAO_STATUS_OK);
    REQUIRE(std::fabs(scale - 1.0F) < 1e-4F);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("animated_badge_apply_props_updates_count_and_pulse", "[ui][badge][props]") {
    SaoUiAnimatedBadgeSpec spec{};
    spec.count = 0;
    spec.pulse_ms = 0;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_animated_badge_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    constexpr char props[] = R"({"count":7,"pulse_ms":800})";
    REQUIRE(sao_ui_animated_badge_apply_props(h, reinterpret_cast<const uint8_t*>(props), sizeof(props) - 1) == SAO_STATUS_OK);
    int32_t count = -1;
    REQUIRE(sao_ui_animated_badge_get_count(h, &count) == SAO_STATUS_OK);
    REQUIRE(count == 7);
    bool pulsing = false;
    REQUIRE(sao_ui_animated_badge_is_pulsing(h, &pulsing) == SAO_STATUS_OK);
    REQUIRE(pulsing);
    sao_ui_widget_data_family_destroy(h);
}

// ── Filter row tests ───────────────────────────────────────────────

TEST_CASE("filter_row_query_set_get_and_match", "[ui][filter][query][match]") {
    SaoUiFilterRowSpec spec{};
    spec.placeholder_utf8 = "search...";
    SaoUiFilterChipSpec chips[] = {
        {"All", 1, true, {0}},
        {"Active", 2, false, {0}},
        {"Pending", 3, false, {0}},
    };
    spec.chips = chips;
    spec.chip_count = 3;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_filter_row_create(nullptr, &spec, &h) == SAO_STATUS_OK);

    REQUIRE(sao_ui_filter_row_set_query(h, "hello") == SAO_STATUS_OK);
    char buf[32] = {};
    size_t written = 0;
    REQUIRE(sao_ui_filter_row_get_query(h, buf, sizeof(buf), &written) == SAO_STATUS_OK);
    REQUIRE(std::string(buf) == "hello");

    bool match = false;
    REQUIRE(sao_ui_filter_row_matches_text(h, "say hello world", &match) == SAO_STATUS_OK);
    REQUIRE(match);
    REQUIRE(sao_ui_filter_row_matches_text(h, "goodbye", &match) == SAO_STATUS_OK);
    REQUIRE(!match);

    // Empty query matches everything.
    REQUIRE(sao_ui_filter_row_set_query(h, "") == SAO_STATUS_OK);
    REQUIRE(sao_ui_filter_row_matches_text(h, "anything", &match) == SAO_STATUS_OK);
    REQUIRE(match);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("filter_row_chip_toggle_and_match", "[ui][filter][chips][match]") {
    SaoUiFilterRowSpec spec{};
    SaoUiFilterChipSpec chips[] = {
        {"Red", 10, false, {0}},
        {"Green", 20, false, {0}},
        {"Blue", 30, true, {0}},
    };
    spec.chips = chips;
    spec.chip_count = 3;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_filter_row_create(nullptr, &spec, &h) == SAO_STATUS_OK);

    bool selected = false;
    REQUIRE(sao_ui_filter_row_is_chip_selected(h, 10, &selected) == SAO_STATUS_OK);
    REQUIRE(!selected);
    REQUIRE(sao_ui_filter_row_is_chip_selected(h, 30, &selected) == SAO_STATUS_OK);
    REQUIRE(selected);

    // Toggle red on.
    REQUIRE(sao_ui_filter_row_toggle_chip(h, 10) == SAO_STATUS_OK);
    REQUIRE(sao_ui_filter_row_is_chip_selected(h, 10, &selected) == SAO_STATUS_OK);
    REQUIRE(selected);

    // Match: candidate has red selected.
    int32_t active[] = {10};
    bool match = false;
    REQUIRE(sao_ui_filter_row_matches_chips(h, active, 1, &match) == SAO_STATUS_OK);
    REQUIRE(match);

    // No active ids → match all.
    REQUIRE(sao_ui_filter_row_matches_chips(h, nullptr, 0, &match) == SAO_STATUS_OK);
    REQUIRE(match);

    // Unknown chip id → not found.
    REQUIRE(sao_ui_filter_row_toggle_chip(h, 999) == SAO_STATUS_ERR_NOT_FOUND);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("filter_row_state_json_round_trips", "[ui][filter][state_json]") {
    SaoUiFilterRowSpec spec{};
    SaoUiFilterChipSpec chips[] = {
        {"A", 1, false, {0}},
        {"B", 2, true, {0}},
    };
    spec.chips = chips;
    spec.chip_count = 2;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_filter_row_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(sao_ui_filter_row_set_query(h, "q") == SAO_STATUS_OK);

    char buf[256] = {};
    size_t written = 0;
    REQUIRE(sao_ui_filter_row_state_json(h, buf, sizeof(buf), &written) == SAO_STATUS_OK);
    const std::string json(buf, written);
    REQUIRE(json.find("\"query\":\"q\"") != std::string::npos);
    REQUIRE(json.find("\"id\":1") != std::string::npos);
    REQUIRE(json.find("\"id\":2") != std::string::npos);
    REQUIRE(json.find("\"selected\":true") != std::string::npos);
    sao_ui_widget_data_family_destroy(h);
}

TEST_CASE("filter_row_apply_props_updates_query_and_chips", "[ui][filter][props]") {
    SaoUiFilterRowSpec spec{};
    SaoUiFilterChipSpec chips[] = {
        {"X", 100, false, {0}},
        {"Y", 200, false, {0}},
    };
    spec.chips = chips;
    spec.chip_count = 2;
    sao_ui_widget_handle_t h = nullptr;
    REQUIRE(sao_ui_filter_row_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    constexpr char props[] =
        R"({"query":"new","chips":[{"id":100,"selected":true},{"id":200,"selected":false}]})";
    REQUIRE(sao_ui_filter_row_apply_props(h, reinterpret_cast<const uint8_t*>(props), sizeof(props) - 1) == SAO_STATUS_OK);
    char buf[16] = {};
    size_t written = 0;
    REQUIRE(sao_ui_filter_row_get_query(h, buf, sizeof(buf), &written) == SAO_STATUS_OK);
    REQUIRE(std::string(buf) == "new");
    bool selected = false;
    REQUIRE(sao_ui_filter_row_is_chip_selected(h, 100, &selected) == SAO_STATUS_OK);
    REQUIRE(selected);
    sao_ui_widget_data_family_destroy(h);
}