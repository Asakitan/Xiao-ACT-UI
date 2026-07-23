#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/menu.h"
#include "sao/ui/nervegear.h"
#include "sao/ui/overlay_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_entity_shell_s* sao_ui_entity_shell_handle_t;

#define SAO_UI_ENTITY_ROOT_MAX_COUNT 64u
#define SAO_UI_ENTITY_ROOT_ID_CAPACITY 128u

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

// ABI v1 root descriptor. struct_size MUST be the first field so the
// platform can safely read the caller's declared size. The array ABI has no
// element stride field; every descriptor's struct_size must equal
// sizeof(SaoUiEntityRootItem) at the caller side (which equals
// SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE when both sides are compiled against
// ABI minor 8). Future ABI additions extending the tail must ship together
// with a matching platform-side reader that gates on struct_size.
struct SaoUiEntityRootItem {
    size_t struct_size;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    int32_t action_id;
    bool can_activate;
    uint8_t _pad[3];
    const SaoUiMenuItem* children;
    size_t child_count;
};

// Canonical byte size of the v1 root descriptor. x64 packing:
// 8 (struct_size) + 8+8+8 (3 char*) + 4 (int32) + 1 (bool) + 3 (pad) + 8
// (children) + 8 (child_count).
#define SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE 56u

struct SaoUiEntityRootSnapshot {
    size_t root_count;
    size_t first_visible_root_index;
    size_t visible_root_count;
    uint64_t root_tree_revision;
    char active_root_id_utf8[SAO_UI_ENTITY_ROOT_ID_CAPACITY];
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_create(
    sao_ui_overlay_host_handle_t host,
    const SaoUiEntityShellConfig* config,
    sao_ui_entity_shell_handle_t* out_handle);

// Attach Entity's NerveGear/menu layers to an existing compositor. The shell
// borrows the compositor and never presents or destroys it; the process-level
// owner drives sao_ui_compositor_tick after all feature layers update.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_create_on_compositor(
    sao_ui_compositor_handle_t compositor,
    const SaoUiEntityShellConfig* config,
    sao_ui_entity_shell_handle_t* out_handle);

#if defined(SAO_UI_TESTING)
SAO_UI_API void SAO_UI_CALL sao_ui_test_fail_next_entity_shell_construction(void);
#endif

// Destruction is owner-thread-affine and retryable. Owned compositor teardown
// failures preserve the shell handle and compositor ownership for another
// call. Borrowed compositors remain owned by their process-level host.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_try_destroy(
    sao_ui_entity_shell_handle_t handle);

// Compatibility wrapper that ignores the retryable status.
SAO_UI_API void SAO_UI_CALL sao_ui_entity_shell_destroy(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_bring_online(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_take_offline(
    sao_ui_entity_shell_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_set_nervgear_mode(
    sao_ui_entity_shell_handle_t handle,
    bool enabled);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_set_children(
    sao_ui_entity_shell_handle_t handle,
    const char* parent_name_utf8,
    const SaoUiMenuItem* items,
    size_t item_count);

// Replaces the complete root/child tree. Every descriptor's struct_size must
// equal sizeof(SaoUiEntityRootItem), because this array ABI has no element
// stride field. All descriptor pointers are borrowed only for this synchronous
// call; the shell retains a deep copy.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_set_roots(
    sao_ui_entity_shell_handle_t handle, const SaoUiEntityRootItem* roots, size_t root_count);

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

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_entity_shell_get_root_snapshot(
    sao_ui_entity_shell_handle_t handle, SaoUiEntityRootSnapshot* out_snapshot);

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

#if defined(__cplusplus)
static_assert(offsetof(SaoUiEntityRootItem, struct_size) == 0u,
              "SaoUiEntityRootItem::struct_size must be the first field");
static_assert(sizeof(SaoUiEntityRootItem) == SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE,
              "SaoUiEntityRootItem v1 size drift");
#endif
