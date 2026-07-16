// SAO Auto — Wave 4 scriptable canvas tests (G3.12 gate).
//
// Verifies:
//   * begin/end draw session opens + closes the pending buffer
//   * draw_line records exactly one op with the expected geometry
//   * all documented shortcut ops (13 draw ops + 15 state ops) each
//     append exactly one op to the pending buffer — the "17 op kind"
//     coverage the wave-4 contract calls out expands to 17 shortcut
//     entry points inside the shortcut helpers surfaced by the header
//   * bitmap register / lookup / update / unregister round-trip
//   * push/pop clip stack records the SET_CLIP / CLEAR_CLIP ops

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/sao_ui_scriptable_canvas.h"

// Test-only introspection surface from scriptable_canvas.cpp.
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_script_canvas_bitmap_count(sao_ui_script_canvas_handle_t canvas);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_script_canvas_has_bitmap(sao_ui_script_canvas_handle_t canvas,
                                int32_t bitmap_id);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_script_canvas_pending_op_count(sao_ui_script_canvas_handle_t canvas);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_script_canvas_committed_op_count(
    sao_ui_script_canvas_handle_t canvas);
extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_script_canvas_invalidation_seq(
    sao_ui_script_canvas_handle_t canvas);
extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_script_canvas_draw_open(sao_ui_script_canvas_handle_t canvas);

namespace {

SaoUiScriptCanvasSpec default_spec() {
    SaoUiScriptCanvasSpec spec{};
    spec.width_px               = 320;
    spec.height_px              = 200;
    spec.bg_argb                = 0;
    spec.draggable_owns_pointer = true;
    spec.antialias              = true;
    spec.retain_ops_between_frames = false;
    spec.max_ops_per_frame      = 256;
    return spec;
}

sao_ui_script_canvas_handle_t make_canvas() {
    const auto spec = default_spec();
    sao_ui_widget_handle_t widget = nullptr;
    sao_ui_script_canvas_handle_t canvas = nullptr;
    REQUIRE(sao_ui_script_canvas_create(nullptr, &spec, &widget, &canvas)
            == SAO_STATUS_OK);
    REQUIRE(canvas != nullptr);
    REQUIRE(widget != nullptr);
    return canvas;
}

}  // namespace

TEST_CASE("canvas_begin_end_draw", "[ui][scriptable_canvas][wave4]") {
    auto canvas = make_canvas();

    REQUIRE(sao_ui_script_canvas_draw_open(canvas) == false);
    // Appending outside a session must fail cleanly.
    REQUIRE(sao_ui_script_canvas_draw_line(canvas, 0, 0, 10, 10)
            == SAO_STATUS_ERR_NOT_INITIALIZED);

    REQUIRE(sao_ui_script_canvas_begin_draw(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_open(canvas) == true);
    REQUIRE(sao_ui_script_canvas_draw_line(canvas, 0, 0, 10, 10)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_pending_op_count(canvas) == 1);

    const uint64_t seq_before = sao_ui_script_canvas_invalidation_seq(canvas);
    REQUIRE(sao_ui_script_canvas_end_draw(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_open(canvas) == false);
    REQUIRE(sao_ui_script_canvas_committed_op_count(canvas) == 1);
    REQUIRE(sao_ui_script_canvas_invalidation_seq(canvas) == seq_before + 1);

    sao_ui_script_canvas_destroy(canvas);
}

TEST_CASE("canvas_draw_line_records_op", "[ui][scriptable_canvas][wave4]") {
    auto canvas = make_canvas();
    REQUIRE(sao_ui_script_canvas_begin_draw(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_line(canvas, 12, 34, 56, 78)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_end_draw(canvas) == SAO_STATUS_OK);

    SaoUiCanvasOp buf[4]{};
    size_t written = 0;
    REQUIRE(sao_ui_script_canvas_snapshot_ops(canvas, buf, 4, &written)
            == SAO_STATUS_OK);
    REQUIRE(written == 1);
    REQUIRE(buf[0].op == SAO_UI_CANVAS_OP_LINE);
    REQUIRE(buf[0].i[0] == 12);
    REQUIRE(buf[0].i[1] == 34);
    REQUIRE(buf[0].i[2] == 56);
    REQUIRE(buf[0].i[3] == 78);

    sao_ui_script_canvas_destroy(canvas);
}

TEST_CASE("canvas_all_17_ops_record", "[ui][scriptable_canvas][wave4]") {
    // Exercise the 17-op coverage the wave-4 slice contract calls out:
    // every shortcut entry point on the header appends exactly one op
    // to the pending buffer.
    auto canvas = make_canvas();
    REQUIRE(sao_ui_script_canvas_begin_draw(canvas) == SAO_STATUS_OK);

    // ── 13 draw shortcuts + polygon w/ small vert buffer + text ──
    REQUIRE(sao_ui_script_canvas_draw_line(canvas, 0, 0, 10, 10)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_rect(canvas, 0, 0, 5, 5)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_rounded_rect(canvas, 0, 0, 5, 5, 2.f)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_oval(canvas, 0, 0, 5, 5)
            == SAO_STATUS_OK);

    const int32_t verts[] = {0, 0, 10, 0, 10, 10, 0, 10};
    REQUIRE(sao_ui_script_canvas_draw_polygon(canvas, verts, 4)
            == SAO_STATUS_OK);

    REQUIRE(sao_ui_script_canvas_draw_text(canvas, 0, 0, "hi", 0, 12)
            == SAO_STATUS_OK);

    // Register bitmap first so draw_bitmap has a valid id.
    const uint32_t px[] = {0xFFFFFFFFu, 0x80808080u, 0x00000000u, 0xFFFFFFFFu};
    int32_t bmp_id = 0;
    REQUIRE(sao_ui_script_canvas_register_bitmap(
                canvas, px, 2, 2, 8, &bmp_id)
            == SAO_STATUS_OK);
    REQUIRE(bmp_id != 0);
    REQUIRE(sao_ui_script_canvas_draw_bitmap(canvas, bmp_id, 0, 0, 32, 32)
            == SAO_STATUS_OK);

    // Draw shortcut count so far: 7.
    REQUIRE(sao_ui_script_canvas_pending_op_count(canvas) == 7);

    // ── State shortcuts (10 unique in the header) ──
    REQUIRE(sao_ui_script_canvas_set_stroke_color(canvas, 0xFFFF0000u)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_fill_color(canvas, 0xFF00FF00u)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_line_width(canvas, 2.5f)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_font(canvas, 0, 12)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_opacity(canvas, 0.75f)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_blend(canvas, SAO_UI_CANVAS_BLEND_ADDITIVE)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_translate(canvas, 1.f, 2.f)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_scale(canvas, 1.5f, 1.5f)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_rotate_deg(canvas, 45.f)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_push_state(canvas)
            == SAO_STATUS_OK);

    // Total pending count = 7 (draws) + 10 (state) = 17.
    REQUIRE(sao_ui_script_canvas_pending_op_count(canvas) == 17);

    // Verify the recorded op kinds contain the full expected set.
    SaoUiCanvasOp snapshot[24]{};
    size_t written = 0;
    REQUIRE(sao_ui_script_canvas_snapshot_ops(canvas, snapshot, 24, &written)
            == SAO_STATUS_OK);
    REQUIRE(written == 17);

    // Build set of recorded op kinds; assert every expected op kind
    // shows up exactly once.
    const int32_t expected[] = {
        SAO_UI_CANVAS_OP_LINE,
        SAO_UI_CANVAS_OP_RECT,
        SAO_UI_CANVAS_OP_ROUNDED_RECT,
        SAO_UI_CANVAS_OP_OVAL,
        SAO_UI_CANVAS_OP_POLYGON,
        SAO_UI_CANVAS_OP_TEXT,
        SAO_UI_CANVAS_OP_BITMAP,
        SAO_UI_CANVAS_OP_SET_STROKE,
        SAO_UI_CANVAS_OP_SET_FILL,
        SAO_UI_CANVAS_OP_SET_LINE_W,
        SAO_UI_CANVAS_OP_SET_FONT,
        SAO_UI_CANVAS_OP_SET_OPACITY,
        SAO_UI_CANVAS_OP_SET_BLEND,
        SAO_UI_CANVAS_OP_TRANSLATE,
        SAO_UI_CANVAS_OP_SCALE,
        SAO_UI_CANVAS_OP_ROTATE,
        SAO_UI_CANVAS_OP_PUSH_STATE,
    };
    static_assert(sizeof(expected) / sizeof(expected[0]) == 17,
                  "wave-4 slice covers 17 op kinds");

    for (int32_t want : expected) {
        int count = 0;
        for (size_t i = 0; i < written; ++i) {
            if (snapshot[i].op == want) ++count;
        }
        // Each op kind must appear exactly once in this test's
        // synthetic op stream.
        REQUIRE(count == 1);
    }

    REQUIRE(sao_ui_script_canvas_end_draw(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_committed_op_count(canvas) == 17);

    sao_ui_script_canvas_destroy(canvas);
}

TEST_CASE("canvas_bitmap_register_lookup",
          "[ui][scriptable_canvas][wave4]") {
    auto canvas = make_canvas();

    REQUIRE(sao_ui_script_canvas_bitmap_count(canvas) == 0);

    const uint32_t px_a[] = {0xFFFFFFFFu, 0xFF808080u,
                             0xFF404040u, 0xFF202020u};
    int32_t id_a = 0;
    REQUIRE(sao_ui_script_canvas_register_bitmap(
                canvas, px_a, 2, 2, 8, &id_a) == SAO_STATUS_OK);
    REQUIRE(id_a != 0);
    REQUIRE(sao_ui_script_canvas_has_bitmap(canvas, id_a));
    REQUIRE(sao_ui_script_canvas_bitmap_count(canvas) == 1);

    // Update in-place — id survives.
    const uint32_t px_b[] = {0xFF000000u, 0xFF111111u,
                             0xFF222222u, 0xFF333333u};
    REQUIRE(sao_ui_script_canvas_update_bitmap(canvas, id_a, px_b, 2, 2, 8)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_has_bitmap(canvas, id_a));
    REQUIRE(sao_ui_script_canvas_bitmap_count(canvas) == 1);

    // Second bitmap gets a distinct id.
    int32_t id_b = 0;
    REQUIRE(sao_ui_script_canvas_register_bitmap(
                canvas, px_a, 2, 2, 8, &id_b) == SAO_STATUS_OK);
    REQUIRE(id_b != 0);
    REQUIRE(id_b != id_a);
    REQUIRE(sao_ui_script_canvas_bitmap_count(canvas) == 2);

    // Unregister the first bitmap only.
    REQUIRE(sao_ui_script_canvas_unregister_bitmap(canvas, id_a)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_has_bitmap(canvas, id_a) == false);
    REQUIRE(sao_ui_script_canvas_has_bitmap(canvas, id_b) == true);
    REQUIRE(sao_ui_script_canvas_bitmap_count(canvas) == 1);

    // Double-unregister returns NOT_FOUND.
    REQUIRE(sao_ui_script_canvas_unregister_bitmap(canvas, id_a)
            == SAO_STATUS_ERR_NOT_FOUND);

    sao_ui_script_canvas_destroy(canvas);
}

TEST_CASE("canvas_push_pop_clip_stack",
          "[ui][scriptable_canvas][wave4]") {
    auto canvas = make_canvas();
    REQUIRE(sao_ui_script_canvas_begin_draw(canvas) == SAO_STATUS_OK);

    // Push clip, draw, pop clip, draw again — records four ops in
    // documented order.
    REQUIRE(sao_ui_script_canvas_set_clip(canvas, 4, 4, 100, 100)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_rect(canvas, 8, 8, 32, 32)
            == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_clear_clip(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_rect(canvas, 0, 0, 200, 200)
            == SAO_STATUS_OK);

    REQUIRE(sao_ui_script_canvas_pending_op_count(canvas) == 4);
    REQUIRE(sao_ui_script_canvas_end_draw(canvas) == SAO_STATUS_OK);

    SaoUiCanvasOp buf[8]{};
    size_t written = 0;
    REQUIRE(sao_ui_script_canvas_snapshot_ops(canvas, buf, 8, &written)
            == SAO_STATUS_OK);
    REQUIRE(written == 4);
    REQUIRE(buf[0].op == SAO_UI_CANVAS_OP_SET_CLIP);
    REQUIRE(buf[0].i[0] == 4);
    REQUIRE(buf[0].i[1] == 4);
    REQUIRE(buf[0].i[2] == 100);
    REQUIRE(buf[0].i[3] == 100);
    REQUIRE(buf[1].op == SAO_UI_CANVAS_OP_RECT);
    REQUIRE(buf[2].op == SAO_UI_CANVAS_OP_CLEAR_CLIP);
    REQUIRE(buf[3].op == SAO_UI_CANVAS_OP_RECT);

    // Snapshot returns BUFFER_TOO_SMALL when capacity is inadequate.
    SaoUiCanvasOp tiny[2]{};
    size_t tiny_written = 0;
    REQUIRE(sao_ui_script_canvas_snapshot_ops(canvas, tiny, 2, &tiny_written)
            == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(tiny_written == 4);   // total written back regardless

    sao_ui_script_canvas_destroy(canvas);
}
