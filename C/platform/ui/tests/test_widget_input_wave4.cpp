// Wave 4 tests for the Button widget (G3.8 first slice).
//
// Coverage (5 CASE):
//   * button_create_stores_spec_and_text
//   * button_preferred_size_matches_text_and_padding
//   * button_hit_test_inside_and_outside
//   * button_dispatch_click_fires_callback_with_action_id
//   * button_disabled_button_swallows_click
//
// All tests are pure state-machine — no D3D device.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>

#include "sao/core/status.h"
#include "sao/ui/widget_input.h"

// ── Wave 4 helper API prototypes (not in widget_input.h) ───────────

extern "C" {

struct SaoUiPointF {
    float x;
    float y;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_preferred_size(
    sao_ui_widget_handle_t handle,
    int32_t* out_width,
    int32_t* out_height);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_hit_test(
    sao_ui_widget_handle_t handle,
    SaoUiPointF point,
    bool* out_hit);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_dispatch_event(
    sao_ui_widget_handle_t handle,
    int32_t event_type,
    int32_t* out_action_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_widget_button_is_active(
    sao_ui_widget_handle_t handle,
    bool* out_active);

SAO_UI_API void SAO_UI_CALL sao_ui_widget_input_family_destroy(
    sao_ui_widget_handle_t handle);

}  // extern "C"

namespace {

// Match widget_input.cpp private enum.
constexpr int32_t kBtnEvtMouseDown = 100;
constexpr int32_t kBtnEvtMouseUp   = 101;

SaoUiButtonSpec make_button_spec(const char* text,
                                  int32_t kind = SAO_UI_BTN_NORMAL,
                                  bool active = false,
                                  bool disabled = false,
                                  int32_t pad_x = 10,
                                  int32_t pad_y = 6) {
    SaoUiButtonSpec spec{};
    spec.text_utf8   = text;
    spec.kind        = kind;
    spec.radius_px   = 4;
    spec.pad_x_px    = pad_x;
    spec.pad_y_px    = pad_y;
    spec.active      = active;
    spec.disabled    = disabled;
    return spec;
}

struct ClickSink {
    std::atomic<int> hits{0};
};

extern "C" void SAO_UI_CALL sink_click_cb(void* user_data) {
    if (user_data != nullptr) {
        static_cast<ClickSink*>(user_data)->hits.fetch_add(1);
    }
}

}  // namespace

TEST_CASE("button_create_stores_spec_and_text",
          "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("OK");
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(h != nullptr);
    bool active = true;
    REQUIRE(sao_ui_widget_button_is_active(h, &active) == SAO_STATUS_OK);
    REQUIRE(active == false);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_preferred_size_matches_text_and_padding",
          "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("hello", SAO_UI_BTN_NORMAL,
                                              false, false, 8, 4);
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_widget_button_preferred_size(h, &w, &hpx) == SAO_STATUS_OK);
    // "hello" = 5 glyphs × 8 = 40 px, + 2×pad_x 8 = 56 px total width.
    REQUIRE(w == 5 * 8 + 2 * 8);
    // Glyph height 16 + 2×pad_y 4 = 24.
    REQUIRE(hpx == 16 + 2 * 4);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_hit_test_inside_and_outside",
          "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("go", SAO_UI_BTN_NORMAL,
                                              false, false, 4, 2);
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_widget_button_preferred_size(h, &w, &hpx) == SAO_STATUS_OK);
    bool hit = false;
    // Origin inside.
    REQUIRE(sao_ui_widget_button_hit_test(h, {0.5f, 0.5f}, &hit)
            == SAO_STATUS_OK);
    REQUIRE(hit == true);
    // Right beyond the right edge.
    REQUIRE(sao_ui_widget_button_hit_test(h,
        {static_cast<float>(w) + 1.0f, 1.0f}, &hit) == SAO_STATUS_OK);
    REQUIRE(hit == false);
    // Negative.
    REQUIRE(sao_ui_widget_button_hit_test(h, {-1.0f, 0.0f}, &hit)
            == SAO_STATUS_OK);
    REQUIRE(hit == false);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_dispatch_click_fires_callback_with_action_id",
          "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("primary", SAO_UI_BTN_GOLD);
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    ClickSink sink;
    REQUIRE(sao_ui_button_set_click_handler(h, sink_click_cb, &sink)
            == SAO_STATUS_OK);
    int32_t action_id = -1;
    // Full down/up cycle → click.
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseDown,
        &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 0);   // no click on down alone
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseUp,
        &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 1);
    REQUIRE(action_id == SAO_UI_BTN_GOLD);
    sao_ui_widget_input_family_destroy(h);
}

TEST_CASE("button_disabled_button_swallows_click",
          "[ui][widget][input][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiButtonSpec spec = make_button_spec("no");
    REQUIRE(sao_ui_button_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    ClickSink sink;
    REQUIRE(sao_ui_button_set_click_handler(h, sink_click_cb, &sink)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_button_set_disabled(h, true) == SAO_STATUS_OK);
    int32_t action_id = -1;
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseDown,
        &action_id) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseUp,
        &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 0);
    REQUIRE(action_id == -1);
    // Re-enable → clicks fire again.
    REQUIRE(sao_ui_button_set_disabled(h, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseDown,
        &action_id) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_button_dispatch_event(h, kBtnEvtMouseUp,
        &action_id) == SAO_STATUS_OK);
    REQUIRE(sink.hits.load() == 1);
    sao_ui_widget_input_family_destroy(h);
}
