#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/nervegear.h"
#include "sao/ui/overlay_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_entity_shell_s* sao_ui_entity_shell_handle_t;

enum SaoUiEntityAction : int32_t {
    // Built-in token groups are 1 (About), 100-105 (Control), 110-112 (Tools),
    // 120-122 (Plugins), and 130-131 (Skins). Assigned tokens are never reused;
    // unlisted values do not establish a public provider range.
    SAO_UI_ENTITY_ACTION_OPEN_ABOUT = 1,
    SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST = 100,
    SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR = 101,
    SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE = 102,
    SAO_UI_ENTITY_ACTION_SAVE_SETTINGS = 103,
    SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL = 104,
    SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE = 105,
    SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR = 110,
    SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP = 111,
    SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR = 112,
    SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER = 120,
    SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS = 121,
    SAO_UI_ENTITY_ACTION_PLUGIN_STATUS = 122,
    SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT = 130,
    SAO_UI_ENTITY_ACTION_SET_ALL_DARK = 131,
};

typedef sao_status_t(SAO_UI_CALL* sao_ui_entity_action_fn_t)(
    SaoUiEntityAction action, void* user_data);

struct SaoUiEntityShellConfig {
    int32_t width;
    int32_t height;
    int32_t origin_x;
    int32_t origin_y;
    sao_ui_entity_action_fn_t action_fn;
    void* action_user_data;
};

struct SaoUiEntityShellSnapshot {
    int32_t origin_x;
    int32_t origin_y;
    int32_t width;
    int32_t height;
    int32_t nervegear_x;
    int32_t nervegear_y;
    int32_t menu_x;
    int32_t menu_y;
    int32_t menu_width;
    int32_t menu_height;
    int32_t menu_hover_index;
    int32_t menu_pressed_index;
    SaoUiNerveGearState nervegear_state;
    bool online;
    bool overlay_visible;
    bool menu_visible;
    uint8_t _pad[1];
    uint64_t frame_count;
    uint64_t action_count;
    sao_status_t last_status;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_create(
    sao_ui_overlay_host_handle_t host,
    const SaoUiEntityShellConfig* config,
    sao_ui_entity_shell_handle_t* out_handle);

// Destruction is owner-thread-affine. A non-owner call only detaches the
// shell's host callbacks; the owner must call destroy again to release the
// shell and its D3D11/DirectComposition resources.
SAO_UI_API void SAO_UI_CALL sao_ui_entity_shell_destroy(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_bring_online(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_take_offline(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_tick(
    sao_ui_entity_shell_handle_t handle,
    uint32_t elapsed_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_handle_mouse(
    sao_ui_entity_shell_handle_t handle,
    uint32_t message,
    int32_t screen_x,
    int32_t screen_y,
    int32_t button,
    int32_t wheel_delta);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_hit_test(
    sao_ui_entity_shell_handle_t handle,
    int32_t screen_x,
    int32_t screen_y,
    bool* out_hit);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_home(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_insert(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_get_snapshot(
    sao_ui_entity_shell_handle_t handle,
    SaoUiEntityShellSnapshot* out_snapshot);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_snapshot_bgra(
    sao_ui_entity_shell_handle_t handle,
    uint8_t* out_bgra_pixels,
    size_t capacity,
    uint32_t* out_width,
    uint32_t* out_height,
    size_t* out_bytes);

#ifdef __cplusplus
}
#endif
