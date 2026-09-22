#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"

extern "C" {

typedef struct sao_ui_plugin_tabs_s* sao_ui_plugin_tabs_handle_t;
typedef sao_status_t(SAO_UI_CALL* sao_ui_plugin_tab_action_fn_t)(int32_t action_id,
                                                               void* user_data);

struct SaoUiPluginTabAction {
    size_t struct_size;
    const char* name_utf8;
    const char* icon_utf8;
    int32_t action_id;
    bool enabled;
    bool keep_open;
    bool close_before;
    uint8_t reserved;
};

enum SaoUiPluginTabStatus : uint8_t {
    SAO_UI_PLUGIN_TAB_STATUS_UNKNOWN = 0,
    SAO_UI_PLUGIN_TAB_STATUS_ACTIVE = 1,
    SAO_UI_PLUGIN_TAB_STATUS_DISABLED = 2,
};

struct SaoUiPluginTab {
    size_t struct_size;
    const char* plugin_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    const SaoUiPluginTabAction* actions;
    size_t action_count;
    bool enabled;
    // Lifecycle presentation is independent of selection and action availability.
    uint8_t status;
    uint8_t reserved[6];
};

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(SaoUiPluginTab) == 56);
static_assert(alignof(SaoUiPluginTab) == 8);
static_assert(offsetof(SaoUiPluginTab, struct_size) == 0);
static_assert(offsetof(SaoUiPluginTab, plugin_id_utf8) == 8);
static_assert(offsetof(SaoUiPluginTab, name_utf8) == 16);
static_assert(offsetof(SaoUiPluginTab, icon_utf8) == 24);
static_assert(offsetof(SaoUiPluginTab, actions) == 32);
static_assert(offsetof(SaoUiPluginTab, action_count) == 40);
static_assert(offsetof(SaoUiPluginTab, enabled) == 48);
static_assert(offsetof(SaoUiPluginTab, status) == 49);
static_assert(offsetof(SaoUiPluginTab, reserved) == 50);
#endif

struct SaoUiPluginTabsSnapshot {
    size_t struct_size;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    size_t tab_count;
    size_t first_visible;
    bool visible;
    bool dragging;
    uint8_t reserved[6];
    char selected_plugin_id_utf8[128];
    sao_status_t last_action_status;
};

// Item pointers are borrowed for set_items only; the UI retains value copies.
// UTF-8 byte limits: ID 127, icon 128, label 4096; tables allow 256 tabs and 1024 total actions.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_create(
    sao_ui_compositor_handle_t compositor, sao_ui_plugin_tab_action_fn_t action_fn,
    void* user_data, sao_ui_plugin_tabs_handle_t* out_handle);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_set_items(
    sao_ui_plugin_tabs_handle_t handle, const SaoUiPluginTab* items, size_t count);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_set_visible(
    sao_ui_plugin_tabs_handle_t handle, bool visible);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_tick(
    sao_ui_plugin_tabs_handle_t handle, uint32_t elapsed_ms);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_get_snapshot(
    sao_ui_plugin_tabs_handle_t handle, SaoUiPluginTabsSnapshot* out_snapshot);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_plugin_tabs_try_destroy(
    sao_ui_plugin_tabs_handle_t handle);

}
