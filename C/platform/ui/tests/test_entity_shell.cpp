#include <catch2/catch_test_macros.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/entity_shell.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr int32_t kMenuPad = 40;
constexpr int32_t kMenuSlot = 70;
constexpr int32_t kMenuSlotCenter = kMenuPad + kMenuSlot / 2;

struct ActionLog {
    uint32_t calls = 0;
    SaoUiEntityAction last = SAO_UI_ENTITY_ACTION_OPEN_ABOUT;
    sao_ui_entity_shell_handle_t shell = nullptr;
    sao_status_t snapshot_status = SAO_STATUS_ERR_NOT_INITIALIZED;
    sao_status_t return_status = SAO_STATUS_OK;
    bool menu_visible_when_called = true;
    bool throw_exception = false;
    uint64_t frame_count_when_called = 0;
};

sao_status_t SAO_UI_CALL record_action(SaoUiEntityAction action, void* user_data) {
    auto* log = static_cast<ActionLog*>(user_data);
    ++log->calls;
    log->last = action;
    if (log->shell != nullptr) {
        SaoUiEntityShellSnapshot snapshot{};
        log->snapshot_status = sao_ui_entity_shell_get_snapshot(log->shell, &snapshot);
        log->menu_visible_when_called = snapshot.menu_visible;
        log->frame_count_when_called = snapshot.frame_count;
    }
    if (log->throw_exception)
        throw std::runtime_error("action callback");
    return log->return_status;
}

SaoUiEntityShellConfig headless_config(ActionLog* actions) {
    SaoUiEntityShellConfig config{};
    config.width = 420;
    config.height = 460;
    config.origin_x = 100;
    config.origin_y = 200;
    config.action_fn = &record_action;
    config.action_user_data = actions;
    return config;
}

std::vector<uint8_t> snapshot_pixels(sao_ui_entity_shell_handle_t shell,
                                     uint32_t* width_out = nullptr,
                                     uint32_t* height_out = nullptr) {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t bytes = 0;
    REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, nullptr, 0, &width, &height, &bytes) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(bytes);
    REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, pixels.data(), pixels.size(), &width, &height,
                                              &bytes) == SAO_STATUS_OK);
    if (width_out != nullptr)
        *width_out = width;
    if (height_out != nullptr)
        *height_out = height;
    return pixels;
}

sao_status_t send_left_click_status(sao_ui_entity_shell_handle_t shell, int32_t screen_x,
                                    int32_t screen_y) {
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, screen_x, screen_y, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, screen_x, screen_y, 0, 0) ==
            SAO_STATUS_OK);
    return sao_ui_entity_shell_handle_mouse(shell, kLeftButtonUp, screen_x, screen_y, 0, 0);
}

void send_left_click(sao_ui_entity_shell_handle_t shell, int32_t screen_x, int32_t screen_y) {
    REQUIRE(send_left_click_status(shell, screen_x, screen_y) == SAO_STATUS_OK);
}

bool SAO_UI_CALL shell_hit_test(int32_t x, int32_t y, void* user_data) {
    bool hit = false;
    return sao_ui_entity_shell_hit_test(static_cast<sao_ui_entity_shell_handle_t>(user_data), x, y,
                                        &hit) == SAO_STATUS_OK &&
           hit;
}

void SAO_UI_CALL shell_mouse(uint32_t message, int32_t x, int32_t y, int32_t button,
                             int32_t wheel_delta, void* user_data) {
    (void)sao_ui_entity_shell_handle_mouse(static_cast<sao_ui_entity_shell_handle_t>(user_data),
                                           message, x, y, button, wheel_delta);
}

} // namespace

TEST_CASE("Entity shell renders real BGRA and changes NerveGear state pixels",
          "[ui][entity_shell][raster]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const uint64_t initial_frame_count = state.frame_count;
    REQUIRE(sao_ui_entity_shell_tick(shell, 16) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.frame_count == initial_frame_count);
    uint32_t width = 0;
    uint32_t height = 0;
    const auto idle = snapshot_pixels(shell, &width, &height);
    REQUIRE(std::any_of(idle.begin() + 3, idle.end(), [](uint8_t value) { return value != 0; }));
    const size_t corner_alpha =
        (static_cast<size_t>(state.nervegear_y) * width + state.nervegear_x) * 4U + 3U;
    REQUIRE(corner_alpha < idle.size());
    CHECK(idle[corner_alpha] == 0);

    const int32_t center_x = state.origin_x + state.nervegear_x + SAO_UI_NERVEGEAR_SIZE / 2;
    const int32_t center_y = state.origin_y + state.nervegear_y + SAO_UI_NERVEGEAR_SIZE / 2;
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, center_x, center_y, -1, 0) ==
            SAO_STATUS_OK);
    const auto hover = snapshot_pixels(shell);
    CHECK(hover != idle);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.nervegear_state == SAO_UI_NG_STATE_HOVER);
    CHECK(state.frame_count > 1);

    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, center_x, center_y, 0, 0) ==
            SAO_STATUS_OK);
    const auto pressed = snapshot_pixels(shell);
    CHECK(pressed != hover);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.nervegear_state == SAO_UI_NG_STATE_PRESSED);

    sao_ui_entity_shell_take_offline(shell);
    sao_ui_entity_shell_destroy(shell);
    sao_ui_entity_shell_destroy(nullptr);
}

TEST_CASE("Entity shell click opens menu and About dismisses before action",
          "[ui][entity_shell][input]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    actions.shell = shell;
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    send_left_click(shell, state.origin_x + state.nervegear_x + 36,
                    state.origin_y + state.nervegear_y + 36);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    REQUIRE(state.menu_visible);
    const auto open_menu = snapshot_pixels(shell);

    const int32_t about_x = state.origin_x + state.menu_x + kMenuSlotCenter;
    const int32_t about_y =
        state.origin_y + state.menu_y + kMenuPad + 4 * kMenuSlot + kMenuSlot / 2;
    send_left_click(shell, about_x, about_y);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.menu_visible);
    CHECK(actions.calls == 1);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_OPEN_ABOUT);
    CHECK(actions.snapshot_status == SAO_STATUS_OK);
    CHECK_FALSE(actions.menu_visible_when_called);
    CHECK(actions.frame_count_when_called == state.frame_count);
    CHECK(snapshot_pixels(shell) != open_menu);

    REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.menu_visible);
    REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.menu_visible);

    REQUIRE(sao_ui_entity_shell_insert(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.overlay_visible);
    uint32_t hidden_width = 1;
    uint32_t hidden_height = 1;
    size_t hidden_bytes = 1;
    REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, nullptr, 0, &hidden_width, &hidden_height,
                                              &hidden_bytes) == SAO_STATUS_OK);
    CHECK(hidden_width == 0);
    CHECK(hidden_height == 0);
    CHECK(hidden_bytes == 0);
    bool hit = true;
    REQUIRE(sao_ui_entity_shell_hit_test(shell, state.origin_x + state.nervegear_x + 36,
                                         state.origin_y + state.nervegear_y + 36,
                                         &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);
    REQUIRE(sao_ui_entity_shell_insert(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.overlay_visible);

    sao_ui_entity_shell_take_offline(shell);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity shell records About callback failure in last_status",
          "[ui][entity_shell][input][status]") {
    ActionLog actions;
    actions.return_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    actions.shell = shell;
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    send_left_click(shell, state.origin_x + state.nervegear_x + 36,
                    state.origin_y + state.nervegear_y + 36);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    REQUIRE(state.menu_visible);

    const int32_t about_x = state.origin_x + state.menu_x + kMenuSlotCenter;
    const int32_t about_y =
        state.origin_y + state.menu_y + kMenuPad + 4 * kMenuSlot + kMenuSlot / 2;
    REQUIRE(send_left_click_status(shell, about_x, about_y) == actions.return_status);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.menu_visible);
    CHECK(state.last_status == actions.return_status);
    CHECK(actions.calls == 1);

    actions.throw_exception = true;
    send_left_click(shell, state.origin_x + state.nervegear_x + 36,
                    state.origin_y + state.nervegear_y + 36);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    REQUIRE(state.menu_visible);
    REQUIRE(send_left_click_status(shell, about_x, about_y) == SAO_STATUS_ERR_UNKNOWN);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.menu_visible);
    CHECK(state.last_status == SAO_STATUS_ERR_UNKNOWN);
    CHECK(actions.calls == 2);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity shell non-owner destroy detaches without releasing",
          "[ui][entity_shell][lifecycle]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    std::thread non_owner([shell] { sao_ui_entity_shell_destroy(shell); });
    non_owner.join();

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.online);
    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

#if defined(_WIN32)
TEST_CASE("compositor alpha-zero input layer leaves host click-through",
          "[ui][entity_shell][compositor][region]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 64;
    host_config.height = 64;
    host_config.title_utf16 = L"SAO alpha-zero input region test";
    sao_ui_overlay_host_handle_t host = nullptr;
    if (sao_ui_overlay_host_create(&host_config, &host) != SAO_STATUS_OK) {
        SKIP("overlay host unavailable in this desktop session");
    }

    sao_status_t cross_thread_status = SAO_STATUS_OK;
    std::thread cross_thread(
        [&] { cross_thread_status = sao_ui_overlay_host_set_input_passthrough(host, false); });
    cross_thread.join();
    CHECK(cross_thread_status == SAO_STATUS_ERR_ACCESS_DENIED);

    sao_ui_compositor_handle_t compositor = nullptr;
    const sao_status_t compositor_status = sao_ui_compositor_create(host, nullptr, &compositor);
    if (compositor_status != SAO_STATUS_OK) {
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D11/DirectComposition unavailable in this environment");
    }

    SaoLayerConfig config{};
    config.name_utf8 = "alpha-zero-input";
    config.x = 7;
    config.y = 9;
    config.width = 20;
    config.height = 18;
    config.click_through = false;
    config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &config, &layer) == SAO_STATUS_OK);
    const std::array<uint8_t, 4> pixel{};
    CHECK(sao_ui_layer_update_bgra(layer, pixel.data(), 0x40000000U, 1U, 0U) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    CHECK(sao_ui_layer_set_geometry(layer, INT32_MAX - 5, 9, 20, 18) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const SaoUiLayerInputRect local_rect{0, 0, 20, 18};
    REQUIRE(sao_ui_layer_set_input_rects(layer, &local_rect, 1) == SAO_STATUS_OK);
    CHECK(sao_ui_layer_set_geometry(layer, 7, 9, 10, 10) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_layer_set_input_rects(layer, nullptr, 0) == SAO_STATUS_OK);

    REQUIRE(sao_ui_layer_set_alpha(layer, 0.0F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_sync_host_rgn(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_sync_host_input_mode(compositor) == SAO_STATUS_OK);
    CHECK(sao_ui_overlay_host_input_passthrough(host));

    HRGN region = CreateRectRgn(0, 0, 0, 0);
    REQUIRE(region != nullptr);
    REQUIRE(GetWindowRgn(static_cast<HWND>(sao_ui_overlay_host_hwnd(host)), region) != ERROR);
    CHECK_FALSE(PtInRegion(region, 10, 12));
    DeleteObject(region);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("real overlay HWND routes mouse callback into Entity menu",
          "[ui][entity_shell][production]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 640;
    host_config.height = 480;
    host_config.origin_x = 100;
    host_config.origin_y = 80;
    host_config.title_utf16 = L"SAO Entity production-chain test";
    sao_ui_overlay_host_handle_t host = nullptr;
    const sao_status_t host_status = sao_ui_overlay_host_create(&host_config, &host);
    if (host_status != SAO_STATUS_OK) {
        SKIP("overlay host unavailable in this desktop session");
    }

    ActionLog actions;
    SaoUiEntityShellConfig shell_config{};
    shell_config.action_fn = &record_action;
    shell_config.action_user_data = &actions;
    sao_ui_entity_shell_handle_t shell = nullptr;
    const sao_status_t create_status = sao_ui_entity_shell_create(host, &shell_config, &shell);
    if (create_status != SAO_STATUS_OK) {
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D11/DirectComposition unavailable in this environment");
    }

    REQUIRE(sao_ui_overlay_host_set_hit_test(host, &shell_hit_test, shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_set_mouse(host, &shell_mouse, shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    HWND hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    const LPARAM point = MAKELPARAM(state.nervegear_x + 36, state.nervegear_y + 36);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.menu_visible);

    HRGN region = CreateRectRgn(0, 0, 0, 0);
    REQUIRE(region != nullptr);
    REQUIRE(GetWindowRgn(hwnd, region) != ERROR);
    CHECK(PtInRegion(region, state.menu_x + kMenuSlotCenter, state.menu_y + kMenuSlotCenter));
    CHECK_FALSE(PtInRegion(region, state.menu_x + 120, state.menu_y + kMenuSlotCenter));
    DeleteObject(region);

    REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.menu_visible);
    REQUIRE(sao_ui_entity_shell_tick(shell, 16) == SAO_STATUS_OK);
    region = CreateRectRgn(0, 0, 0, 0);
    REQUIRE(region != nullptr);
    REQUIRE(GetWindowRgn(hwnd, region) != ERROR);
    CHECK_FALSE(PtInRegion(region, state.menu_x + kMenuSlotCenter, state.menu_y + kMenuSlotCenter));
    CHECK(PtInRegion(region, state.nervegear_x + 36, state.nervegear_y + 36));
    DeleteObject(region);

    const int32_t old_menu_x = state.menu_x;
    const int32_t old_menu_y = state.menu_y;
    REQUIRE(SetWindowPos(hwnd, nullptr, 180, 120, 800, 600, SWP_NOACTIVATE | SWP_NOZORDER));
    REQUIRE(sao_ui_entity_shell_tick(shell, 16) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.origin_x == 180);
    CHECK(state.origin_y == 120);
    CHECK(state.width == 800);
    CHECK(state.height == 600);
    CHECK(state.nervegear_x == 708);
    CHECK(state.nervegear_y == 508);

    REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 16) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    region = CreateRectRgn(0, 0, 0, 0);
    REQUIRE(region != nullptr);
    REQUIRE(GetWindowRgn(hwnd, region) != ERROR);
    CHECK(PtInRegion(region, state.menu_x + kMenuSlotCenter, state.menu_y + kMenuSlotCenter));
    CHECK_FALSE(PtInRegion(region, old_menu_x + kMenuSlotCenter, old_menu_y + kMenuSlotCenter));
    DeleteObject(region);

    REQUIRE(sao_ui_overlay_host_set_mouse(host, nullptr, nullptr) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_set_hit_test(host, nullptr, nullptr) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}
#endif
