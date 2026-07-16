// SAO Auto — scriptable canvas (G3.12, Wave 4).
//
// Implementation of the plugin script-language canvas: a widget whose
// entire paint is described by an op stream, matching the CANVAS_OPS
// vocabulary used by the Python-side act_platform/ui_spec.py.
//
// This slice focuses on the ops-append + bitmap registry + snapshot
// surface — the D2D rasteriser that consumes the op list arrives in a
// downstream slice with the paint context wire-up.  Everything below
// is testable head-only (no D3D11 required, no live paint context).
//
// Op vocabulary summary (see header comment for the full mapping):
//   * 13 draw ops (BEGIN_PATH .. FILL_PATH)
//   * 15 state ops (PUSH_STATE .. SET_BLEND)
//   The header also exposes fluent shortcut APIs — draw_line, draw_rect,
//   push_state, set_stroke_color etc. — which we implement here by
//   pushing the same SaoUiCanvasOp records the raw submit_ops path
//   would produce, matching the "17 op kind" surface the wave-4 slice
//   contract asks for.
//
// Python source alignment:
//   * act_platform/ui_spec.py::CANVAS_OPS
//   * plugins/candy_demo/candy_render.py — reference producer
//   * gui_modules/sao_plugin_ui_render.py — dual renderer
//   * memory [脚本插件canvas UI三坑] — Lua binding pitfalls

#include "sao/ui/sao_ui_scriptable_canvas.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ─── Bitmap store ──────────────────────────────────────────────────
struct BitmapRecord {
    std::vector<uint8_t> pixels;  // owned copy (BGRA, top-down, premul)
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
};

// ─── Op storage with owned aux buffers ──────────────────────────────
//
// SaoUiCanvasOp.aux is documented as non-owning; the runtime keeps
// pointers valid only until end_draw returns.  We honour that by
// copying aux content (polygon verts, text strings) into per-op
// owned buffers so the op stream remains valid across begin/end
// pairs while retain_ops_between_frames == true.

struct OwnedOp {
    SaoUiCanvasOp op{};
    // Owning storage for aux content — chosen by op kind.
    std::vector<int32_t> poly_verts;   // POLYGON verts (2 * count int32)
    std::string          text_utf8;    // TEXT payload
};

}  // namespace

// ─── Canvas record ──────────────────────────────────────────────────
struct sao_ui_script_canvas_s {
    int32_t tag{160};
    std::mutex mu;
    SaoUiScriptCanvasSpec spec{};

    // Ops buffered inside the current draw session, then swapped into
    // `committed_ops` on end_draw.  When retain_ops_between_frames is
    // true, committed_ops survives across begin_draw calls (a re-begin
    // resets the pending buffer but keeps the committed one).
    std::vector<OwnedOp> pending_ops;
    std::vector<OwnedOp> committed_ops;
    bool                 draw_open       = false;
    uint64_t             begin_count     = 0;
    uint64_t             end_count       = 0;

    // Bitmap registry.
    std::unordered_map<int32_t, BitmapRecord> bitmaps;
    int32_t next_bitmap_id = 1;

    // Pointer input callback.
    sao_ui_script_canvas_pointer_cb_t pointer_cb = nullptr;
    void*                             pointer_ud = nullptr;

    // Invalidation counter — bumped whenever ops commit or an
    // explicit invalidate request lands.
    uint64_t invalidation_seq = 0;

    // Owning widget handle emitted at create.  Test rigs use it to
    // verify routing; we synthesize a unique per-canvas value so the
    // pointer never collides with a real widget record.
    sao_ui_widget_handle_t widget = nullptr;
};

namespace {

// Reserve op capacity in the pending buffer, respecting the per-frame
// cap (mirrors Python MAX_CANVAS_OPS silent-drop semantics).
bool can_append_op(const sao_ui_script_canvas_s* canvas) {
    if (canvas->spec.max_ops_per_frame <= 0) return true;
    return static_cast<int32_t>(canvas->pending_ops.size()) <
           canvas->spec.max_ops_per_frame;
}

// Append a plain op (no aux) to the pending buffer.  Locks the caller
// must hold `canvas->mu` before calling this helper.
sao_status_t append_simple(sao_ui_script_canvas_s* canvas,
                           const SaoUiCanvasOp& op) {
    if (!canvas->draw_open) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!can_append_op(canvas)) return SAO_STATUS_OK;  // silent drop
    OwnedOp owned{};
    owned.op = op;
    // Aux pointer belongs to the OwnedOp arrays — set nullptr here;
    // callers that need aux must go through the *_with_aux helpers.
    owned.op.aux     = nullptr;
    owned.op.aux_len = 0;
    canvas->pending_ops.push_back(std::move(owned));
    return SAO_STATUS_OK;
}

sao_status_t append_polygon(sao_ui_script_canvas_s* canvas,
                            const int32_t* verts_xy_pairs,
                            size_t point_count) {
    if (!canvas->draw_open) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!can_append_op(canvas)) return SAO_STATUS_OK;
    if (verts_xy_pairs == nullptr || point_count == 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    OwnedOp owned{};
    owned.op.op = SAO_UI_CANVAS_OP_POLYGON;
    owned.op.i[0] = static_cast<int32_t>(point_count);
    owned.poly_verts.assign(verts_xy_pairs,
                            verts_xy_pairs + point_count * 2);
    canvas->pending_ops.push_back(std::move(owned));
    // Rewire aux pointer *after* move so it points into the vector's
    // final storage location — std::vector<int32_t> move preserves
    // its heap allocation, but the OwnedOp record has moved.
    OwnedOp& landed = canvas->pending_ops.back();
    landed.op.aux     = landed.poly_verts.data();
    landed.op.aux_len = landed.poly_verts.size() * sizeof(int32_t);
    return SAO_STATUS_OK;
}

sao_status_t append_text(sao_ui_script_canvas_s* canvas,
                         int32_t x, int32_t y,
                         const char* text_utf8,
                         int32_t font_slot, int32_t font_size_px) {
    if (!canvas->draw_open) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (!can_append_op(canvas)) return SAO_STATUS_OK;
    OwnedOp owned{};
    owned.op.op = SAO_UI_CANVAS_OP_TEXT;
    owned.op.i[0] = x;
    owned.op.i[1] = y;
    owned.op.i[2] = font_size_px;
    owned.op.i[3] = font_slot;
    if (text_utf8 != nullptr) owned.text_utf8 = text_utf8;
    canvas->pending_ops.push_back(std::move(owned));
    // Same self-heal after move — text buffer moved with its owner.
    OwnedOp& landed = canvas->pending_ops.back();
    landed.op.aux     = landed.text_utf8.c_str();
    landed.op.aux_len = landed.text_utf8.size();
    return SAO_STATUS_OK;
}

}  // namespace

// ─── Canvas lifecycle ──────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_create(
    void* /*d3d_device_ptr*/,
    const SaoUiScriptCanvasSpec* spec,
    sao_ui_widget_handle_t* out_widget,
    sao_ui_script_canvas_handle_t* out_canvas) {

    if (out_widget != nullptr) *out_widget = nullptr;
    if (out_canvas != nullptr) *out_canvas = nullptr;
    if (spec == nullptr || out_canvas == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    auto* canvas = new sao_ui_script_canvas_s();
    canvas->spec = *spec;
    if (canvas->spec.max_ops_per_frame <= 0) {
        canvas->spec.max_ops_per_frame = 4000;  // parity with Python cap
    }
    // Synthetic widget handle — reinterpret-cast of the canvas
    // pointer, guaranteed unique per canvas.
    canvas->widget = reinterpret_cast<sao_ui_widget_handle_t>(canvas);

    *out_canvas = canvas;
    if (out_widget != nullptr) *out_widget = canvas->widget;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_script_canvas_destroy(
    sao_ui_script_canvas_handle_t canvas) {
    if (canvas != nullptr) {
        uint32_t removed = 0;
        (void)sao_ui_widget_release_event_handlers(canvas->widget, &removed);
    }
    delete canvas;  // nullptr-safe
}

// ─── Draw session ──────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_begin_draw(
    sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    canvas->pending_ops.clear();
    canvas->draw_open = true;
    canvas->begin_count += 1;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_submit_ops(
    sao_ui_script_canvas_handle_t canvas,
    const SaoUiCanvasOp* ops, size_t op_count) {

    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (op_count == 0) return SAO_STATUS_OK;
    if (ops == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    if (!canvas->draw_open) return SAO_STATUS_ERR_NOT_INITIALIZED;

    for (size_t i = 0; i < op_count; ++i) {
        if (!can_append_op(canvas)) break;  // silent drop
        OwnedOp owned{};
        owned.op = ops[i];
        // Copy aux content into owned storage when a plausible aux
        // shape is present, so the record survives past end_draw().
        const bool has_polygon_aux =
            ops[i].op == SAO_UI_CANVAS_OP_POLYGON &&
            ops[i].aux != nullptr && ops[i].aux_len > 0;
        const bool has_text_aux =
            ops[i].op == SAO_UI_CANVAS_OP_TEXT && ops[i].aux != nullptr;
        if (has_polygon_aux) {
            const auto* src = reinterpret_cast<const int32_t*>(ops[i].aux);
            const size_t words = ops[i].aux_len / sizeof(int32_t);
            owned.poly_verts.assign(src, src + words);
        } else if (has_text_aux) {
            owned.text_utf8.assign(
                reinterpret_cast<const char*>(ops[i].aux), ops[i].aux_len);
        }
        canvas->pending_ops.push_back(std::move(owned));
        // Self-heal the aux pointer after move so it references the
        // final storage inside pending_ops.back().
        OwnedOp& landed = canvas->pending_ops.back();
        if (has_polygon_aux) {
            landed.op.aux = landed.poly_verts.data();
            landed.op.aux_len =
                landed.poly_verts.size() * sizeof(int32_t);
        } else if (has_text_aux) {
            landed.op.aux     = landed.text_utf8.c_str();
            landed.op.aux_len = landed.text_utf8.size();
        } else {
            landed.op.aux     = nullptr;
            landed.op.aux_len = 0;
        }
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_end_draw(
    sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    if (!canvas->draw_open) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (canvas->spec.retain_ops_between_frames) {
        // Keep the last committed set if no new ops arrived (parity
        // with retain semantics documented in the header).
        if (!canvas->pending_ops.empty()) {
            canvas->committed_ops = std::move(canvas->pending_ops);
        }
    } else {
        canvas->committed_ops = std::move(canvas->pending_ops);
    }
    canvas->pending_ops.clear();
    canvas->draw_open = false;
    canvas->end_count += 1;
    canvas->invalidation_seq += 1;
    return SAO_STATUS_OK;
}

// ─── Draw-op shortcuts (13 draw ops, 15 state ops) ─────────────────

#define BEGIN_APPEND()                                                    \
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;        \
    std::lock_guard<std::mutex> guard(canvas->mu);                        \
    if (!canvas->draw_open) return SAO_STATUS_ERR_NOT_INITIALIZED;        \
    if (!can_append_op(canvas)) return SAO_STATUS_OK;                     \
    SaoUiCanvasOp op{};

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_line(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_LINE;
    op.i[0] = x1; op.i[1] = y1; op.i[2] = x2; op.i[3] = y2;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_rect(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_RECT;
    op.i[0] = x; op.i[1] = y; op.i[2] = w; op.i[3] = h;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_rounded_rect(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h, float radius) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_ROUNDED_RECT;
    op.i[0] = x; op.i[1] = y; op.i[2] = w; op.i[3] = h;
    op.f[0] = radius;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_oval(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_OVAL;
    op.i[0] = x; op.i[1] = y; op.i[2] = w; op.i[3] = h;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_polygon(
    sao_ui_script_canvas_handle_t canvas,
    const int32_t* verts_xy_pairs, size_t point_count) {
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return append_polygon(canvas, verts_xy_pairs, point_count);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_text(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y,
    const char* text_utf8, int32_t font_slot, int32_t font_size_px) {
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return append_text(canvas, x, y, text_utf8, font_slot, font_size_px);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_draw_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    int32_t bitmap_id,
    int32_t dst_x, int32_t dst_y, int32_t dst_w, int32_t dst_h) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_BITMAP;
    op.i[0] = dst_x; op.i[1] = dst_y; op.i[2] = dst_w; op.i[3] = dst_h;
    op.i[4] = bitmap_id;
    return append_simple(canvas, op);
}

// ─── State ops ─────────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_stroke_color(
    sao_ui_script_canvas_handle_t canvas, uint32_t argb) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SET_STROKE;
    op.i[0] = static_cast<int32_t>(argb);
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_fill_color(
    sao_ui_script_canvas_handle_t canvas, uint32_t argb) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SET_FILL;
    op.i[0] = static_cast<int32_t>(argb);
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_line_width(
    sao_ui_script_canvas_handle_t canvas, float width_px) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SET_LINE_W;
    op.f[0] = width_px;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_font(
    sao_ui_script_canvas_handle_t canvas,
    int32_t font_slot, int32_t font_size_px) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SET_FONT;
    op.i[0] = font_slot;
    op.i[1] = font_size_px;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_opacity(
    sao_ui_script_canvas_handle_t canvas, float opacity_0_to_1) {
    if (opacity_0_to_1 < 0.0f) opacity_0_to_1 = 0.0f;
    if (opacity_0_to_1 > 1.0f) opacity_0_to_1 = 1.0f;
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SET_OPACITY;
    op.f[0] = opacity_0_to_1;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_blend(
    sao_ui_script_canvas_handle_t canvas, int32_t mode) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SET_BLEND;
    op.i[0] = mode;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_translate(
    sao_ui_script_canvas_handle_t canvas, float dx, float dy) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_TRANSLATE;
    op.f[0] = dx;
    op.f[1] = dy;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_scale(
    sao_ui_script_canvas_handle_t canvas, float sx, float sy) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_SCALE;
    op.f[0] = sx;
    op.f[1] = sy;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_rotate_deg(
    sao_ui_script_canvas_handle_t canvas, float degrees) {
    BEGIN_APPEND();
    op.op   = SAO_UI_CANVAS_OP_ROTATE;
    op.f[0] = degrees;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_push_state(
    sao_ui_script_canvas_handle_t canvas) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_PUSH_STATE;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_pop_state(
    sao_ui_script_canvas_handle_t canvas) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_POP_STATE;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_clip(
    sao_ui_script_canvas_handle_t canvas,
    int32_t x, int32_t y, int32_t w, int32_t h) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_SET_CLIP;
    op.i[0] = x; op.i[1] = y; op.i[2] = w; op.i[3] = h;
    return append_simple(canvas, op);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_clear_clip(
    sao_ui_script_canvas_handle_t canvas) {
    BEGIN_APPEND();
    op.op = SAO_UI_CANVAS_OP_CLEAR_CLIP;
    return append_simple(canvas, op);
}

#undef BEGIN_APPEND

// ─── Bitmap registry ───────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_register_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    const void* bgra_pixels,
    uint32_t width, uint32_t height, uint32_t stride,
    int32_t* out_bitmap_id) {

    if (out_bitmap_id != nullptr) *out_bitmap_id = 0;
    if (canvas == nullptr || bgra_pixels == nullptr ||
        width == 0 || height == 0 || stride < width * 4) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(canvas->mu);
    BitmapRecord rec{};
    rec.width  = width;
    rec.height = height;
    rec.stride = stride;
    const size_t bytes = static_cast<size_t>(stride) * height;
    rec.pixels.assign(
        reinterpret_cast<const uint8_t*>(bgra_pixels),
        reinterpret_cast<const uint8_t*>(bgra_pixels) + bytes);
    const int32_t id = canvas->next_bitmap_id++;
    canvas->bitmaps.emplace(id, std::move(rec));
    if (out_bitmap_id != nullptr) *out_bitmap_id = id;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_update_bitmap(
    sao_ui_script_canvas_handle_t canvas,
    int32_t bitmap_id, const void* bgra_pixels,
    uint32_t width, uint32_t height, uint32_t stride) {

    if (canvas == nullptr || bgra_pixels == nullptr ||
        width == 0 || height == 0 || stride < width * 4) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> guard(canvas->mu);
    auto it = canvas->bitmaps.find(bitmap_id);
    if (it == canvas->bitmaps.end()) return SAO_STATUS_ERR_NOT_FOUND;
    BitmapRecord& rec = it->second;
    rec.width  = width;
    rec.height = height;
    rec.stride = stride;
    const size_t bytes = static_cast<size_t>(stride) * height;
    rec.pixels.assign(
        reinterpret_cast<const uint8_t*>(bgra_pixels),
        reinterpret_cast<const uint8_t*>(bgra_pixels) + bytes);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_unregister_bitmap(
    sao_ui_script_canvas_handle_t canvas, int32_t bitmap_id) {
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    auto it = canvas->bitmaps.find(bitmap_id);
    if (it == canvas->bitmaps.end()) return SAO_STATUS_ERR_NOT_FOUND;
    canvas->bitmaps.erase(it);
    return SAO_STATUS_OK;
}

// ─── Pointer input ─────────────────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_set_pointer_handler(
    sao_ui_script_canvas_handle_t canvas,
    sao_ui_script_canvas_pointer_cb_t callback, void* user_data) {

    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    canvas->pointer_cb = callback;
    canvas->pointer_ud = user_data;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_invalidate(
    sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);
    canvas->invalidation_seq += 1;
    return SAO_STATUS_OK;
}

// ─── Snapshot for dual renderer / tests ────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_snapshot_ops(
    sao_ui_script_canvas_handle_t canvas,
    SaoUiCanvasOp* out_ops, size_t capacity, size_t* out_written) {

    if (out_written != nullptr) *out_written = 0;
    if (canvas == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(canvas->mu);

    // Snapshot from committed if not in a draw session; else from
    // pending — this lets tests inspect ops mid-session.
    const auto& src = canvas->draw_open ? canvas->pending_ops
                                        : canvas->committed_ops;
    if (out_written != nullptr) *out_written = src.size();
    if (out_ops == nullptr) return SAO_STATUS_OK;
    const size_t to_copy = std::min(capacity, src.size());
    for (size_t i = 0; i < to_copy; ++i) {
        out_ops[i] = src[i].op;
    }
    if (src.size() > capacity) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    return SAO_STATUS_OK;
}

// ─── Test-only introspection ───────────────────────────────────────

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_script_canvas_bitmap_count(sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return 0;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return canvas->bitmaps.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_script_canvas_has_bitmap(sao_ui_script_canvas_handle_t canvas,
                                int32_t bitmap_id) {
    if (canvas == nullptr) return false;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return canvas->bitmaps.find(bitmap_id) != canvas->bitmaps.end();
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_script_canvas_pending_op_count(sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return 0;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return canvas->pending_ops.size();
}

extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_script_canvas_committed_op_count(
    sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return 0;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return canvas->committed_ops.size();
}

extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_script_canvas_invalidation_seq(
    sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return 0;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return canvas->invalidation_seq;
}

extern "C" SAO_UI_API bool SAO_UI_CALL
sao_ui_script_canvas_draw_open(sao_ui_script_canvas_handle_t canvas) {
    if (canvas == nullptr) return false;
    std::lock_guard<std::mutex> guard(canvas->mu);
    return canvas->draw_open;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_script_canvas_rasterize(
    sao_ui_script_canvas_handle_t canvas, sao_ui_offscreen_raster_handle_t raster,
    int32_t offset_x_px, int32_t offset_y_px) {
    if (canvas == nullptr || raster == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    struct PaintState {
        float translate_x{};
        float translate_y{};
        float scale_x{1.0F};
        float scale_y{1.0F};
        float opacity{1.0F};
        float line_width{1.0F};
        int32_t font_size{12};
        uint32_t stroke{0xffffffffU};
        uint32_t fill{0xffffffffU};
    };

    auto with_opacity = [](uint32_t argb, float opacity) {
        const uint32_t alpha = (argb >> 24U) & 0xffU;
        return (argb & 0x00ffffffU) | (static_cast<uint32_t>(std::lround(alpha * std::clamp(opacity, 0.0F, 1.0F))) << 24U);
    };
    auto point_x = [offset_x_px](const PaintState& state, float value) { return static_cast<float>(offset_x_px) + state.translate_x + value * state.scale_x; };
    auto point_y = [offset_y_px](const PaintState& state, float value) { return static_cast<float>(offset_y_px) + state.translate_y + value * state.scale_y; };

    sao_ui_paint_ctx_handle_t context = nullptr;
    sao_status_t status = sao_ui_paint_ctx_create_offscreen(raster, &context);
    if (status != SAO_STATUS_OK) return status;

    std::lock_guard<std::mutex> guard(canvas->mu);
    PaintState state{};
    std::vector<PaintState> state_stack;
    std::vector<std::pair<float, float>> path;
    bool has_clip = false;

    for (const OwnedOp& owned : canvas->committed_ops) {
        const SaoUiCanvasOp& op = owned.op;
        const auto fill = with_opacity(state.fill, state.opacity);
        const auto stroke = with_opacity(state.stroke, state.opacity);
        switch (op.op) {
        case SAO_UI_CANVAS_OP_BEGIN_PATH:
            path.clear();
            break;
        case SAO_UI_CANVAS_OP_LINE:
            sao_ui_paint_ctx_stroke_line(context, point_x(state, static_cast<float>(op.i[0])), point_y(state, static_cast<float>(op.i[1])), point_x(state, static_cast<float>(op.i[2])), point_y(state, static_cast<float>(op.i[3])), state.line_width, stroke);
            path.push_back({static_cast<float>(op.i[2]), static_cast<float>(op.i[3])});
            break;
        case SAO_UI_CANVAS_OP_RECT:
        case SAO_UI_CANVAS_OP_ROUNDED_RECT:
            sao_ui_paint_ctx_fill_rect(context, point_x(state, static_cast<float>(op.i[0])), point_y(state, static_cast<float>(op.i[1])), static_cast<float>(op.i[2]) * state.scale_x, static_cast<float>(op.i[3]) * state.scale_y, fill);
            break;
        case SAO_UI_CANVAS_OP_OVAL:
            sao_ui_paint_ctx_fill_ellipse(context, point_x(state, static_cast<float>(op.i[0])), point_y(state, static_cast<float>(op.i[1])), static_cast<float>(op.i[2]) * state.scale_x, static_cast<float>(op.i[3]) * state.scale_y, fill);
            break;
        case SAO_UI_CANVAS_OP_POLYGON: {
            const size_t points = std::min(static_cast<size_t>(std::max(0, op.i[0])), owned.poly_verts.size() / 2U);
            std::vector<int32_t> translated(points * 2U);
            for (size_t index = 0U; index < points; ++index) {
                translated[index * 2U] = static_cast<int32_t>(std::lround(point_x(state, static_cast<float>(owned.poly_verts[index * 2U]))));
                translated[index * 2U + 1U] = static_cast<int32_t>(std::lround(point_y(state, static_cast<float>(owned.poly_verts[index * 2U + 1U]))));
            }
            if (points >= 3U) sao_ui_paint_ctx_fill_polygon(context, translated.data(), points, fill);
            break;
        }
        case SAO_UI_CANVAS_OP_TEXT:
            sao_ui_paint_ctx_draw_utf8(context, point_x(state, static_cast<float>(op.i[0])), point_y(state, static_cast<float>(op.i[1])), owned.text_utf8.c_str(), static_cast<float>(op.i[2] > 0 ? op.i[2] : state.font_size), fill);
            break;
        case SAO_UI_CANVAS_OP_BITMAP: {
            const auto bitmap = canvas->bitmaps.find(op.i[4]);
            if (bitmap != canvas->bitmaps.end()) {
                const BitmapRecord& source = bitmap->second;
                sao_ui_paint_ctx_blit_premultiplied_bgra(context, source.pixels.data(), source.width, source.height, source.stride,
                    point_x(state, static_cast<float>(op.i[0])), point_y(state, static_cast<float>(op.i[1])), static_cast<float>(op.i[2]) * state.scale_x, static_cast<float>(op.i[3]) * state.scale_y);
            }
            break;
        }
        case SAO_UI_CANVAS_OP_ARC: {
            const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(op.f[1]) / 12.0F)));
            float previous_x = static_cast<float>(op.i[0]) + static_cast<float>(op.i[2]) * std::cos(op.f[0] * 3.14159265F / 180.0F);
            float previous_y = static_cast<float>(op.i[1]) + static_cast<float>(op.i[2]) * std::sin(op.f[0] * 3.14159265F / 180.0F);
            for (int index = 1; index <= segments; ++index) {
                const float angle = op.f[0] + op.f[1] * static_cast<float>(index) / static_cast<float>(segments);
                const float current_x = static_cast<float>(op.i[0]) + static_cast<float>(op.i[2]) * std::cos(angle * 3.14159265F / 180.0F);
                const float current_y = static_cast<float>(op.i[1]) + static_cast<float>(op.i[2]) * std::sin(angle * 3.14159265F / 180.0F);
                sao_ui_paint_ctx_stroke_line(context, point_x(state, previous_x), point_y(state, previous_y), point_x(state, current_x), point_y(state, current_y), state.line_width, stroke);
                previous_x = current_x;
                previous_y = current_y;
            }
            break;
        }
        case SAO_UI_CANVAS_OP_QUAD_CURVE:
        case SAO_UI_CANVAS_OP_CUBIC_CURVE: {
            const int segments = 16;
            float previous_x = static_cast<float>(op.i[0]);
            float previous_y = static_cast<float>(op.i[1]);
            for (int index = 1; index <= segments; ++index) {
                const float t = static_cast<float>(index) / static_cast<float>(segments);
                const float inverse = 1.0F - t;
                float current_x = 0.0F;
                float current_y = 0.0F;
                if (op.op == SAO_UI_CANVAS_OP_QUAD_CURVE) {
                    current_x = inverse * inverse * op.i[0] + 2.0F * inverse * t * op.i[2] + t * t * op.i[4];
                    current_y = inverse * inverse * op.i[1] + 2.0F * inverse * t * op.i[3] + t * t * op.i[5];
                } else {
                    current_x = inverse * inverse * inverse * op.i[0] + 3.0F * inverse * inverse * t * op.i[2] + 3.0F * inverse * t * t * op.i[4] + t * t * t * op.i_ex[0];
                    current_y = inverse * inverse * inverse * op.i[1] + 3.0F * inverse * inverse * t * op.i[3] + 3.0F * inverse * t * t * op.i[5] + t * t * t * op.i_ex[1];
                }
                sao_ui_paint_ctx_stroke_line(context, point_x(state, previous_x), point_y(state, previous_y), point_x(state, current_x), point_y(state, current_y), state.line_width, stroke);
                previous_x = current_x;
                previous_y = current_y;
            }
            break;
        }
        case SAO_UI_CANVAS_OP_STROKE_PATH:
            for (size_t index = 1U; index < path.size(); ++index) sao_ui_paint_ctx_stroke_line(context, point_x(state, path[index - 1U].first), point_y(state, path[index - 1U].second), point_x(state, path[index].first), point_y(state, path[index].second), state.line_width, stroke);
            break;
        case SAO_UI_CANVAS_OP_FILL_PATH: {
            std::vector<int32_t> points(path.size() * 2U);
            for (size_t index = 0U; index < path.size(); ++index) {
                points[index * 2U] = static_cast<int32_t>(std::lround(point_x(state, path[index].first)));
                points[index * 2U + 1U] = static_cast<int32_t>(std::lround(point_y(state, path[index].second)));
            }
            if (path.size() >= 3U) sao_ui_paint_ctx_fill_polygon(context, points.data(), path.size(), fill);
            break;
        }
        case SAO_UI_CANVAS_OP_PUSH_STATE: state_stack.push_back(state); break;
        case SAO_UI_CANVAS_OP_POP_STATE: if (!state_stack.empty()) { state = state_stack.back(); state_stack.pop_back(); } break;
        case SAO_UI_CANVAS_OP_TRANSLATE: state.translate_x += op.f[0]; state.translate_y += op.f[1]; break;
        case SAO_UI_CANVAS_OP_SCALE: state.scale_x *= op.f[0]; state.scale_y *= op.f[1]; break;
        case SAO_UI_CANVAS_OP_SET_STROKE: state.stroke = static_cast<uint32_t>(op.i[0]); break;
        case SAO_UI_CANVAS_OP_SET_FILL: state.fill = static_cast<uint32_t>(op.i[0]); break;
        case SAO_UI_CANVAS_OP_SET_LINE_W: state.line_width = std::max(0.5F, op.f[0]); break;
        case SAO_UI_CANVAS_OP_SET_FONT: state.font_size = std::max(1, op.i[1]); break;
        case SAO_UI_CANVAS_OP_SET_OPACITY: state.opacity = std::clamp(op.f[0], 0.0F, 1.0F); break;
        case SAO_UI_CANVAS_OP_SET_CLIP:
            if (has_clip) sao_ui_paint_ctx_pop_clip(context);
            sao_ui_paint_ctx_push_clip(context, point_x(state, static_cast<float>(op.i[0])), point_y(state, static_cast<float>(op.i[1])), static_cast<float>(op.i[2]) * state.scale_x, static_cast<float>(op.i[3]) * state.scale_y);
            has_clip = true;
            break;
        case SAO_UI_CANVAS_OP_CLEAR_CLIP:
            if (has_clip) sao_ui_paint_ctx_pop_clip(context);
            has_clip = false;
            break;
        default: break;
        }
    }
    if (has_clip) sao_ui_paint_ctx_pop_clip(context);
    sao_ui_paint_ctx_destroy(context);
    return SAO_STATUS_OK;
}
