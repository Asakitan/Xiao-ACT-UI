// Wave 15 menu + dialog parity contract. This file deliberately has no CMake
// registration; the integration owner chooses its final target.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dialog.h"
#include "sao/ui/menu.h"
#include "sao/ui/overlay_host.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#ifndef SAO_UI_TEST_FIXTURE_DIR
#  define SAO_UI_TEST_FIXTURE_DIR ""
#endif

extern "C" {
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_compute_button_layout(
    sao_ui_menu_handle_t handle,
    int32_t button_index,
    int32_t* out_x,
    int32_t* out_y,
    int32_t* out_w,
    int32_t* out_h);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_tick(
    sao_ui_menu_handle_t handle,
    int32_t dt_ms);
}

namespace {

constexpr int32_t kFixtureWidth = 442;
constexpr int32_t kFixtureHeight = 290;

std::vector<uint8_t> read_fixture(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(SAO_UI_TEST_FIXTURE_DIR) / name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

SaoCompositorConfig compositor_config() {
    SaoCompositorConfig config{};
    config.target_hz = 60;
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    return config;
}

std::vector<uint8_t> snapshot(sao_ui_compositor_handle_t compositor,
                              uint32_t* width,
                              uint32_t* height) {
    size_t required = 0;
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, nullptr, 0, width, height, &required) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(required);
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, pixels.data(), pixels.size(), width, height,
                &required) == SAO_STATUS_OK);
    return pixels;
}

sao_ui_menu_handle_t make_menu() {
    sao_ui_menu_handle_t menu = nullptr;
    REQUIRE(sao_ui_menu_create(nullptr, nullptr, SAO_UI_MENU_MODE_RING, &menu) ==
            SAO_STATUS_OK);
    const std::array<SaoUiMenuItem, 3> items = {{
        {"Status", "S", 10, true, {false, false, false}},
        {"Party", "P", 11, true, {false, false, false}},
        {"System", "*", 12, true, {false, false, false}},
    }};
    SaoUiMenuLayout layout{};
    layout.center_x = 300;
    layout.center_y = 240;
    layout.inner_radius = 60;
    layout.outer_radius = 160;
    layout.child_ring_radius = 240;
    layout.button_size = 54;
    layout.button_max_size = 70;
    layout.slot_size = 70;
    layout.max_visible = 9;
    REQUIRE(sao_ui_menu_set_layout(menu, &layout) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_set_items(menu, items.data(), items.size()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_show(menu, layout.center_x, layout.center_y) == SAO_STATUS_OK);
    REQUIRE(sao_ui_menu_tick(menu, 450) == SAO_STATUS_OK);
    return menu;
}

struct DialogResult {
    int calls = 0;
    SaoUiDialogButton pressed = SAO_UI_DIALOG_BTN_CUSTOM;
};

void SAO_UI_CALL record_dialog_result(SaoUiDialogButton pressed,
                                      const char*,
                                      size_t,
                                      void* user_data) {
    auto* result = static_cast<DialogResult*>(user_data);
    ++result->calls;
    result->pressed = pressed;
}

sao_ui_dialog_handle_t make_dialog(DialogResult* result) {
    sao_ui_dialog_handle_t dialog = nullptr;
    REQUIRE(sao_ui_dialog_create(nullptr, nullptr, &dialog) == SAO_STATUS_OK);
    SaoUiDialogSpec spec{};
    spec.kind = SAO_UI_DIALOG_ASK;
    spec.title_utf8 = "Confirm";
    spec.message_utf8 = "Use the Python-authority timing contract.";
    spec.dismiss_on_esc = true;
    REQUIRE(sao_ui_dialog_show(dialog, &spec, &record_dialog_result, result) ==
            SAO_STATUS_OK);
    return dialog;
}

}  // namespace

TEST_CASE("wave15 menu fixture crosses the production compositor exactly",
          "[ui][wave15][parity][menu][pixel]") {
    const std::vector<uint8_t> fixture = read_fixture("wave15_menu_dialog.bgra");
    REQUIRE(fixture.size() == static_cast<size_t>(kFixtureWidth) * kFixtureHeight * 4u);

    sao_ui_compositor_handle_t compositor = nullptr;
    const SaoCompositorConfig config = compositor_config();
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig backdrop_config{};
    backdrop_config.name_utf8 = "wave15_menu_backdrop";
    backdrop_config.width = 1;
    backdrop_config.height = 1;
    backdrop_config.z_order = 1999;
    backdrop_config.click_through = true;
    sao_ui_layer_handle_t backdrop = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &backdrop_config, &backdrop) == SAO_STATUS_OK);
    const std::array<uint8_t, 4> backdrop_pixel = {0x10, 0x08, 0x04, 0x20};
    REQUIRE(sao_ui_layer_update_bgra(backdrop, backdrop_pixel.data(), 1, 1, 4) ==
            SAO_STATUS_OK);

    SaoLayerConfig layer_config{};
    layer_config.name_utf8 = "wave15_menu_authority";
    layer_config.width = kFixtureWidth;
    layer_config.height = kFixtureHeight;
    layer_config.z_order = 2000;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    layer_config.bgra_swizzle = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_update_bgra(
                layer, fixture.data(), kFixtureWidth, kFixtureHeight,
                kFixtureWidth * 4) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_position(layer, 7, 11) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_alpha(layer, 1.0f) == SAO_STATUS_OK);

        SaoLayerConfig alpha_config{};
        alpha_config.name_utf8 = "wave15_alpha_foreground";
        alpha_config.width = 1;
        alpha_config.height = 1;
        alpha_config.z_order = 2001;
        alpha_config.click_through = true;
        sao_ui_layer_handle_t alpha_foreground = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &alpha_config, &alpha_foreground) == SAO_STATUS_OK);
        const std::array<uint8_t, 4> alpha_pixel = {0x80, 0x40, 0x20, 0x80};
        REQUIRE(sao_ui_layer_update_bgra(alpha_foreground, alpha_pixel.data(), 1, 1, 4) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_layer_set_alpha(alpha_foreground, 0.5f) == SAO_STATUS_OK);

        std::array<sao_ui_layer_handle_t, 3> layers{};
    size_t layer_count = 0;
    REQUIRE(sao_ui_compositor_list_layers(
                compositor, layers.data(), layers.size(), &layer_count) == SAO_STATUS_OK);
    REQUIRE(layer_count == 3);
    REQUIRE(layers[0] == backdrop);
    REQUIRE(layers[1] == layer);
    REQUIRE(layers[2] == alpha_foreground);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> composed = snapshot(compositor, &width, &height);
    REQUIRE(width == kFixtureWidth + 7u);
    REQUIRE(height == kFixtureHeight + 11u);
    for (int32_t row = 0; row < kFixtureHeight; ++row) {
        const auto source = fixture.begin() + static_cast<size_t>(row) * kFixtureWidth * 4u;
        const auto target = composed.begin() +
            (static_cast<size_t>(row + 11) * width + 7u) * 4u;
        REQUIRE(std::equal(source, source + static_cast<size_t>(kFixtureWidth) * 4u, target));
    }

    REQUIRE(composed[0] == 0x4C);
    REQUIRE(composed[1] == 0x26);
    REQUIRE(composed[2] == 0x13);
    REQUIRE(composed[3] == 0x58);

    sao_ui_layer_destroy(alpha_foreground);
    sao_ui_layer_destroy(layer);
    sao_ui_layer_destroy(backdrop);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("wave15 menu geometry and hit regions follow the authoritative layout",
          "[ui][wave15][parity][menu][geometry]") {
    sao_ui_menu_handle_t menu = make_menu();
    for (int32_t index = 0; index < 3; ++index) {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        REQUIRE(sao_ui_menu_compute_button_layout(menu, index, &x, &y, &width, &height) ==
                SAO_STATUS_OK);
        REQUIRE(width == 54);
        REQUIRE(height == 54);
        int32_t hit_menu = -1;
        int32_t hit_child = -1;
        REQUIRE(sao_ui_menu_hit_test(menu, x + width / 2, y + height / 2,
                                     &hit_menu, &hit_child) == SAO_STATUS_OK);
        REQUIRE(hit_menu == index);
        REQUIRE(hit_child == -1);
    }
    int32_t hit_menu = 0;
    REQUIRE(sao_ui_menu_hit_test(menu, 300, 240, &hit_menu, nullptr) == SAO_STATUS_OK);
    REQUIRE(hit_menu == -1);
    sao_ui_menu_destroy(menu);
}

TEST_CASE("wave15 dialog authority covers geometry timings z order and keys",
          "[ui][wave15][parity][dialog]") {
    DialogResult result{};
    sao_ui_dialog_handle_t dialog = make_dialog(&result);

    SaoUiDialogLayoutSnapshot layout{};
    REQUIRE(sao_ui_dialog_get_layout_snapshot(dialog, &layout) == SAO_STATUS_OK);
    REQUIRE(layout.width == 135);
    REQUIRE(layout.height == 240);
    REQUIRE(layout.initial_width == 135);
    REQUIRE(layout.header_height == 68);
    REQUIRE(layout.content_height == 87);
    REQUIRE(layout.footer_height == 83);
    REQUIRE(layout.separator_height == 1);
    REQUIRE(layout.expand_ms == 500);
    REQUIRE(layout.title_reveal_delay_ms == 100);
    REQUIRE(layout.title_reveal_ms == 400);
    REQUIRE(layout.message_reveal_delay_ms == 600);
    REQUIRE(layout.message_reveal_ms == 350);
    REQUIRE(layout.shrink_ms == 350);
    REQUIRE(layout.mirror_z == 2000);
    REQUIRE(layout.alpha == 1.0f);

    REQUIRE(sao_ui_dialog_tick(dialog, 500) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_layout_snapshot(dialog, &layout) == SAO_STATUS_OK);
    REQUIRE(layout.width == 375);
    REQUIRE(sao_ui_dialog_dispatch_key(dialog, 0x09u, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_dispatch_key(dialog, 0x0Du, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_tick(dialog, 175) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_get_layout_snapshot(dialog, &layout) == SAO_STATUS_OK);
    REQUIRE(layout.width < 375);
    REQUIRE(layout.alpha < 1.0f);
    REQUIRE(sao_ui_dialog_tick(dialog, 175) == SAO_STATUS_OK);
    REQUIRE(result.calls == 1);
    REQUIRE(result.pressed == SAO_UI_DIALOG_BTN_CANCEL);
    sao_ui_dialog_destroy(dialog);

    DialogResult escape_result{};
    dialog = make_dialog(&escape_result);
    REQUIRE(sao_ui_dialog_dispatch_key(dialog, 0x1Bu, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dialog_tick(dialog, 350) == SAO_STATUS_OK);
    REQUIRE(escape_result.calls == 1);
    REQUIRE(escape_result.pressed == SAO_UI_DIALOG_BTN_DISMISS);
    sao_ui_dialog_destroy(dialog);
}

#if defined(_WIN32)
TEST_CASE("wave15 compositor synchronizes the production host hit region",
          "[ui][wave15][parity][compositor][hit-region]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 64;
    host_config.height = 64;
    host_config.title_utf16 = L"wave15 menu dialog parity";
    sao_ui_overlay_host_handle_t host = nullptr;
    if (sao_ui_overlay_host_create(&host_config, &host) != SAO_STATUS_OK) {
        SKIP("a desktop window station is unavailable in this environment");
    }

    sao_ui_compositor_handle_t compositor = nullptr;
    const SaoCompositorConfig config = compositor_config();
    REQUIRE(sao_ui_compositor_create(host, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config{};
    layer_config.name_utf8 = "wave15_hit_region";
    layer_config.x = 9;
    layer_config.y = 13;
    layer_config.width = 17;
    layer_config.height = 19;
    layer_config.z_order = 2000;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_sync_host_rgn(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_sync_host_input_mode(compositor) == SAO_STATUS_OK);
    REQUIRE_FALSE(sao_ui_overlay_host_input_passthrough(host));

    HRGN region = ::CreateRectRgn(0, 0, 0, 0);
    REQUIRE(region != nullptr);
    REQUIRE(::GetWindowRgn(static_cast<HWND>(sao_ui_overlay_host_hwnd(host)), region) != ERROR);
    REQUIRE(::PtInRegion(region, 9, 13) == TRUE);
    REQUIRE(::PtInRegion(region, 25, 31) == TRUE);
    REQUIRE(::PtInRegion(region, 26, 32) == FALSE);
    ::DeleteObject(region);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}
#endif