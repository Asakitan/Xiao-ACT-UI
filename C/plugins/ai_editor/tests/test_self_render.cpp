// Catch2 tests for GdiFramebuffer (self_render.h).
//
// Focuses on framebuffer geometry, clear/fill_rect pixel outcomes, and
// premultiply_alpha correctness.  Does NOT cover draw_text or rounded_rect
// where GDI-side rasterisation makes exact pixel asserts brittle across
// Windows versions; those are exercised by higher-level integration tests
// once the migration wire-up lands.

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "self_render.h"

TEST_CASE("GdiFramebuffer init creates DIB section",
          "[ai_editor][self_render]") {
    sao::ai_editor::GdiFramebuffer fb;
    REQUIRE(fb.init(64, 48));
    REQUIRE(fb.is_initialized());
    REQUIRE(fb.width() == 64);
    REQUIRE(fb.height() == 48);
    REQUIRE(fb.stride() == 256);
    REQUIRE(fb.byte_size() == 64u * 48u * 4u);
    REQUIRE(fb.pixels() != nullptr);
    REQUIRE(fb.dc() != nullptr);
}

TEST_CASE("GdiFramebuffer init rejects invalid dimensions",
          "[ai_editor][self_render]") {
    sao::ai_editor::GdiFramebuffer fb;
    REQUIRE_FALSE(fb.init(0, 48));
    REQUIRE_FALSE(fb.init(64, 0));
    REQUIRE_FALSE(fb.init(-1, 48));
    REQUIRE_FALSE(fb.is_initialized());
}

TEST_CASE("GdiFramebuffer clear fills entire buffer",
          "[ai_editor][self_render]") {
    sao::ai_editor::GdiFramebuffer fb;
    REQUIRE(fb.init(16, 8));
    const uint32_t colour = sao::ai_editor::bgra_rgba(0x40, 0x80, 0xC0, 0xFF);
    fb.clear(colour);
    const uint32_t* pixels =
        reinterpret_cast<const uint32_t*>(fb.pixels());
    for (int i = 0; i < 16 * 8; ++i) {
        REQUIRE(pixels[i] == colour);
    }
}

TEST_CASE("GdiFramebuffer fill_rect paints only the requested area",
          "[ai_editor][self_render]") {
    sao::ai_editor::GdiFramebuffer fb;
    REQUIRE(fb.init(16, 8));
    const uint32_t bg = sao::ai_editor::bgra_rgba(0x00, 0x00, 0x00, 0xFF);
    const uint32_t fg = sao::ai_editor::bgra_rgba(0xFF, 0x00, 0x00, 0xFF);
    fb.clear(bg);
    fb.fill_rect(4, 2, 8, 4, fg);
    ::GdiFlush();
    const uint32_t* pixels =
        reinterpret_cast<const uint32_t*>(fb.pixels());
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uint32_t got = pixels[y * 16 + x];
            const bool inside =
                (x >= 4 && x < 12 && y >= 2 && y < 6);
            const uint32_t want = inside ? fg : bg;
            REQUIRE(got == want);
        }
    }
}

TEST_CASE("GdiFramebuffer premultiply_alpha scales channels",
          "[ai_editor][self_render]") {
    sao::ai_editor::GdiFramebuffer fb;
    REQUIRE(fb.init(2, 1));
    uint8_t* p = fb.pixels_mutable();
    // Pixel 0: B=200, G=100, R=50, A=128 (half-transparent).
    p[0] = 200;
    p[1] = 100;
    p[2] = 50;
    p[3] = 128;
    // Pixel 1: fully opaque, must not change.
    p[4] = 200;
    p[5] = 100;
    p[6] = 50;
    p[7] = 255;
    fb.premultiply_alpha();
    // (200 * 128 + 127) / 255 = 100
    REQUIRE(p[0] == 100);
    REQUIRE(p[1] == 50);
    REQUIRE(p[2] == 25);
    REQUIRE(p[3] == 128);
    REQUIRE(p[4] == 200);
    REQUIRE(p[5] == 100);
    REQUIRE(p[6] == 50);
    REQUIRE(p[7] == 255);
}

TEST_CASE("GdiFramebuffer premultiply_alpha zeroes fully-transparent pixels",
          "[ai_editor][self_render]") {
    sao::ai_editor::GdiFramebuffer fb;
    REQUIRE(fb.init(1, 1));
    uint8_t* p = fb.pixels_mutable();
    p[0] = 200;
    p[1] = 100;
    p[2] = 50;
    p[3] = 0;
    fb.premultiply_alpha();
    REQUIRE(p[0] == 0);
    REQUIRE(p[1] == 0);
    REQUIRE(p[2] == 0);
    REQUIRE(p[3] == 0);
}

TEST_CASE("GdiFramebuffer create_font returns a real HFONT",
          "[ai_editor][self_render]") {
    HFONT font = sao::ai_editor::GdiFramebuffer::create_font(
        L"Segoe UI", 14, false);
    REQUIRE(font != nullptr);
    ::DeleteObject(font);
}
