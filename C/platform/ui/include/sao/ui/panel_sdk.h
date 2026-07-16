// SAO Auto — SDK consumer-facing panel descriptor API.
//
// Modern registration path for plugin panels.  Legacy `SaoPanelConfig`
// + `sao_ui_panel_set_spec()` (in panel.h) stays alive for ui_spec JSON
// consumers; the descriptor path below is what plugin SDK bindings
// speak:
//
//   1. Plugin fills a `SaoPanelDescriptor` once.
//   2. Calls `sao_ui_panel_register()` — gets a panel + body handle.
//   3. Populates the body's layout tree using panel_layout.h APIs
//      (sao_ui_layout_node_add_container / add_widget).
//   4. Live updates go through `sao_ui_panel_update_body()` batched
//      mutations so the compositor sees one dirty-rects snapshot per
//      batch instead of per-widget touch.
//
// Split from panel.h to stay under the 350-line per-header budget while
// giving both the legacy ui_spec path and the modern descriptor path
// room to grow.
//
// Python source alignment:
//   * plugin_manager.register_panel()          — host-side registration
//   * gui_modules/sao_panel_ui.py              — panel body & geometry
//   * settings `panel_themes[<panel>]`         — theme override
//   * _enforce_z_order + z_order.h manager     — z / topmost

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/panel.h"
#include "sao/ui/panel_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Anchor point — where the panel snaps within host viewport ───────
enum sao_ui_panel_anchor_e : int32_t {
    SAO_UI_PANEL_ANCHOR_ABSOLUTE     = 0,
    SAO_UI_PANEL_ANCHOR_TOP_LEFT     = 1,
    SAO_UI_PANEL_ANCHOR_TOP          = 2,
    SAO_UI_PANEL_ANCHOR_TOP_RIGHT    = 3,
    SAO_UI_PANEL_ANCHOR_RIGHT        = 4,
    SAO_UI_PANEL_ANCHOR_BOTTOM_RIGHT = 5,
    SAO_UI_PANEL_ANCHOR_BOTTOM       = 6,
    SAO_UI_PANEL_ANCHOR_BOTTOM_LEFT  = 7,
    SAO_UI_PANEL_ANCHOR_LEFT         = 8,
    SAO_UI_PANEL_ANCHOR_CENTER       = 9,
    // Anchor relative to another named panel — BuffMon sits above
    // HpOverlay, BossBuff sits right of BossHp, etc.
    SAO_UI_PANEL_ANCHOR_FOLLOW       = 10,
};

// Z-order class (memory `input proxy逐像素+防焦点偷` / z_order.h).
// Only sao_ui_z_order_manager may promote across classes; panels stay
// within their class.
enum sao_ui_panel_z_class_e : int32_t {
    SAO_UI_PANEL_Z_NORMAL  = 0,
    SAO_UI_PANEL_Z_TOPMOST = 1,   // above game — overlay panels (DPS/BossHP)
    SAO_UI_PANEL_Z_BOTTOM  = 2,
};

struct SaoPanelDescriptor {
    const char* panel_id_utf8;                // unique across process
    const char* title_utf8;
    // Anchor + geometry.  When anchor != ABSOLUTE, (x, y) act as pixel
    // offsets from the anchor edge / corner.
    int32_t     anchor;                       // sao_ui_panel_anchor_e
    int32_t     default_x_px;
    int32_t     default_y_px;
    int32_t     default_width_px;
    int32_t     default_height_px;
    int32_t     min_width_px;
    int32_t     min_height_px;
    int32_t     max_width_px;                 // 0 → unbounded
    int32_t     max_height_px;
    // Interactivity flags — 1:1 with SaoPanelConfig.
    bool        movable;
    bool        resizable;
    bool        show_titlebar;
    bool        show_close_button;
    bool        visible;                      // initial state
    bool        remember_geometry;
    bool        modal;                        // grabs input focus until closed
    bool        overlay_style;                // click-through outside widget
                                              // region (DPS / BossHP)
    // Z-order.
    int32_t     z_class;                      // sao_ui_panel_z_class_e
    int32_t     z_within_class;               // higher wins ties
    // Follow-anchor sibling id (only when anchor == FOLLOW).
    const char* follow_panel_id_utf8;
    int32_t     follow_offset_x_px;
    int32_t     follow_offset_y_px;
    // Theme override — token map JSON.  NULL → inherit host theme.
    // Matches Python `panel_themes[<panel>]` (buffmon_dark / hp_light).
    const char* theme_override_json_utf8;
    // Optional icon (BGRA 4bpp) for titlebar / task list.
    const void* icon_bgra_pixels;
    uint32_t    icon_width;
    uint32_t    icon_height;
    uint32_t    icon_stride;
    float       initial_opacity;              // DPS default 0.93
    // DisplayAffinity — only honoured in streaming mode.
    bool        exclude_from_capture;
    bool        auto_scroll;                  // wrap body in scroll view
    uint8_t     _pad[6];
};

// Register a panel using the modern descriptor path.  Allocates a
// compositor layer, wires the input router, and returns the panel +
// body handle (root of the layout tree the plugin fills).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_register(
    sao_ui_compositor_handle_t compositor,
    const SaoPanelDescriptor* descriptor,
    sao_ui_panel_handle_t* out_panel,
    sao_ui_panel_body_handle_t* out_body);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_unregister(
    sao_ui_panel_handle_t panel);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_get_descriptor(
    sao_ui_panel_handle_t panel,
    SaoPanelDescriptor* descriptor_out);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_registry_count(
    size_t* count_out);

// Access the body's layout root + tree.  Plugins add container/widget
// nodes through panel_layout.h using these handles.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_body_get_root(
    sao_ui_panel_body_handle_t body,
    sao_ui_layout_node_handle_t* out_root);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_body_get_tree(
    sao_ui_panel_body_handle_t body,
    sao_ui_layout_tree_handle_t* out_tree);

// Incremental body updates — batched so the compositor sees one
// dirty-rects snapshot per call, not per widget touch.
enum sao_ui_body_mutation_kind_e : int32_t {
    SAO_UI_BODY_ADD_WIDGET          = 0,
    SAO_UI_BODY_ADD_CONTAINER       = 1,
    SAO_UI_BODY_REMOVE_NODE         = 2,
    SAO_UI_BODY_UPDATE_SPEC         = 3,
    SAO_UI_BODY_REORDER_NODE        = 4,
    SAO_UI_BODY_UPDATE_WIDGET_PROPS = 5,      // widget-specific JSON props
};

struct SaoUiBodyMutation {
    int32_t                       kind;       // sao_ui_body_mutation_kind_e
    sao_ui_layout_node_handle_t   target;     // parent for ADD_*, self for others
    int32_t                       layout_mode; // for ADD_CONTAINER
    int32_t                       new_index;  // for REORDER
    sao_ui_widget_handle_t        widget;     // for ADD_WIDGET
    const SaoUiLayoutSpec*        spec;       // for ADD_* + UPDATE_SPEC
    const uint8_t*                props_json_utf8; // for UPDATE_WIDGET_PROPS
    size_t                        props_len;
    sao_ui_layout_node_handle_t*  out_new_node;    // NULL if caller uninterested
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_update_body(
    sao_ui_panel_body_handle_t body,
    const SaoUiBodyMutation* mutations,
    size_t mutation_count);

// Replace the entire body from a ui_spec JSON.  Equivalent to
// REMOVE_NODE(root) + normalize + dispatch mutations.  Kept as the
// primary path for scripting-language plugins that emit ui_spec but
// don't want to speak SaoUiBodyMutation records directly.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_body_set_spec(
    sao_ui_panel_body_handle_t body,
    const uint8_t* spec_json_utf8,
    size_t spec_len);

// Show / hide with proper z-order + focus enforcement.  These delegate
// to z_order.h internally — plugins never call SetWindowPos directly.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_show(
    sao_ui_panel_handle_t panel);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_hide(
    sao_ui_panel_handle_t panel);

// Raise the panel above its z-class siblings.  Never crosses classes.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_bring_to_front(
    sao_ui_panel_handle_t panel);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_send_to_back(
    sao_ui_panel_handle_t panel);

// Opacity control (0..1).  DPS default 0.93; BossHP fixed 1.0.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_opacity(
    sao_ui_panel_handle_t panel, float opacity_0_to_1);

// Theme override — repeats broadcast via `theme.h` to descendants.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_theme_override(
    sao_ui_panel_handle_t panel,
    const uint8_t* override_json_utf8,
    size_t override_len);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_clear_theme_override(
    sao_ui_panel_handle_t panel);

// Persistence hook — fires after ~500ms debounce when geometry changes
// and `remember_geometry == true`.  Host writes to settings.json (or a
// plugin-scoped store) so the panel reopens where the user left it.
typedef void (SAO_UI_CALL* sao_ui_panel_geometry_cb_t)(
    const char* panel_id_utf8,
    int32_t x, int32_t y, int32_t width, int32_t height,
    void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_geometry_persist_handler(
    sao_ui_panel_handle_t panel,
    sao_ui_panel_geometry_cb_t callback,
    void* user_data);

#ifdef __cplusplus
}  // extern "C"
#endif
