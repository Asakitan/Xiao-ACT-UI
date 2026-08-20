// Deterministic CPU raster fixture coverage.
#include <catch2/catch_test_macros.hpp>

#include "sao/ui/d2d_widgets.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"
#include "sao/ui/theme.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
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


TEST_CASE("generic widget semantic styles use the dark active theme",
          "[ui][raster][widget][style]") {
    SaoUiThemeId original_theme = SAO_UI_THEME_DARK;
    REQUIRE(sao_ui_theme_get_active_id(&original_theme) == SAO_STATUS_OK);
    struct ThemeReset {
        SaoUiThemeId original;
        ~ThemeReset() {
            (void)sao_ui_theme_set_active_id(original);
        }
    } reset{original_theme};
    REQUIRE(sao_ui_theme_set_active_id(SAO_UI_THEME_DARK) == SAO_STATUS_OK);

    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_ACTION_BUTTON, nullptr, & widget) == SAO_STATUS_OK);
    constexpr char props[] = R"({"style":"danger","text":"Danger"})";
    REQUIRE(sao_ui_widget_apply_props(widget, reinterpret_cast<const uint8_t*>(props),
                                      sizeof(props) - 1U) == SAO_STATUS_OK);
    const auto pixels = paint_widget_snapshot(widget, 48, 32, 0, 0, 48, 32);
    const Pixel center = pixels[16U * 48U + 24U];
    const uint32_t expected = sao_ui_theme_resolve_color(SAO_UI_THEME_DARK, SAO_UI_TOKEN_APP_RED);
    CHECK(center.r == static_cast<uint8_t>((expected >> 16U) & 0xffU));
    CHECK(center.g == static_cast<uint8_t>((expected >> 8U) & 0xffU));
    CHECK(center.b == static_cast<uint8_t>(expected & 0xffU));
    REQUIRE(sao_ui_widget_set_hovered(widget, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_widget_set_pressed(widget, true) == SAO_STATUS_OK);
    const auto pressed = paint_widget_snapshot(widget, 48, 32, 0, 0, 48, 32);
    CHECK(pressed[16U * 48U + 24U].r != center.r);
    sao_ui_widget_destroy(widget);
}

TEST_CASE("scrollbar geometry validates finite bounds and output postconditions",
          "[ui][raster][widget][scroll]") {
    auto compute = [](float bounds_x, float bounds_y, float bounds_width, float bounds_height,
                      float page_size, float content_size, float value, int32_t show_arrows,
                      SaoUiScrollbarGeometry* geometry) {
        return sao_ui_scrollbar_geometry_compute(bounds_x, bounds_y, bounds_width, bounds_height,
                                                 page_size, content_size, value, show_arrows,
                                                 geometry);
    };
    SaoUiScrollbarGeometry geometry{};
    constexpr float kValid[] = {0.0F, 0.0F, 100.0F, 20.0F, 25.0F, 100.0F, 0.5F};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();

    CHECK(compute(0.0F, 0.0F, 0.0F, 20.0F, 25.0F, 100.0F, 0.5F, 0, &geometry) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(compute(0.0F, 0.0F, 100.0F, 0.0F, 25.0F, 100.0F, 0.5F, 0, &geometry) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(compute(0.0F, 0.0F, -1.0F, 20.0F, 25.0F, 100.0F, 0.5F, 0, &geometry) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(compute(0.0F, 0.0F, 100.0F, -1.0F, 25.0F, 100.0F, 0.5F, 0, &geometry) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    for (size_t index = 0; index < sizeof(kValid) / sizeof(kValid[0]); ++index) {
        auto inputs = std::array<float, 7>{kValid[0], kValid[1], kValid[2], kValid[3],
                                           kValid[4], kValid[5], kValid[6]};
        inputs[index] = nan;
        CHECK(compute(inputs[0], inputs[1], inputs[2], inputs[3], inputs[4], inputs[5], inputs[6],
                      0, &geometry) == SAO_STATUS_ERR_INVALID_ARGUMENT);
        inputs[index] = infinity;
        CHECK(compute(inputs[0], inputs[1], inputs[2], inputs[3], inputs[4], inputs[5], inputs[6],
                      0, &geometry) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    }

    const float float_max = std::numeric_limits<float>::max();
    CHECK(compute(float_max * 0.75F, 0.0F, float_max * 0.5F, 20.0F, 25.0F, 100.0F, 0.5F, 0,
                  &geometry) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(compute(0.0F, float_max * 0.75F, 20.0F, float_max * 0.5F, 25.0F, 100.0F, 0.5F, 0,
                  &geometry) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    const auto check_postconditions = [](const SaoUiScrollbarGeometry& result,
                                         int32_t horizontal) {
        CHECK(result.horizontal == horizontal);
        const float values[] = {
            result.decrement_x,      result.decrement_y,      result.decrement_width,
            result.decrement_height, result.increment_x,      result.increment_y,
            result.increment_width,  result.increment_height, result.track_hit_x,
            result.track_hit_y,      result.track_hit_width,  result.track_hit_height,
            result.track_x,          result.track_y,          result.track_width,
            result.track_height,     result.thumb_x,         result.thumb_y,
            result.thumb_width,      result.thumb_height,    result.travel,
            result.page_fraction};
        for (float value : values)
            CHECK(std::isfinite(value));
        const float dimensions[] = {result.decrement_width, result.decrement_height,
                                    result.increment_width, result.increment_height,
                                    result.track_hit_width, result.track_hit_height,
                                    result.track_width, result.track_height, result.thumb_width,
                                    result.thumb_height, result.travel, result.page_fraction};
        for (float value : dimensions)
            CHECK(value >= 0.0F);
    };

    REQUIRE(compute(-20.0F, 8.0F, 160.0F, 24.0F, 40.0F, 160.0F, 0.4F, 1, &geometry) ==
            SAO_STATUS_OK);
    check_postconditions(geometry, 1);
    REQUIRE(compute(8.0F, -20.0F, 24.0F, 160.0F, 40.0F, 160.0F, 0.4F, 1, &geometry) ==
            SAO_STATUS_OK);
    check_postconditions(geometry, 0);
}

TEST_CASE("paint context and raster primitives reject unbounded work",
          "[ui][raster][bounds][abi]") {
    sao_ui_paint_ctx_handle_t unavailable =
        reinterpret_cast<sao_ui_paint_ctx_handle_t>(uintptr_t{1});
    CHECK(sao_ui_paint_ctx_create(nullptr, nullptr, &unavailable) ==
          SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(unavailable == nullptr);

    SaoUiOffscreenRasterDesc desc{16, 16, 0x00000000U};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);

    const float float_max = std::numeric_limits<float>::max();
    CHECK(sao_ui_paint_ctx_push_clip(context, float_max * 0.75F, 0.0F,
                                     float_max * 0.5F, 1.0F) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_paint_ctx_stroke_line(context, -float_max * 0.5F, 8.0F,
                                         float_max * 0.5F, 8.0F, 1.0F,
                                         0xffffffffU) == SAO_STATUS_OK);
    CHECK(sao_ui_paint_ctx_stroke_line(context, 0.0F, 0.0F, 1.0F, 1.0F,
                                       float_max, 0xffffffffU) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    const int32_t polygon[] = {
        std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(),
    };
    REQUIRE(sao_ui_paint_ctx_fill_polygon(context, polygon, 4U, 0xff00ff00U) ==
            SAO_STATUS_OK);
    CHECK(sao_ui_paint_ctx_draw_scanlines(
              context, 0.0F, 0.0F, 16.0F, 16.0F,
              std::numeric_limits<float>::denorm_min(), 1.0F, 0xffffffffU) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_paint_ctx_draw_scanlines(context, float_max * 0.75F, 0.0F,
                                          float_max * 0.5F, 1.0F, 1.0F, 1.0F,
                                          0xffffffffU) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    const auto pixels = snapshot(raster);
    CHECK(pixels[8U * 16U + 8U].g == 255U);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
}


TEST_CASE("native_widget_paint_rejects_non_finite_coordinates", "[ui][raster][finite]") {
    sao_ui_widget_handle_t widget = nullptr;
    REQUIRE(sao_ui_widget_create(SAO_UI_WIDGET_CHECKBOX, nullptr, &widget) == SAO_STATUS_OK);
    SaoUiOffscreenRasterDesc desc{16, 16, 0};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    sao_ui_paint_ctx_handle_t context = nullptr;
    REQUIRE(sao_ui_offscreen_raster_create(&desc, &raster) == SAO_STATUS_OK);
    REQUIRE(sao_ui_paint_ctx_create_offscreen(raster, &context) == SAO_STATUS_OK);
    CHECK(sao_ui_widget_paint(widget, context, std::numeric_limits<float>::quiet_NaN(), 0, 8, 8) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_paint_ctx_fill_rect(context, std::numeric_limits<float>::infinity(), 0, 8, 8, 0xffffffffU) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    sao_ui_paint_ctx_destroy(context);
    sao_ui_offscreen_raster_destroy(raster);
    sao_ui_widget_destroy(widget);
}
