#include <catch2/catch_test_macros.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/entity_shell.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
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
constexpr uint32_t kMouseWheel = 0x020A;
constexpr int32_t kMenuPad = 40;
constexpr int32_t kMenuSlot = 70;
constexpr int32_t kMenuSlotCenter = kMenuPad + kMenuSlot / 2;
constexpr int32_t kChildRowX = 162;
constexpr int32_t kChildRowY = 40;
constexpr int32_t kChildRowHeight = 44;
constexpr int32_t kChildRowStride = 47;
constexpr int32_t kChildRowWidth = 240;
constexpr int32_t kChildPhysicalCapacity = 8;
constexpr int32_t kChildActionBase = 1000;

struct ActionLog {
    uint32_t calls = 0;
    SaoUiEntityAction last = SAO_UI_ENTITY_ACTION_OPEN_ABOUT;
    sao_ui_entity_shell_handle_t shell = nullptr;
    sao_status_t snapshot_status = SAO_STATUS_ERR_NOT_INITIALIZED;
    sao_status_t return_status = SAO_STATUS_OK;
    bool menu_visible_when_called = true;
    bool throw_exception = false;
    bool destroy_self = false;
    sao_status_t self_destroy_status = SAO_STATUS_OK;
    uint64_t frame_count_when_called = 0;
};

struct ChildItemStorage {
    std::vector<std::string> names;
    std::vector<SaoUiMenuItem> items;
};

ChildItemStorage make_child_items(size_t count, int32_t action_base = kChildActionBase,
                                  int32_t disabled_index = -1) {
    ChildItemStorage storage;
    storage.names.reserve(count);
    storage.items.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const char marker = static_cast<char>('A' + index % 26U);
        storage.names.push_back(std::string(1, marker) + " viewport row " +
                                std::to_string(index));
    }
    for (size_t index = 0; index < count; ++index) {
        SaoUiMenuItem item{};
        item.name_utf8 = storage.names[index].c_str();
        item.icon_utf8 = ">";
        item.action_id = action_base + static_cast<int32_t>(index);
        item.can_activate = static_cast<int32_t>(index) != disabled_index;
        storage.items.push_back(item);
    }
    return storage;
}

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
    if (log->destroy_self && log->shell != nullptr)
        log->self_destroy_status = sao_ui_entity_shell_try_destroy(log->shell);
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

size_t layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    return sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK
               ? count
               : (std::numeric_limits<size_t>::max)();
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

void set_entity_children(sao_ui_entity_shell_handle_t shell, const char* parent_name,
                         const ChildItemStorage& storage) {
    const SaoUiMenuItem* items = storage.items.empty() ? nullptr : storage.items.data();
    REQUIRE(sao_ui_entity_shell_set_children(shell, parent_name, items, storage.items.size()) ==
            SAO_STATUS_OK);
}

void select_root(sao_ui_entity_shell_handle_t shell, int32_t root_index) {
    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    if (!state.menu_visible)
        REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    const int32_t root_x = state.origin_x + state.menu_x + kMenuSlotCenter;
    const int32_t root_y =
        state.origin_y + state.menu_y + kMenuPad + root_index * kMenuSlot + kMenuSlot / 2;
    send_left_click(shell, root_x, root_y);
    REQUIRE(sao_ui_entity_shell_tick(shell, 1000) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 200) == SAO_STATUS_OK);
}

std::array<int32_t, 2> child_point(const SaoUiEntityShellSnapshot& state, int32_t slot,
                                   int32_t local_x = kChildRowX + 18,
                                   int32_t row_offset_y = kChildRowHeight / 2) {
    return {state.origin_x + state.menu_x + local_x,
            state.origin_y + state.menu_y + kChildRowY + slot * kChildRowStride +
                row_offset_y};
}

void send_wheel(sao_ui_entity_shell_handle_t shell, const std::array<int32_t, 2>& point,
                int32_t delta) {
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseWheel, point[0], point[1], -1, delta) ==
            SAO_STATUS_OK);
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

TEST_CASE("Entity shell dismisses menu before AI Editor action",
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

    const int32_t tools_x = state.origin_x + state.menu_x + kMenuSlotCenter;
    const int32_t tools_y = state.origin_y + state.menu_y + kMenuPad +
                            kMenuSlot + kMenuSlot / 2;
    send_left_click(shell, tools_x, tools_y);
    REQUIRE(sao_ui_entity_shell_tick(shell, 600) == SAO_STATUS_OK);

    const int32_t ai_editor_x = state.origin_x + state.menu_x + 180;
    const int32_t ai_editor_y = state.origin_y + state.menu_y + kMenuPad + 22;
    send_left_click(shell, ai_editor_x, ai_editor_y);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.menu_visible);
    CHECK(actions.calls == 1);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR);
    CHECK(actions.snapshot_status == SAO_STATUS_OK);
    CHECK_FALSE(actions.menu_visible_when_called);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity shell initial actions fail closed before launcher publication",
          "[ui][entity_shell][authority][initial][focused]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    const auto click_child = [&](int32_t row) {
        SaoUiEntityShellSnapshot state{};
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        const auto point = child_point(state, row);
        send_left_click(shell, point[0], point[1]);
    };

    select_root(shell, 0);
    click_child(0);
    click_child(4);
    click_child(5);
    CHECK(actions.calls == 0);

    click_child(1);
    CHECK(actions.calls == 1);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR);
    click_child(3);
    CHECK(actions.calls == 2);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE);
    click_child(7);
    CHECK(actions.calls == 3);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_SAVE_SETTINGS);

    select_root(shell, 1);
    click_child(1);
    click_child(2);
    CHECK(actions.calls == 3);
    click_child(0);
    CHECK(actions.calls == 4);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR);

    select_root(shell, 2);
    click_child(0);
    CHECK(actions.calls == 4);
    click_child(1);
    CHECK(actions.calls == 5);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS);

    select_root(shell, 3);
    click_child(0);
    CHECK(actions.calls == 6);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT);
    click_child(1);
    CHECK(actions.calls == 7);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_SET_ALL_DARK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const int32_t about_x = state.origin_x + state.menu_x + kMenuSlotCenter;
    const int32_t about_y =
        state.origin_y + state.menu_y + kMenuPad + 4 * kMenuSlot + kMenuSlot / 2;
    send_left_click(shell, about_x, about_y);
    CHECK(actions.calls == 8);
    CHECK(actions.last == SAO_UI_ENTITY_ACTION_OPEN_ABOUT);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity child viewport derives eight physical rows from fixed geometry",
          "[ui][entity_shell][viewport][geometry]") {
    for (const size_t row_count : {0U, 8U, 9U, 16U}) {
        DYNAMIC_SECTION(row_count << " logical rows") {
            ActionLog actions;
            const auto config = headless_config(&actions);
            sao_ui_entity_shell_handle_t shell = nullptr;
            REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
            REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
            const auto children = make_child_items(row_count);
            set_entity_children(shell, "Control", children);
            select_root(shell, 0);

            SaoUiEntityShellSnapshot state{};
            REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
            bool hit = false;
            const auto slot0 = child_point(state, 0, kChildRowX, 0);
            REQUIRE(sao_ui_entity_shell_hit_test(shell, slot0[0], slot0[1], &hit) == SAO_STATUS_OK);
            CHECK(hit == (row_count > 0));

            const auto right_edge = child_point(state, 0, kChildRowX + kChildRowWidth, 10);
            REQUIRE(sao_ui_entity_shell_hit_test(shell, right_edge[0], right_edge[1], &hit) ==
                    SAO_STATUS_OK);
            CHECK_FALSE(hit);
            const auto row_bottom = child_point(state, 0, kChildRowX + 10, kChildRowHeight);
            REQUIRE(sao_ui_entity_shell_hit_test(shell, row_bottom[0], row_bottom[1], &hit) ==
                    SAO_STATUS_OK);
            CHECK_FALSE(hit);
            const auto next_row = child_point(state, 1, kChildRowX + 10, 0);
            REQUIRE(sao_ui_entity_shell_hit_test(shell, next_row[0], next_row[1], &hit) ==
                    SAO_STATUS_OK);
            CHECK(hit == (row_count > 1));

            const auto physical_slot8 =
                child_point(state, kChildPhysicalCapacity, kChildRowX + 10, 0);
            REQUIRE(sao_ui_entity_shell_hit_test(shell, physical_slot8[0], physical_slot8[1],
                                                 &hit) == SAO_STATUS_OK);
            CHECK_FALSE(hit);
            REQUIRE(sao_ui_entity_shell_hit_test(
                        shell, std::numeric_limits<int32_t>::min(),
                        std::numeric_limits<int32_t>::max(), &hit) == SAO_STATUS_OK);
            CHECK_FALSE(hit);

            REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
            sao_ui_entity_shell_destroy(shell);
        }
    }
}

TEST_CASE("Entity child viewport wheel projects physical slots to logical rows",
          "[ui][entity_shell][viewport][wheel][activation]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    actions.shell = shell;
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    const auto children = make_child_items(16);
    set_entity_children(shell, "Control", children);
    select_root(shell, 0);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const auto slot0 = child_point(state, 0);
    const auto slot7 = child_point(state, 7);
    const auto top_pixels = snapshot_pixels(shell);
    const uint64_t top_frame = state.frame_count;

    send_wheel(shell, slot0, 120);
    send_wheel(shell, slot0, 240);
    send_wheel(shell, slot0, 0);
    send_wheel(shell, slot0, 60);
    const auto outside = child_point(state, 0, kChildRowX - 32);
    send_wheel(shell, outside, -120);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.frame_count == top_frame);
    CHECK(snapshot_pixels(shell) == top_pixels);

    send_wheel(shell, slot0, -120);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.frame_count > top_frame);
    CHECK(snapshot_pixels(shell) != top_pixels);
    send_left_click(shell, slot0[0], slot0[1]);
    CHECK(actions.calls == 1);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase + 1);

    send_wheel(shell, slot0, -120 * 20);
    send_left_click(shell, slot7[0], slot7[1]);
    CHECK(actions.calls == 2);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase + 15);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const uint64_t bottom_frame = state.frame_count;
    send_wheel(shell, slot0, -120);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.frame_count == bottom_frame);

    send_wheel(shell, slot0, 120 * 20);
    send_left_click(shell, slot0[0], slot0[1]);
    CHECK(actions.calls == 3);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity child viewport ignores short menus and cancels stale pressed rows",
          "[ui][entity_shell][viewport][wheel][pressed][disabled]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    const auto short_children = make_child_items(8);
    set_entity_children(shell, "Control", short_children);
    select_root(shell, 0);
    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const auto slot0 = child_point(state, 0);
    const uint64_t short_frame = state.frame_count;
    send_wheel(shell, slot0, -120);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.frame_count == short_frame);

    const auto long_children = make_child_items(16, kChildActionBase, 1);
    set_entity_children(shell, "Control", long_children);
    REQUIRE(sao_ui_entity_shell_tick(shell, 1000) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 200) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, slot0[0], slot0[1], -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, slot0[0], slot0[1], 0, 0) ==
            SAO_STATUS_OK);
    send_wheel(shell, slot0, -120);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonUp, slot0[0], slot0[1], 0, 0) ==
            SAO_STATUS_OK);
    CHECK(actions.calls == 0);

        send_wheel(shell, slot0, 120);
    const auto about = std::array<int32_t, 2>{
        state.origin_x + state.menu_x + kMenuSlotCenter,
        state.origin_y + state.menu_y + kMenuPad + 4 * kMenuSlot + kMenuSlot / 2};
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, about[0], about[1], 0, 0) ==
            SAO_STATUS_OK);
    send_wheel(shell, slot0, 120);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonUp, about[0], about[1], 0, 0) ==
            SAO_STATUS_OK);
    CHECK(actions.calls == 0);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.menu_visible);

    send_wheel(shell, slot0, -120);
    send_left_click(shell, slot0[0], slot0[1]);
    CHECK(actions.calls == 0);
    const auto slot1 = child_point(state, 1);
    send_left_click(shell, slot1[0], slot1[1]);
    CHECK(actions.calls == 1);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase + 2);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

TEST_CASE("Entity child setter is owner-thread atomic and clamps refreshed viewport",
          "[ui][entity_shell][viewport][setter]") {
    CHECK(sao_ui_entity_shell_set_children(nullptr, "Control", nullptr, 0) ==
          SAO_STATUS_ERR_HANDLE_INVALID);

    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    const auto original = make_child_items(16);
    set_entity_children(shell, "Control", original);
    select_root(shell, 0);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const auto slot0 = child_point(state, 0);
    const auto slot7 = child_point(state, 7);
    send_wheel(shell, slot0, -120);
    REQUIRE(sao_ui_entity_shell_set_children(shell, nullptr, original.items.data(),
                                             original.items.size()) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_entity_shell_set_children(shell, "Control", nullptr, 1) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_entity_shell_set_children(shell, "Missing", original.items.data(),
                                             original.items.size()) == SAO_STATUS_ERR_NOT_FOUND);

    sao_status_t non_owner_status = SAO_STATUS_OK;
    const auto* original_items = original.items.data();
    const size_t original_item_count = original.items.size();
    std::thread non_owner([&] {
        non_owner_status =
            sao_ui_entity_shell_set_children(shell, "Control", original_items, original_item_count);
    });
    non_owner.join();
    CHECK(non_owner_status == SAO_STATUS_ERR_ACCESS_DENIED);
    send_left_click(shell, slot0[0], slot0[1]);
    CHECK(actions.calls == 1);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase + 1);

    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const uint64_t unchanged_frame = state.frame_count;
    const auto unchanged_pixels = snapshot_pixels(shell);
    set_entity_children(shell, "Control", original);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.frame_count == unchanged_frame);
    CHECK(snapshot_pixels(shell) == unchanged_pixels);

    send_wheel(shell, slot0, -120 * 20);
    const int32_t replacement_base = 2000;
    const auto replacement = make_child_items(9, replacement_base);
    set_entity_children(shell, "Control", replacement);
    REQUIRE(sao_ui_entity_shell_tick(shell, 1000) == SAO_STATUS_OK);
    send_left_click(shell, slot0[0], slot0[1]);
    CHECK(actions.calls == 2);
    CHECK(static_cast<int32_t>(actions.last) == replacement_base + 1);
    send_left_click(shell, slot7[0], slot7[1]);
    CHECK(actions.calls == 3);
    CHECK(static_cast<int32_t>(actions.last) == replacement_base + 8);

    const auto tools = make_child_items(16, 3000);
    set_entity_children(shell, "Tools", tools);
    select_root(shell, 1);
    send_left_click(shell, slot0[0], slot0[1]);
    CHECK(actions.calls == 4);
    CHECK(static_cast<int32_t>(actions.last) == 3000);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
}

    TEST_CASE("Entity shell NerveGear mode hides only the trigger",
          "[ui][entity_shell][nervgear_mode]") {
        CHECK(sao_ui_entity_shell_set_nervgear_mode(nullptr, false) ==
          SAO_STATUS_ERR_HANDLE_INVALID);

        ActionLog actions;
        const auto config = headless_config(&actions);
        sao_ui_entity_shell_handle_t shell = nullptr;
        REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

        SaoUiEntityShellSnapshot state{};
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        const int32_t trigger_x = state.origin_x + state.nervegear_x + 36;
        const int32_t trigger_y = state.origin_y + state.nervegear_y + 36;
        bool hit = false;
        REQUIRE(sao_ui_entity_shell_hit_test(shell, trigger_x, trigger_y, &hit) ==
            SAO_STATUS_OK);
        REQUIRE(hit);

        REQUIRE(sao_ui_entity_shell_set_nervgear_mode(shell, false) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_hit_test(shell, trigger_x, trigger_y, &hit) ==
            SAO_STATUS_OK);
        CHECK_FALSE(hit);
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        CHECK(state.online);
        CHECK(state.overlay_visible);
        CHECK_FALSE(state.menu_visible);

        REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        CHECK(state.menu_visible);

        const int32_t control_x = state.origin_x + state.menu_x + kMenuSlotCenter;
        const int32_t control_y = state.origin_y + state.menu_y + kMenuSlotCenter;
        send_left_click(shell, control_x, control_y);
        REQUIRE(sao_ui_entity_shell_tick(shell, 600) == SAO_STATUS_OK);
        const auto control_off = snapshot_pixels(shell);

        REQUIRE(sao_ui_entity_shell_set_nervgear_mode(shell, true) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_tick(shell, 600) == SAO_STATUS_OK);
        const auto control_on = snapshot_pixels(shell);
        CHECK(control_on != control_off);
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        CHECK(state.menu_visible);
        REQUIRE(sao_ui_entity_shell_hit_test(shell, trigger_x, trigger_y, &hit) ==
            SAO_STATUS_OK);
        CHECK(hit);

        REQUIRE(sao_ui_entity_shell_set_nervgear_mode(shell, false) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        const uint64_t settled_frame_count = state.frame_count;
        REQUIRE(sao_ui_entity_shell_set_nervgear_mode(shell, false) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
        CHECK(state.frame_count == settled_frame_count);
        CHECK(state.menu_visible);

        sao_status_t non_owner_status = SAO_STATUS_OK;
        std::thread non_owner([&] {
        non_owner_status = sao_ui_entity_shell_set_nervgear_mode(shell, true);
        });
        non_owner.join();
        CHECK(non_owner_status == SAO_STATUS_ERR_ACCESS_DENIED);

        REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
        sao_ui_entity_shell_destroy(shell);
    }

TEST_CASE("Entity shell non-owner destroy is ignored without releasing",
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

TEST_CASE("Entity owned shell try destroy preserves handle after non-owner failure",
          "[ui][entity_shell][lifecycle][retry]") {
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    sao_status_t non_owner_status = SAO_STATUS_OK;
    std::thread non_owner([&] { non_owner_status = sao_ui_entity_shell_try_destroy(shell); });
    non_owner.join();
    CHECK(non_owner_status == SAO_STATUS_ERR_ACCESS_DENIED);
    SaoUiEntityShellSnapshot snapshot{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &snapshot) == SAO_STATUS_OK);
    CHECK(snapshot.online);
    REQUIRE(sao_ui_entity_shell_try_destroy(shell) == SAO_STATUS_OK);
}

TEST_CASE("Entity shell borrows and preserves the central compositor",
          "[ui][entity_shell][compositor][borrowed]") {
    SaoCompositorConfig compositor_config{};
    compositor_config.target_hz = 60;
    compositor_config.enable_temporal_union = true;
    compositor_config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) ==
            SAO_STATUS_OK);

    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create_on_compositor(compositor, &config, &shell) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    CHECK(layer_count(compositor) == 2);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    sao_ui_entity_shell_destroy(shell);
    CHECK(layer_count(compositor) == 0);

    SaoLayerConfig sentinel_config{};
    sentinel_config.name_utf8 = "entity.borrowed.sentinel";
    sentinel_config.width = 4;
    sentinel_config.height = 4;
    sentinel_config.click_through = true;
    sentinel_config.bgra_swizzle = true;
    sao_ui_layer_handle_t sentinel = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &sentinel_config, &sentinel) == SAO_STATUS_OK);
    REQUIRE(sentinel != nullptr);
    sao_ui_layer_destroy(sentinel);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Entity borrowed construction failure leaves no stale compositor cleanup",
          "[ui][entity_shell][compositor][borrowed][construction][rollback]") {
    sao_ui_compositor_handle_t first_compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &first_compositor) == SAO_STATUS_OK);
    SaoLayerConfig sentinel_config{};
    sentinel_config.name_utf8 = "entity.borrowed.rollback.sentinel";
    sentinel_config.width = 4;
    sentinel_config.height = 4;
    sentinel_config.click_through = true;
    sao_ui_layer_handle_t sentinel = nullptr;
    REQUIRE(sao_ui_layer_create(first_compositor, &sentinel_config, &sentinel) == SAO_STATUS_OK);

    ActionLog first_actions;
    const auto first_config = headless_config(&first_actions);
    sao_status_t non_owner_status = SAO_STATUS_OK;
    sao_ui_entity_shell_handle_t non_owner_shell =
        reinterpret_cast<sao_ui_entity_shell_handle_t>(uintptr_t{1});
    std::thread non_owner([&] {
        non_owner_status = sao_ui_entity_shell_create_on_compositor(
            first_compositor, &first_config, &non_owner_shell);
    });
    non_owner.join();
    CHECK(non_owner_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(non_owner_shell == nullptr);
    CHECK(layer_count(first_compositor) == 1);

    sao_ui_entity_shell_handle_t failed_shell =
        reinterpret_cast<sao_ui_entity_shell_handle_t>(uintptr_t{1});
    sao_ui_test_fail_next_entity_shell_construction();
    CHECK(sao_ui_entity_shell_create_on_compositor(first_compositor, &first_config,
                                                    &failed_shell) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(failed_shell == nullptr);
    CHECK(layer_count(first_compositor) == 1);
    REQUIRE(sao_ui_layer_set_position(sentinel, 7, 9) == SAO_STATUS_OK);
    sao_ui_layer_destroy(sentinel);
    REQUIRE(sao_ui_compositor_try_destroy(first_compositor) == SAO_STATUS_OK);

    sao_ui_compositor_handle_t second_compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &second_compositor) == SAO_STATUS_OK);
    ActionLog second_actions;
    const auto second_config = headless_config(&second_actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create_on_compositor(second_compositor, &second_config, &shell) ==
            SAO_STATUS_OK);
    CHECK(layer_count(second_compositor) == 2);
    REQUIRE(sao_ui_entity_shell_try_destroy(shell) == SAO_STATUS_OK);
    CHECK(layer_count(second_compositor) == 0);
    REQUIRE(sao_ui_compositor_try_destroy(second_compositor) == SAO_STATUS_OK);
}

TEST_CASE("Entity action self-destroy finalizes after the central input batch",
          "[ui][entity_shell][compositor][input][destroy][deferred]") {
    SaoCompositorConfig compositor_config{};
    compositor_config.target_hz = 60;
    compositor_config.enable_temporal_union = true;
    compositor_config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) ==
            SAO_STATUS_OK);

    ActionLog actions;
    actions.destroy_self = true;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create_on_compositor(compositor, &config, &shell) ==
            SAO_STATUS_OK);
    actions.shell = shell;
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 1000) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const int32_t about_x = state.menu_x + kMenuSlotCenter;
    const int32_t about_y = state.menu_y + kMenuPad + 4 * kMenuSlot + kMenuSlot / 2;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, about_x, about_y, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, about_x, about_y, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, about_x, about_y, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(actions.calls == 1);
    CHECK(actions.self_destroy_status == SAO_STATUS_ERR_CANCELLED);
    CHECK(layer_count(compositor) == 0);

    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Entity central NerveGear input matches its logical circular hit shape",
          "[ui][entity_shell][compositor][input][shape]") {
    SaoCompositorConfig compositor_config{};
    compositor_config.target_hz = 60;
    compositor_config.enable_temporal_union = true;
    compositor_config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) ==
            SAO_STATUS_OK);
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create_on_compositor(compositor, &config, &shell) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    bool central_hit = true;
    bool entity_hit = true;
    REQUIRE(sao_ui_compositor_hit_test(compositor, state.nervegear_x, state.nervegear_y,
                                       &central_hit) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_hit_test(shell, state.origin_x + state.nervegear_x,
                                         state.origin_y + state.nervegear_y,
                                         &entity_hit) == SAO_STATUS_OK);
    CHECK_FALSE(central_hit);
    CHECK_FALSE(entity_hit);

    REQUIRE(sao_ui_compositor_hit_test(compositor, state.nervegear_x + 36,
                                       state.nervegear_y + 36, &central_hit) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_hit_test(shell, state.origin_x + state.nervegear_x + 36,
                                         state.origin_y + state.nervegear_y + 36,
                                         &entity_hit) == SAO_STATUS_OK);
    CHECK(central_hit);
    CHECK(entity_hit);

    REQUIRE(sao_ui_entity_shell_try_destroy(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Entity central wheel uses the current event coordinates instead of stale cursor",
          "[ui][entity_shell][compositor][input][wheel]") {
    SaoCompositorConfig compositor_config{};
    compositor_config.target_hz = 60;
    compositor_config.enable_temporal_union = true;
    compositor_config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) ==
            SAO_STATUS_OK);
    ActionLog actions;
    const auto config = headless_config(&actions);
    sao_ui_entity_shell_handle_t shell = nullptr;
    REQUIRE(sao_ui_entity_shell_create_on_compositor(compositor, &config, &shell) ==
            SAO_STATUS_OK);
    actions.shell = shell;
    const auto children = make_child_items(10);
    set_entity_children(shell, "Control", children);
    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    select_root(shell, 0);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    const int32_t child_host_x = state.menu_x + kChildRowX + 18;
    const int32_t child_host_y = state.menu_y + kChildRowY + kChildRowHeight / 2;
    const int32_t root_host_x = state.menu_x + kMenuSlotCenter;
    const int32_t root_host_y = state.menu_y + kMenuPad + kMenuSlot / 2;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, child_host_x, child_host_y,
                                              -1, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseWheel, root_host_x, root_host_y, -1,
                                              -120) == SAO_STATUS_OK);

    auto slot0 = child_point(state, 0);
    send_left_click(shell, slot0[0], slot0[1]);
    REQUIRE(actions.calls == 1);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseWheel, child_host_x, child_host_y,
                                              -1, -120) == SAO_STATUS_OK);
    send_left_click(shell, slot0[0], slot0[1]);
    REQUIRE(actions.calls == 2);
    CHECK(static_cast<int32_t>(actions.last) == kChildActionBase + 1);

    REQUIRE(sao_ui_entity_shell_try_destroy(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
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

    sao_ui_compositor_handle_t compositor = nullptr;
    const sao_status_t compositor_status = sao_ui_compositor_create(host, nullptr, &compositor);
    if (compositor_status != SAO_STATUS_OK) {
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D11/DirectComposition unavailable in this environment");
    }

    ActionLog actions;
    SaoUiEntityShellConfig shell_config{};
    shell_config.action_fn = &record_action;
    shell_config.action_user_data = &actions;
    sao_ui_entity_shell_handle_t shell = nullptr;
    const sao_status_t create_status =
        sao_ui_entity_shell_create_on_compositor(compositor, &shell_config, &shell);
    if (create_status != SAO_STATUS_OK) {
        sao_ui_compositor_destroy(compositor);
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D11/DirectComposition unavailable in this environment");
    }

    REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_set_visible(host, true) == SAO_STATUS_OK);
    CHECK(sao_ui_overlay_host_visible(host));
    REQUIRE(sao_ui_compositor_tick(compositor) == SAO_STATUS_OK);

    SaoUiEntityShellSnapshot state{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    HWND hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    const LPARAM point = MAKELPARAM(state.nervegear_x + 36, state.nervegear_y + 36);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
    REQUIRE(sao_ui_compositor_tick(compositor) == SAO_STATUS_OK);
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
    REQUIRE(sao_ui_compositor_tick(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_tick(compositor) == SAO_STATUS_OK);
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
    REQUIRE(sao_ui_compositor_tick(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    CHECK(state.origin_x == 180);
    CHECK(state.origin_y == 120);
    CHECK(state.width == 800);
    CHECK(state.height == 600);
    CHECK(state.nervegear_x == 708);
    CHECK(state.nervegear_y == 508);

    REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 16) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_tick(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &state) == SAO_STATUS_OK);
    region = CreateRectRgn(0, 0, 0, 0);
    REQUIRE(region != nullptr);
    REQUIRE(GetWindowRgn(hwnd, region) != ERROR);
    CHECK(PtInRegion(region, state.menu_x + kMenuSlotCenter, state.menu_y + kMenuSlotCenter));
    CHECK_FALSE(PtInRegion(region, old_menu_x + kMenuSlotCenter, old_menu_y + kMenuSlotCenter));
    DeleteObject(region);

    REQUIRE(sao_ui_entity_shell_take_offline(shell) == SAO_STATUS_OK);
    CHECK(sao_ui_overlay_host_visible(host));
    REQUIRE(sao_ui_entity_shell_try_destroy(shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}
#endif
