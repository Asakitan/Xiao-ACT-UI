// SAO Auto — generic panel container.
//
// Python authoritative source:
//   `sao_auto/python/sao_panel_ui.py`  (panel base class)
//   `sao_auto/python/sao_gui/panel.py` (SaoToplevel base)
//   `sao_auto/python/render/tk_mirror.py` (mirror-attach semantics)
//   memory `ACT扁平化机制` — flat=True per-page mode
//   memory `webview整体停维护` — legacy webview panels stopped 2026-07-10
//
// A panel is a movable, resizable, dockable rectangle owned by the
// compositor.  Each plugin main UI is a panel.  Panels host widgets +
// child panels; layout is done via the UI spec normalizer in
// `engine/ui_spec.h`.
//
// ── Panel lifecycle model (memory memes) ──────────────────────
//   Single-instance-reuse pattern: `show()` checks `if not exists:
//   build()`, `hide()` only withdraws, only `destroy()` releases.
//   Workshop / ProcessSelector / PluginManager all use this.  New
//   panels MUST use this pattern — one-panel-per-open leaks
//   compositor layers + input proxies.
//
// ── Tk mirror mode (SaoToplevel) ──────────────────────────────
//   Legacy Tk panels attach as mirrors: real Tk window stays alive
//   with alpha=0.01 (PrintWindow requires window mapped); mirror
//   captures its content to a compositor layer.  hide() moves the
//   real window off-screen (-32000,-32000); show() moves it back.
//   NOT auto-withdrawn — that would black the mirror's first frame
//   on re-show.
//
// ── UI spec source ────────────────────────────────────────────
//   Layout via `engine/ui_spec.h` normalizer.  Panel accepts a JSON
//   spec, forwards to widget instantiation.
//
// ── Rendering paths ───────────────────────────────────────────
//   1. Native D2D widgets (default) — full GPU, no Tk mirror.
//   2. Tk mirror (legacy) — for panels using tkinter idioms not yet
//      ported to D2D.  Deprecated for new code (2026-07-10).
//   3. Custom render_fn — plugin-supplied GL rendering (fisheye, HP).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_panel_s* sao_ui_panel_handle_t;
typedef struct sao_ui_panel_body_s* sao_ui_panel_body_handle_t;

enum sao_ui_panel_rendering_e : int32_t {
    // Native D2D — spec-driven, no Tk.  Default for new panels.
    SAO_UI_PANEL_RENDER_NATIVE = 0,
    // Tk mirror — legacy panels; window stays alive with alpha 0.01
    // and off-screen when hidden.  Deprecated but supported.
    SAO_UI_PANEL_RENDER_TK_MIRROR = 1,
    // Custom render_fn attached via `set_render_fn`.
    SAO_UI_PANEL_RENDER_CUSTOM = 2,
};

// A render callback attempted to synchronously trigger another render
// of the same panel.  The nested operation is rejected; the outer render
// continues normally.
enum sao_ui_panel_status_e : int32_t {
    SAO_UI_PANEL_STATUS_ERR_BUSY = -102,
    SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED = -103,
};

// Panel style modifier (memory ACT扁平化机制).
enum sao_ui_panel_flat_e : int32_t {
    SAO_UI_PANEL_FLAT_INHERIT = 0, // use theme's default flat state
    SAO_UI_PANEL_FLAT_TRUE = 1,    // force flat=True style
    SAO_UI_PANEL_FLAT_FALSE = 2,   // force flat=False style
};

struct SaoPanelConfig {
    const char* panel_id_utf8; // unique across the process
    const char* title_utf8;
    int32_t default_x;
    int32_t default_y;
    int32_t default_width;
    int32_t default_height;
    int32_t min_width;
    int32_t min_height;
    int32_t max_width;  // 0 → unlimited
    int32_t max_height; // 0 → unlimited
    bool resizable;
    bool movable;
    bool show_titlebar;
    bool show_close_button;
    bool remember_geometry;
    bool single_instance;   // if true, second create returns existing handle
    int32_t rendering_mode; // sao_ui_panel_rendering_e
    int32_t flat_mode;      // sao_ui_panel_flat_e
    // Theme page id (`d2d_widgets.h` :: theme_page).  NULL → global.
    const char* theme_page_utf8;
};

// Panel lifecycle events.
enum sao_ui_panel_event_e : int32_t {
    SAO_UI_PANEL_EVENT_SHOW = 0,
    SAO_UI_PANEL_EVENT_HIDE = 1,
    SAO_UI_PANEL_EVENT_CLOSE = 2, // user clicked X
    SAO_UI_PANEL_EVENT_MOVE = 3,
    SAO_UI_PANEL_EVENT_RESIZE = 4,
    SAO_UI_PANEL_EVENT_DOCK = 5,
    SAO_UI_PANEL_EVENT_UNDOCK = 6,
    SAO_UI_PANEL_EVENT_FOCUS = 7,
    SAO_UI_PANEL_EVENT_BLUR = 8,
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_create(sao_ui_compositor_handle_t compositor,
                                                        const SaoPanelConfig* config,
                                                        sao_ui_panel_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_panel_destroy(sao_ui_panel_handle_t handle);

// Push an already-normalized UI spec into the panel body.  Spec keys
// per `engine/ui_spec.h`.  Panel rebuilds its widget tree.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_spec(sao_ui_panel_handle_t handle,
                                                          const uint8_t* spec_json_utf8,
                                                          size_t spec_len);

// Update a single widget's props without rebuilding the tree.
// widget_id is the id assigned in the spec (`"id": "row_1"`).
// Faster than set_spec for typical live updates (DPS row values).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_update_widget(sao_ui_panel_handle_t handle,
                                                               const char* widget_id_utf8,
                                                               const uint8_t* props_json_utf8,
                                                               size_t props_len);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_visible(sao_ui_panel_handle_t handle,
                                                             bool visible);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_position(sao_ui_panel_handle_t handle,
                                                              int32_t x, int32_t y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_geometry(sao_ui_panel_handle_t handle,
                                                              int32_t x, int32_t y, int32_t width,
                                                              int32_t height);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_panel_get_layout_tree(sao_ui_panel_handle_t handle, sao_ui_layout_tree_handle_t* out_tree,
                             sao_ui_layout_node_handle_t* out_root);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_rasterize(sao_ui_panel_handle_t handle,
                                                           sao_ui_offscreen_raster_handle_t raster);

// Get the compositor layer id — used by tests / diagnostics.
SAO_UI_API sao_ui_layer_handle_t SAO_UI_CALL sao_ui_panel_layer(sao_ui_panel_handle_t handle);

// Panel action callback — fired when the user clicks a widget marked
// with `action` in the UI spec.
typedef void(SAO_UI_CALL* sao_ui_panel_action_callback_t)(const char* action_key_utf8,
                                                          const uint8_t* action_arg_json_utf8,
                                                          size_t action_arg_len, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_action_handler(
    sao_ui_panel_handle_t handle, sao_ui_panel_action_callback_t callback, void* user_data);

// Panel lifecycle event callback.
typedef void(SAO_UI_CALL* sao_ui_panel_event_callback_t)(int32_t event_kind, // sao_ui_panel_event_e
                                                         void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_event_handler(
    sao_ui_panel_handle_t handle, sao_ui_panel_event_callback_t callback, void* user_data);

// Custom render_fn for CUSTOM rendering mode.  fn is called every
// frame while visible; ctx is the D3D11/D2D paint context (see
// `d2d_widgets.h::sao_ui_paint_ctx_handle_t`).
typedef void(SAO_UI_CALL* sao_ui_panel_render_fn_t)(void* paint_ctx, float x, float y, float width,
                                                    float height, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_set_render_fn(sao_ui_panel_handle_t handle,
                                                               sao_ui_panel_render_fn_t fn,
                                                               void* user_data);

// Query current state.
struct SaoPanelState {
    bool visible;
    bool focused;
    bool docked;
    uint8_t _pad;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_get_state(sao_ui_panel_handle_t handle,
                                                           SaoPanelState* out_state);

// Internal descriptor-body bridge used by panel_sdk.cpp.  The complete
// model is rebuilt off to the side, rasterized, uploaded once, and only
// then adopted by the panel.  Widget handles are borrowed.
struct SaoUiPanelBodyModelNode {
    uint64_t model_id;
    uint64_t parent_model_id; // 0 only for the root
    int32_t layout_mode;
    SaoUiLayoutSpec spec;
    sao_ui_widget_handle_t widget;  // NULL for containers
    const uint8_t* props_json_utf8; // merged props, optional
    size_t props_len;
    const uint8_t* rollback_props_json_utf8;
    size_t rollback_props_len;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_replace_body_model(
    sao_ui_panel_handle_t handle, const SaoUiPanelBodyModelNode* nodes, size_t node_count,
    sao_ui_layout_node_handle_t* out_layout_nodes, size_t out_layout_node_capacity,
    sao_ui_layout_tree_handle_t* out_tree);

// Look up an existing panel by id.  Used by the single-instance
// pattern.  Returns SAO_STATUS_ERR_NOT_FOUND if not present.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_find_by_id(sao_ui_compositor_handle_t compositor,
                                                            const char* panel_id_utf8,
                                                            sao_ui_panel_handle_t* out_handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_panel_enumerate(sao_ui_compositor_handle_t compositor,
                                                           sao_ui_panel_handle_t* out_handles,
                                                           size_t capacity, size_t* out_written);

// SDK consumer-facing descriptor API (register / update_body / theme
// override / bring_to_front / geometry persist) lives in `panel_sdk.h`
// — split out to keep this header under the 350-line per-file budget.

#ifdef __cplusplus
} // extern "C"
#endif
