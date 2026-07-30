// Deterministic CPU raster fixture coverage.
#include <catch2/catch_test_macros.hpp>

#include "sao/ui/d2d_widgets.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

struct Pixel {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
};

std::vector<Pixel> snapshot(sao_ui_offscreen_raster_handle_t raster) {
    size_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    REQUIRE(sao_ui_offscreen_raster_snapshot(raster, nullptr, 0, &bytes, &width, &height,
                                             &stride) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<Pixel> pixels(bytes / sizeof(Pixel));
    REQUIRE(sao_ui_offscreen_raster_snapshot(raster, reinterpret_cast<uint8_t*>(pixels.data()),
                                             bytes, &bytes, &width, &height,
                                             &stride) == SAO_STATUS_OK);
    REQUIRE(stride == width * sizeof(Pixel));
    REQUIRE(pixels.size() == static_cast<size_t>(width) * height);
    return pixels;
}

std::vector<Pixel> paint_widget_snapshot(sao_ui_widget_handle_t widget, uint32_t raster_width,
                                         uint32_t raster_height, float x, float y, float width,
                                         float height) {
    SaoUiOffscreenRasterDesc desc{raster_width, raster_height, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint(widget, context, x, y, width, height) == SAO_STATUS_OK);
    auto pixels = snapshot(raster);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    return pixels;
}

} // namespace

TEST_CASE("offscreen raster composites premultiplied BGRA and honors clips", "[ui][raster]") {
    SaoUiOffscreenRasterDesc desc{4, 4, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);

    REQUIRE(sao_ui_paint_ctx_fill_rect(context, 0, 0, 4, 4, 0x80ff0000U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_push_clip(context, 1, 1, 2, 2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_fill_rect(context, 0, 0, 4, 4, 0xff00ff00U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_pop_clip(context) == SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    REQUIRE(pixels[0].b == 0);
    REQUIRE(pixels[0].g == 0);
    REQUIRE(pixels[0].r == 128);
    REQUIRE(pixels[0].a == 128);
    REQUIRE(pixels[1U + 1U * 4U].b == 0);
    REQUIRE(pixels[1U + 1U * 4U].g == 255);
    REQUIRE(pixels[1U + 1U * 4U].r == 0);
    REQUIRE(pixels[1U + 1U * 4U].a == 255);

    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
}

TEST_CASE("generic progress widget produces stable fill geometry", "[ui][raster][widget]") {
    SaoUiOffscreenRasterDesc desc{20, 8, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_BAR, nullptr, &widget) == SAO_STATUS_OK);
    constexpr char props[] = "{\"ratio\":0.5,\"fill\":\"#000000\",\"accent\":\"#00ff00\"}";
    REQUIRE(sao_ui_widget_apply_props(widget, reinterpret_cast<const uint8_t*>(props),
                                      sizeof(props) - 1U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint(widget, context, 0, 0, 20, 8) == SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    REQUIRE(pixels[4U * 20U + 5U].g == 255);
    REQUIRE(pixels[4U * 20U + 14U].g == 0);
    bool hit = false;
    REQUIRE(sao_ui_widget_hit_test(widget, 5, 4, &hit) == SAO_STATUS_OK);
    REQUIRE(hit);

    sao_ui_widget_destroy(widget);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
}

TEST_CASE("generic scrollbar paints proportional horizontal and vertical thumbs",
          "[ui][raster][widget][scroll]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_SCROLLBAR, nullptr, &widget) == SAO_STATUS_OK);
    constexpr char props[] =
        R"({"value":0.5,"page_size":25,"content_size":100,"nudge_step":0.1,"keyboard_nudge":true,"show_arrows":false,"track":"#101010","thumb":"#4080c0","thumb_hover":"#80a0e0","thumb_active":"#204060"})";
    REQUIRE(sao_ui_widget_apply_props(widget, reinterpret_cast<const uint8_t*>(props),
                                      sizeof(props) - 1U) == SAO_STATUS_OK);

    auto pixels = paint_widget_snapshot(widget, 100, 20, 0, 0, 100, 20);
    CHECK(pixels[10U * 100U + 10U].r == 0x10U);
    CHECK(pixels[10U * 100U + 50U].r == 0x40U);
    CHECK(pixels[10U * 100U + 50U].g == 0x80U);
    CHECK(pixels[10U * 100U + 50U].b == 0xc0U);

    int32_t part = SAO_UI_SCROLLBAR_HIT_NONE;
    float page_value = -1.0F;
    REQUIRE(sao_ui_widget_scrollbar_hit_test(widget, 50, 10, &part, &page_value) == SAO_STATUS_OK);
    CHECK(part == SAO_UI_SCROLLBAR_HIT_THUMB);
    REQUIRE(sao_ui_widget_scrollbar_hit_test(widget, 10, 10, &part, &page_value) == SAO_STATUS_OK);
    CHECK(part == SAO_UI_SCROLLBAR_HIT_TRACK_BEFORE);
    CHECK(std::fabs(page_value - 0.25F) < 1e-6F);
    REQUIRE(sao_ui_widget_scrollbar_hit_test(widget, 90, 10, &part, &page_value) == SAO_STATUS_OK);
    CHECK(part == SAO_UI_SCROLLBAR_HIT_TRACK_AFTER);
    CHECK(std::fabs(page_value - 0.75F) < 1e-6F);

    REQUIRE(sao_ui_widget_set_hovered(widget, true) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 100, 20, 0, 0, 100, 20);
    CHECK(pixels[10U * 100U + 50U].r == 0x80U);
    CHECK(pixels[10U * 100U + 50U].g == 0xa0U);
    CHECK(pixels[10U * 100U + 50U].b == 0xe0U);
    REQUIRE(sao_ui_widget_set_pressed(widget, true) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 100, 20, 0, 0, 100, 20);
    CHECK(pixels[10U * 100U + 50U].r == 0x20U);
    CHECK(pixels[10U * 100U + 50U].g == 0x40U);
    CHECK(pixels[10U * 100U + 50U].b == 0x60U);

    REQUIRE(sao_ui_widget_set_pressed(widget, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_set_hovered(widget, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_set_value(widget, 1.0F) == SAO_STATUS_OK);
    float value = 0.0F;
    REQUIRE(sao_ui_widget_get_value(widget, &value) == SAO_STATUS_OK);
    CHECK(value == 1.0F);
    pixels = paint_widget_snapshot(widget, 100, 20, 0, 0, 100, 20);
    CHECK(pixels[10U * 100U + 50U].r == 0x10U);
    CHECK(pixels[10U * 100U + 90U].r == 0x40U);

    REQUIRE(sao_ui_widget_set_value(widget, 0.5F) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 20, 100, 0, 0, 20, 100);
    CHECK(pixels[10U * 20U + 10U].r == 0x10U);
    CHECK(pixels[50U * 20U + 10U].r == 0x40U);
    REQUIRE(sao_ui_widget_scrollbar_hit_test(widget, 10, 50, &part, &page_value) == SAO_STATUS_OK);
    CHECK(part == SAO_UI_SCROLLBAR_HIT_THUMB);
    REQUIRE(sao_ui_widget_scrollbar_hit_test(widget, 10, 10, &part, &page_value) == SAO_STATUS_OK);
    CHECK(part == SAO_UI_SCROLLBAR_HIT_TRACK_BEFORE);

    sao_ui_widget_destroy(widget);
}

TEST_CASE("generic widget interaction visuals and disabled hit gate are deterministic",
          "[ui][raster][widget][state]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, &widget) == SAO_STATUS_OK);
    constexpr char props[] =
        R"({"fill":"#202020","border":"#404040","accent":"#00ff00","focus":"#ff00ff","radius":0,"border_width":1,"enabled":true})";
    REQUIRE(sao_ui_widget_apply_props(widget, reinterpret_cast<const uint8_t*>(props),
                                      sizeof(props) - 1U) == SAO_STATUS_OK);

    auto pixels = paint_widget_snapshot(widget, 20, 12, 2, 2, 16, 8);
    CHECK(pixels[6U * 20U + 10U].r == 0x20U);
    REQUIRE(sao_ui_widget_set_hovered(widget, true) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 20, 12, 2, 2, 16, 8);
    CHECK(pixels[6U * 20U + 10U].r == 0x32U);

    REQUIRE(sao_ui_widget_set_pressed(widget, true) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 20, 12, 2, 2, 16, 8);
    CHECK(pixels[6U * 20U + 10U].r == 0x1cU);
    CHECK(pixels[6U * 20U + 4U].r != pixels[6U * 20U + 10U].r);

    REQUIRE(sao_ui_widget_set_pressed(widget, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_set_hovered(widget, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_set_focused(widget, true) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 20, 12, 2, 2, 16, 8);
    CHECK(pixels[6U * 20U + 1U].r == 0xffU);
    CHECK(pixels[6U * 20U + 1U].b == 0xffU);

    REQUIRE(sao_ui_widget_set_enabled(widget, false) == SAO_STATUS_OK);
    pixels = paint_widget_snapshot(widget, 20, 12, 2, 2, 16, 8);
    CHECK(pixels[6U * 20U + 10U].a == 102U);
    bool hit = true;
    REQUIRE(sao_ui_widget_hit_test(widget, 5, 4, &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);

    sao_ui_widget_destroy(widget);
}

TEST_CASE("generic widget RGBA colors preserve semitransparent and transparent alpha",
          "[ui][raster][widget][color][alpha]") {
    SaoUiOffscreenRasterDesc desc{4, 2, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_ui_widget_handle_t translucent = nullptr;
    sao_ui_widget_handle_t transparent = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_BAR, nullptr, &translucent) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_BAR, nullptr, &transparent) == SAO_STATUS_OK);

    constexpr char translucent_props[] = R"({"fill":"#ff000080"})";
    constexpr char transparent_props[] = R"({"fill":"#00ff0000"})";
    REQUIRE(sao_ui_widget_apply_props(translucent,
                                      reinterpret_cast<const uint8_t*>(translucent_props),
                                      sizeof(translucent_props) - 1U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_apply_props(transparent,
                                      reinterpret_cast<const uint8_t*>(transparent_props),
                                      sizeof(transparent_props) - 1U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint(translucent, context, 0, 0, 2, 2) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_paint(transparent, context, 2, 0, 2, 2) == SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    CHECK(pixels[0].b == 0);
    CHECK(pixels[0].g == 0);
    CHECK(pixels[0].r == 128);
    CHECK(pixels[0].a == 128);
    CHECK(pixels[2].b == 0);
    CHECK(pixels[2].g == 0);
    CHECK(pixels[2].r == 0);
    CHECK(pixels[2].a == 0);

    sao_ui_widget_destroy(transparent);
    sao_ui_widget_destroy(translucent);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
}

TEST_CASE("scriptable canvas rasterizes stateful line rectangle and bitmap operations",
          "[ui][raster][canvas]") {
    SaoUiOffscreenRasterDesc desc{16, 16, 0x00000000U};
    SaoUiScriptCanvasSpec spec{16, 16, 0, false, false, false, 0, 64};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_script_canvas_handle_t canvas = nullptr;
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_create(nullptr, &spec, &widget, &canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_begin_draw(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_fill_color(canvas, 0xffff0000U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_rect(canvas, 1, 1, 4, 4) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_set_stroke_color(canvas, 0xff00ff00U) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_line(canvas, 0, 10, 10, 10) == SAO_STATUS_OK);
    const std::array<Pixel, 1> blue{{{255, 0, 0, 255}}};
    int32_t bitmap_id = 0;
    REQUIRE(sao_ui_script_canvas_register_bitmap(canvas, blue.data(), 1, 1, sizeof(Pixel),
                                                 &bitmap_id) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_draw_bitmap(canvas, bitmap_id, 12, 12, 2, 2) == SAO_STATUS_OK);
    SaoUiCanvasOp cubic{};
    cubic.op = SAO_UI_CANVAS_OP_CUBIC_CURVE;
    cubic.i[0] = 0;
    cubic.i[1] = 14;
    cubic.i[2] = 4;
    cubic.i[3] = 14;
    cubic.i[4] = 8;
    cubic.i[5] = 14;
    cubic.i_ex[0] = 15;
    cubic.i_ex[1] = 14;
    REQUIRE(sao_ui_script_canvas_submit_ops(canvas, &cubic, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_end_draw(canvas) == SAO_STATUS_OK);
    REQUIRE(sao_ui_script_canvas_rasterize(canvas, raster, 0, 0) == SAO_STATUS_OK);

    const auto pixels = snapshot(raster);
    REQUIRE(pixels[2U * 16U + 2U].r == 255);
    REQUIRE(pixels[9U * 16U + 5U].g == 255);
    REQUIRE(pixels[12U * 16U + 12U].b == 255);
    REQUIRE(pixels[14U * 16U + 15U].g == 255);

    sao_ui_script_canvas_destroy(canvas);
    sao_ui_offscreen_raster_destroy(raster);
}
