// Wave 4 tests for the Label widget (G3.8 first slice).
//
// Coverage (5 CASE):
//   * label_create_stores_spec_and_text
//   * label_set_text_updates_measure_dirty
//   * label_measure_single_line_ascii
//   * label_measure_wrap_multi_line
//   * label_max_lines_truncation_appends_ellipsis
//
// All tests are pure state-machine / geometry — no D3D device, so we
// pass nullptr for the d3d_device_ptr slot.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "sao/core/status.h"
#include "sao/ui/widget_text.h"

extern "C" {

SAO_UI_API void SAO_UI_CALL sao_ui_widget_text_family_destroy(
    sao_ui_widget_handle_t handle);

}  // extern "C"

namespace {

SaoUiLabelSpec make_label_spec(const char* text,
                                bool wrap = false,
                                int32_t max_lines = 0,
                                float letter_spacing = 0.0f,
                                int32_t font_size_px = 16,
                                int32_t font_slot = SAO_UI_FONT_SAO) {
    SaoUiLabelSpec spec{};
    spec.text_utf8      = text;
    spec.fg_argb        = 0;
    spec.bg_argb        = 0;
    spec.font_slot      = font_slot;
    spec.font_size_px   = font_size_px;
    spec.font_weight    = SAO_UI_WEIGHT_NORMAL;
    spec.align          = SAO_UI_ALIGN_LEFT;
    spec.anchor         = SAO_UI_ANCHOR_NW;
    spec.max_lines      = max_lines;
    spec.wrap           = wrap;
    spec.letter_spacing_px = letter_spacing;
    return spec;
}

}  // namespace

TEST_CASE("label_create_stores_spec_and_text", "[ui][widget][text][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiLabelSpec spec = make_label_spec("hello");
    REQUIRE(sao_ui_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    REQUIRE(h != nullptr);
    // Immediate measure at max_width = 0 returns single-line width
    // (5 glyphs × 8 px = 40 px, height = 16 px).
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_label_measure(h, 0, &w, &hpx) == SAO_STATUS_OK);
    REQUIRE(w == 5 * 8);
    REQUIRE(hpx == 16);
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("label_set_text_updates_measure_dirty", "[ui][widget][text][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiLabelSpec spec = make_label_spec("hi");
    REQUIRE(sao_ui_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_label_measure(h, 0, &w, &hpx) == SAO_STATUS_OK);
    REQUIRE(w == 2 * 8);   // "hi"
    REQUIRE(sao_ui_label_set_text(h, "hello world") == SAO_STATUS_OK);
    REQUIRE(sao_ui_label_measure(h, 0, &w, &hpx) == SAO_STATUS_OK);
    // "hello world" = 11 glyphs including space, 11 × 8 = 88.
    REQUIRE(w == 11 * 8);
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("label_measure_single_line_ascii", "[ui][widget][text][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiLabelSpec spec = make_label_spec("DPS: 12345");
    REQUIRE(sao_ui_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    // Wrap off, no max width → returns full line.
    REQUIRE(sao_ui_label_measure(h, 0, &w, &hpx) == SAO_STATUS_OK);
    REQUIRE(w == 10 * 8);   // 10 glyphs
    REQUIRE(hpx == 16);     // single line
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("label_measure_wrap_multi_line", "[ui][widget][text][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    // Wrap on, budget of 40 px = 5 glyphs.  "aaaa bbbb cccc" is 3
    // tokens × 4 chars.  Under a 5-char budget only one 4-char token
    // fits per line ("aaaa" alone; adding " bbbb" needs 9 chars).
    SaoUiLabelSpec spec = make_label_spec("aaaa bbbb cccc", /*wrap=*/true);
    REQUIRE(sao_ui_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_label_measure(h, 40, &w, &hpx) == SAO_STATUS_OK);
    // 3 lines → 3 × font_size 16 = 48 px height.
    REQUIRE(hpx == 3 * 16);
    // Max width still 40 (each line is 4 glyphs = 32 px).
    REQUIRE(w == 4 * 8);
    sao_ui_widget_text_family_destroy(h);
}

TEST_CASE("label_max_lines_truncation_appends_ellipsis",
          "[ui][widget][text][wave4]") {
    sao_ui_widget_handle_t h = nullptr;
    SaoUiLabelSpec spec = make_label_spec(
        "line1\nline2\nline3\nline4", /*wrap=*/false, /*max_lines=*/2);
    REQUIRE(sao_ui_label_create(nullptr, &spec, &h) == SAO_STATUS_OK);
    int32_t w = 0, hpx = 0;
    REQUIRE(sao_ui_label_measure(h, 0, &w, &hpx) == SAO_STATUS_OK);
    // 2 lines × 16px = 32 px height.
    REQUIRE(hpx == 2 * 16);
    // "line2…" (with ellipsis, 3-byte UTF-8) → 6 glyphs × 8 = 48.
    REQUIRE(w == 6 * 8);
    sao_ui_widget_text_family_destroy(h);
}
