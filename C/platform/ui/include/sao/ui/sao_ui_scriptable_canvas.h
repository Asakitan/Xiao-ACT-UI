// SAO Auto — scriptable canvas for plugin script languages.
//
// A canvas is a widget whose entire paint is described by a stream of
// low-level draw ops.  Plugins in Lua / Emma / AngelScript / C# use
// this to build custom visuals (piano keyboards, particle sprays,
// notification banners, MIDI note rolls, candy demo layers…) without
// linking Direct2D directly.
//
// The op vocabulary is intentionally small — line, rect, oval, polygon,
// text, bitmap — matching Python `act_platform/ui_spec.py::CANVAS_OPS`
// so an existing plugin's Python canvas ports to native by handing
// the same op list here.  A discipline layer of state helpers
// (transform stack, stroke/fill/font/opacity) sits on top so the op
// stream reads like plain drawing code.
//
// Design contract (memory [脚本插件canvas UI三坑]):
//   * Lua tables must not be exposed as lists — the C ops are declared
//     via `SaoUiCanvasOp` structs so bindings never guess table shape.
//   * `draggable_owns_pointer` and the pointer callback are metadata for the
//     upper host input router; this canvas does not perform top-most hit-test
//     selection or automatically dispatch pointer events.
//   * lupa on lua54: OK to expose object methods; hoist to local before
//     tight loops in the calling script.  On lua55: bindings must NOT
//     rely on attribute caching (see the same memory note).
//
// Python source alignment:
//   * ui_spec.py ::_normalize_canvas — same op vocabulary
//   * plugins/candy_demo/candy_render.py — reference producer
//   * plugins/midi_piano_plugin — reference consumer
//   * gui_modules/sao_plugin_ui_render.py + web/plugin_layer.js — dual
//     renderers on the Python side; both must produce identical output
//     for a given normalized canvas.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_script_canvas_s* sao_ui_script_canvas_handle_t;

// ─── Op vocabulary — matches CANVAS_OPS in ui_spec.py ────────────────
enum sao_ui_canvas_op_e : int32_t {
    SAO_UI_CANVAS_OP_BEGIN_PATH  = 0,       // clears current subpath state
    SAO_UI_CANVAS_OP_LINE        = 1,
    SAO_UI_CANVAS_OP_RECT        = 2,       // filled + optional outline
    SAO_UI_CANVAS_OP_OVAL        = 3,
    SAO_UI_CANVAS_OP_ROUNDED_RECT = 4,
    SAO_UI_CANVAS_OP_POLYGON     = 5,       // closed poly; verts in aux[]
    SAO_UI_CANVAS_OP_TEXT        = 6,
    SAO_UI_CANVAS_OP_BITMAP      = 7,       // BGRA blit from set_bitmap
    SAO_UI_CANVAS_OP_ARC         = 8,       // circular arc segment
    SAO_UI_CANVAS_OP_QUAD_CURVE  = 9,       // 3 control points
    SAO_UI_CANVAS_OP_CUBIC_CURVE = 10,      // 4 control points
    SAO_UI_CANVAS_OP_STROKE_PATH = 11,      // stroke current subpath
    SAO_UI_CANVAS_OP_FILL_PATH   = 12,      // fill current subpath
    // State ops — mutate transform / paint state.  Do not draw.
    SAO_UI_CANVAS_OP_PUSH_STATE  = 100,
    SAO_UI_CANVAS_OP_POP_STATE   = 101,
    SAO_UI_CANVAS_OP_TRANSLATE   = 102,
    SAO_UI_CANVAS_OP_SCALE       = 103,
    SAO_UI_CANVAS_OP_ROTATE      = 104,     // degrees
    SAO_UI_CANVAS_OP_SET_STROKE  = 110,     // stroke ARGB in i32[0]
    SAO_UI_CANVAS_OP_SET_FILL    = 111,     // fill   ARGB
    SAO_UI_CANVAS_OP_SET_LINE_W  = 112,     // width in f32[0]
    SAO_UI_CANVAS_OP_SET_FONT    = 113,     // font_slot in i32[0], size in i32[1]
    SAO_UI_CANVAS_OP_SET_OPACITY = 114,     // 0..1 in f32[0]
    SAO_UI_CANVAS_OP_SET_CLIP    = 115,     // clip rect (x,y,w,h) in i32[0..3]
    SAO_UI_CANVAS_OP_CLEAR_CLIP  = 116,
    SAO_UI_CANVAS_OP_SET_BLEND   = 117,     // src / add / mult in i32[0]
};

enum sao_ui_canvas_blend_e : int32_t {
    SAO_UI_CANVAS_BLEND_NORMAL    = 0,
    SAO_UI_CANVAS_BLEND_ADDITIVE  = 1,
    SAO_UI_CANVAS_BLEND_MULTIPLY  = 2,
    SAO_UI_CANVAS_BLEND_SCREEN    = 3,
    SAO_UI_CANVAS_BLEND_SUBTRACT  = 4,
    SAO_UI_CANVAS_BLEND_MASK      = 5,      // dst *= src.a
};

// ─── One op record ───────────────────────────────────────────────────
//
// The op payload is a discriminated union of small arrays.  Bindings
// generate arrays of these structs; the runtime interprets them.  This
// mirrors the Python `_normalize_canvas` "list of dicts" pattern but
// keeps the wire format pointer-free until aux data is needed.
struct SaoUiCanvasOp {
    int32_t op;                             // sao_ui_canvas_op_e
    // Draw-op geometry payload (pixels).  Interpretation depends on op:
    //   LINE           : (x1, y1, x2, y2)          in i[0..3]
    //   RECT           : (x, y, w, h)              in i[0..3]
    //   OVAL           : (x, y, w, h)              in i[0..3]
    //   ROUNDED_RECT   : (x, y, w, h)              in i[0..3]; radius in f[0]
    //   POLYGON        : point count               in i[0]; verts via aux
    //   TEXT           : (x, y)                    in i[0..1]; size in i[2]
    //   BITMAP         : (dst_x, dst_y, dst_w, dst_h) in i[0..3];
    //                    bitmap id in i[4]
    //   ARC            : (cx, cy, r)               in i[0..2];
    //                    start deg, sweep deg      in f[0..1]
    //   QUAD_CURVE     : (x1,y1,x2,y2,x3,y3)       in i[0..5]
    //   CUBIC_CURVE    : (x1,y1,x2,y2,x3,y3,x4,y4) in i[0..5] + i_ex[0..1]
    int32_t i[6];                           // primary ints
    int32_t i_ex[2];                        // cubic endpoint (x4, y4)
    float   f[4];                           // primary floats

    // Op-specific extras — polygon verts point at 2*count int32s; text
    // points at a UTF-8 c-string; bitmap uses bitmap id in i[4].  None
    // of these are owned by the runtime — bindings must keep the memory
    // alive until submit_ops() or the corresponding shortcut returns.
    const void* aux;                        // polygon verts / text ptr etc.
    size_t      aux_len;
};

// ─── Canvas spec ─────────────────────────────────────────────────────
struct SaoUiScriptCanvasSpec {
    int32_t  width_px;
    int32_t  height_px;
    uint32_t bg_argb;                       // 0 → transparent
    // Pointer ownership metadata for the upper host input router.  The
    // canvas does not perform top-most hit-testing or automatic dispatch.
    bool     draggable_owns_pointer;
    // Anti-alias globally (per-op override via SET_BLEND is separate).
    bool     antialias;
    // If true, canvas keeps last submitted ops until a new begin_draw;
    // when false, ops must be resubmitted every frame.
    bool     retain_ops_between_frames;
    // Cap on ops per draw — bindings that exceed silently drop the
    // overflow (matches Python MAX_CANVAS_OPS = 4000).
    uint8_t  _pad;
    int32_t  max_ops_per_frame;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_create(
    void* d3d_device_ptr,
    const SaoUiScriptCanvasSpec* spec,
    sao_ui_widget_handle_t* out_widget,     // usable as a normal widget
    sao_ui_script_canvas_handle_t* out_canvas);

SAO_UI_API void SAO_UI_CALL sao_ui_script_canvas_destroy(
    sao_ui_script_canvas_handle_t canvas);

// ─── Draw session — begin / submit ops / end ─────────────────────────
//
// The session model matches HTML5 <canvas> — begin_draw clears any
// prior op list (if retain==false), pushes an initial state, and gives
// the caller a fresh slate.  end_draw commits and swaps the buffer.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_begin_draw(
    sao_ui_script_canvas_handle_t canvas);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_submit_ops(
    sao_ui_script_canvas_handle_t canvas,
    const SaoUiCanvasOp* ops,
    size_t op_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_end_draw(
    sao_ui_script_canvas_handle_t canvas);

// ─── High-level shortcuts (call inside a draw session) ───────────────
//
// These build SaoUiCanvasOp records under the hood for bindings that
// prefer a fluent API instead of struct arrays.  Not more efficient
// than submit_ops — pure convenience.

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_line(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_rect(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_rounded_rect(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h, float radius);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_oval(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_polygon(
    sao_ui_script_canvas_handle_t canvas,
    const int32_t* verts_xy_pairs, size_t point_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_text(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y,
    const char* text_utf8,
    int32_t font_slot, int32_t font_size_px);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    int32_t bitmap_id,
    int32_t dst_x, int32_t dst_y,
    int32_t dst_w, int32_t dst_h);

// ─── Bitmap registration — reusable BGRA sources ─────────────────────
//
// Bitmaps are referenced by integer id inside ops so scripts don't need
// to shuttle pixel arrays every frame.  Alignment: 4bpp premultiplied
// BGRA, top-down.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_register_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    const void* bgra_pixels,
    uint32_t width, uint32_t height, uint32_t stride,
    int32_t* out_bitmap_id);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_update_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    int32_t bitmap_id,
    const void* bgra_pixels,
    uint32_t width, uint32_t height, uint32_t stride);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_unregister_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    int32_t bitmap_id);

// ─── State helpers (thin wrappers around SET_* / PUSH/POP ops) ───────
//
// Match the CSS-canvas API surface most scripting languages already
// grok.  Every call issues exactly one op.

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_stroke_color(
    sao_ui_script_canvas_handle_t canvas, uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_fill_color(
    sao_ui_script_canvas_handle_t canvas, uint32_t argb);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_line_width(
    sao_ui_script_canvas_handle_t canvas, float width_px);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_font(
    sao_ui_script_canvas_handle_t canvas,
    int32_t font_slot, int32_t font_size_px);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_opacity(
    sao_ui_script_canvas_handle_t canvas, float opacity_0_to_1);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_blend(
    sao_ui_script_canvas_handle_t canvas, int32_t mode);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_translate(
    sao_ui_script_canvas_handle_t canvas, float dx, float dy);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_scale(
    sao_ui_script_canvas_handle_t canvas, float sx, float sy);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_rotate_deg(
    sao_ui_script_canvas_handle_t canvas, float degrees);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_push_state(
    sao_ui_script_canvas_handle_t canvas);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_pop_state(
    sao_ui_script_canvas_handle_t canvas);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_clip(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_clear_clip(
    sao_ui_script_canvas_handle_t canvas);

// ─── Pointer / input events (bindings register callbacks) ────────────
//
// The host input router may use this callback and its user data when it
// chooses to dispatch an event.  The canvas only stores the callback
// metadata; it does not hit-test or invoke it from draw/snapshot paths.
// Coordinates are canvas-local pixels.

typedef void (SAO_UI_CALL* sao_ui_script_canvas_pointer_cb_t)(
    int32_t event_kind,                     // sao_ui_input_event_kind_e
    int32_t x_px, int32_t y_px,
    int32_t button, int32_t wheel_delta,
    uint32_t modifiers, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_pointer_handler(
    sao_ui_script_canvas_handle_t canvas,
    sao_ui_script_canvas_pointer_cb_t callback,
    void* user_data);

// Request a redraw — used by animation loops that submit ops each
// frame.  Cheap when the canvas is already dirty.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_invalidate(
    sao_ui_script_canvas_handle_t canvas);

// Introspection — used by unit tests + the Python-side dual renderer
// (act_platform/ui_spec.py) so a single ops list can be diffed against
// the Web and Direct2D renderers.
// The aux pointers returned in SaoUiCanvasOp remain valid until the next
// snapshot call for this canvas or until sao_ui_script_canvas_destroy.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_snapshot_ops(
    sao_ui_script_canvas_handle_t canvas,
    SaoUiCanvasOp* out_ops,
    size_t capacity,
    size_t* out_written);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_script_canvas_rasterize(
    sao_ui_script_canvas_handle_t canvas,
    sao_ui_offscreen_raster_handle_t raster,
    int32_t offset_x_px,
    int32_t offset_y_px);

#ifdef __cplusplus
}  // extern "C"
#endif
