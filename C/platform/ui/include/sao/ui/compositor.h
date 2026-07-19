// SAO Auto — virtual-layer compositor.  1:1 with `render/overlay_compositor.py`.
//
// Python authoritative source: `sao_auto/python/render/overlay_compositor.py`
//                              (3460 lines) + `render/overlay_adapter.py` (317 lines)
//
// Successor to the legacy `render/gpu_compositor.py` design.  The
// compositor owns z-order, mouse routing between virtual layers,
// SetWindowRgn accumulation that governs cross-process click-through,
// and the GL master pass that flattens all layer textures into the
// single DWM composition tree owned by `overlay_host.h`.
//
// ── Layer content sources (see §8 of the handoff) ─────────────────
//   1. `update_bgra(bytes, w, h)`  — Any thread (Python C# plugin
//      bridge).  Thread-safe.  Original-Python bug (already-fixed):
//      three separate fields for bytes/w/h can tear cross-thread
//      producing "old buffer + new dims" → Cython scan OOB.  The C++
//      port uses an atomic snapshot triple (bytes*, w, h, seq).
//   2. `set_mmf_source(name)` — Zero-copy memory-mapped-file reader
//      (pet engine BGRA slots).  Render-thread only.
//   3. `set_shared_texture_source(handle, w, h)` — GPU-shared D3D11
//      texture; producer writes straight (non-premultiplied) RGBA,
//      consumer premultiplies in the shader.  Any-thread setter,
//      render-thread consumer.  MMF + shared texture CAN coexist on
//      one layer — shared texture provides color, MMF provides alpha
//      byte for RGN scanning.
//
// ── SetWindowRgn — the click-through mechanism (§4-5 of handoff) ──
//   `SetWindowRgn`'s **exclude** is THE only reliable cross-process
//   passthrough.  Host RGN never NULL.  Three layers of padding
//   applied to the accumulated span set (host coords):
//     _RGN_PAD_STILL     = 0    (static frame: exact pixel accuracy)
//     _RGN_PAD_MOVE      = f()  (moving layer: 2× velocity floor)
//     _RGN_PAD_ANIM_MIN..CAP    (animating content: measured contour)
//     _RGN_STATIC_SETTLE = N ticks  after content settles before
//                                  collapsing back to exact edges
//   Temporal union: a dirty tick emits BOTH the current padded spans
//   AND last tick's spans (`_rgn_union_prev`) — one covers "new region
//   over stale pixels", the other covers "stale region over new pixels".
//   Constants were tightened ~25% (32→24 / 8→6 / 128→96) in the
//   Python revamp; the C++ port keeps the tightened values.
//
// ── Layer name-reuse leak (§7 of handoff, mandatory guard) ────────
//   Original Python `create_layer(name)` unconditionally overwrote
//   `_layers[name]` — reuse via a fixed name like "_motion_blur"
//   during rapid open/close leaked GL FBOs forever.  The C++ port
//   FAILS layer_create() with SAO_STATUS_ERR_ALREADY_EXISTS instead
//   of silently overwriting.  Callers that want reuse must destroy
//   first — the destroy path is what queues _release_gl() into the
//   render thread's command queue.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/overlay_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_compositor_s* sao_ui_compositor_handle_t;
typedef struct sao_ui_layer_s* sao_ui_layer_handle_t;

#define SAO_UI_SOPF_MMF_MAGIC 0x46504F53u
#define SAO_UI_SOPF_MMF_VERSION_V1 1u
#define SAO_UI_SOPF_MMF_VERSION_V2 2u
#define SAO_UI_SOPF_MMF_HEADER_BYTES 64u
#define SAO_UI_SOPF_MMF_V1_MIN_SLOT_COUNT 1u
#define SAO_UI_SOPF_MMF_V2_MIN_SLOT_COUNT 2u
#define SAO_UI_SOPF_MMF_MAX_SLOT_COUNT 8u
#define SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES 8u
#define SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES (512ull * 1024ull * 1024ull)

// SOPF v1 preserves the Python authority's legacy read-slot rule:
// (published_slot + slot_count - 1) % slot_count. published_generation may
// start at zero and is only a change detector; v1 has no per-slot footer.
typedef struct SaoUiSopfMmfHeaderV1 {
    uint32_t magic;
    uint32_t version;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t slot_count;
    uint32_t slot_stride;
    uint64_t published_generation;
    uint32_t published_slot;
    uint8_t reserved[28];
} SaoUiSopfMmfHeaderV1;

// SOPF v2 publishes latest_completed_slot directly at offset 32. A frame is
// consumable only when published_generation is a non-zero even value and the
// selected slot's trailing footer contains the same generation before and
// after payload copy. Odd/zero/torn markers are transient, not cache clears.
typedef struct SaoUiSopfMmfHeaderV2 {
    uint32_t magic;
    uint32_t version;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t slot_count;
    uint32_t slot_stride;
    uint64_t published_generation;
    uint32_t latest_completed_slot;
    uint32_t header_bytes;
    uint8_t reserved[24];
} SaoUiSopfMmfHeaderV2;

typedef struct SaoUiSopfMmfSlotFooterV2 {
    uint64_t slot_generation;
} SaoUiSopfMmfSlotFooterV2;

#if defined(__cplusplus)
static_assert(sizeof(SaoUiSopfMmfHeaderV1) == SAO_UI_SOPF_MMF_HEADER_BYTES);
static_assert(sizeof(SaoUiSopfMmfHeaderV2) == SAO_UI_SOPF_MMF_HEADER_BYTES);
static_assert(sizeof(SaoUiSopfMmfSlotFooterV2) ==
              SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES);
static_assert(offsetof(SaoUiSopfMmfHeaderV1, published_generation) == 24u);
static_assert(offsetof(SaoUiSopfMmfHeaderV1, published_slot) == 32u);
static_assert(offsetof(SaoUiSopfMmfHeaderV2, published_generation) == 24u);
static_assert(offsetof(SaoUiSopfMmfHeaderV2, latest_completed_slot) == 32u);
static_assert(offsetof(SaoUiSopfMmfHeaderV2, header_bytes) == 36u);
#endif

// Layer-local logical input rectangle. These rectangles override visual-alpha
// scanning when present, so translucent decoration can remain visible without
// expanding the cross-process SetWindowRgn hit surface.
struct SaoUiLayerInputRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

// Compositor creation config.
struct SaoCompositorConfig {
    // Refresh-rate cap.  0 → auto-detect via GetDeviceCaps(VREFRESH),
    // clamped 60..240 Hz.  Fallback 60 on failure (matches
    // `overlay_compositor._detect_refresh_hz`).
    int32_t     target_hz;

    // Enable temporal-union RGN skew cover.  True by default;
    // False for debugging RGN issues (falls back to single-frame spans).
    bool        enable_temporal_union;

    // Enable per-layer RGN cache (avoid re-scanning unchanged frames).
    // True by default.
    bool        enable_rgn_cache;
};

// Layer creation config — one struct so extension fields don't ripple
// through 12 argument overloads.
struct SaoLayerConfig {
    // Unique across the process.  Layer reuse FAILS (see the leak
    // note above); caller must destroy an existing layer first.
    const char* name_utf8;

    int32_t     x;
    int32_t     y;
    int32_t     width;
    int32_t     height;

    // Draw order — larger z draws on top.  Layer z-order can be
    // changed after creation via `set_z_order`.
    int32_t     z_order;

    // If true, this layer's pixels never contribute to the host's
    // click-through RGN — the mouse falls through even over opaque
    // sprite pixels.  Interactive layers (Tk mirror panels, plugin
    // main UI) set this false; decoration layers (pet halo, HUD)
    // set this true.
    bool        click_through;

    // If true, `_sync_host_rgn` uses this layer's whole bounding
    // rect for the host click region instead of scanning per-pixel
    // alpha.  Solid rectangular interactive layers (Tk mirror
    // panels) set this: their PrintWindow capture has no reliable
    // alpha byte for native child controls, so alpha-span scanning
    // would punch holes over exactly Entry/Text widgets and make
    // them unclickable.  Irregular sprite layers leave this false.
    bool        rect_hit;

    // BGRA byte order (framework-native) vs raw RGBA (shared
    // texture case).  Selects the fragment shader used at composite.
    bool        bgra_swizzle;

    // If true, layer target FPS ignores idle throttling.  Used by
    // the desktop pet during eye-catching animations.
    bool        high_fps;

    // Explicit per-layer target FPS.  0 → follow global cadence.
    // Used to run low-priority layers (idle HP bar) at 30 Hz while
    // the pet's spring animation runs at 60/144 Hz.
    int32_t     target_fps;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_create(
    sao_ui_overlay_host_handle_t host,
    const SaoCompositorConfig* config,
    sao_ui_compositor_handle_t* out_handle);

// Render-thread-affine. A call from any other thread is a safe no-op: the
// handle remains valid and must be destroyed again on the thread that created
// the compositor. A call made reentrantly from a render/fade callback during
// present is also a safe no-op and must be retried after present returns.
// Before the owner-thread call, the caller must stop and join every concurrent
// compositor/layer API user. The owner-thread call flushes both active and
// pending layer resources before releasing compositor COM.
SAO_UI_API void SAO_UI_CALL sao_ui_compositor_destroy(
    sao_ui_compositor_handle_t handle);

// Borrowed production host and its HWND.  Headless compositors return NULL.
SAO_UI_API sao_ui_overlay_host_handle_t SAO_UI_CALL sao_ui_compositor_host(
    sao_ui_compositor_handle_t handle);

SAO_UI_API void* SAO_UI_CALL sao_ui_compositor_host_hwnd(
    sao_ui_compositor_handle_t handle);

// ── Layer lifecycle ─────────────────────────────────────────────

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_create(
    sao_ui_compositor_handle_t compositor,
    const SaoLayerConfig* config,
    sao_ui_layer_handle_t* out_layer);

// Safe on null and callable from any thread. A non-render-thread call removes
// the layer from the active set immediately and queues its resource release;
// every subsequent mutator on the old handle returns
// SAO_STATUS_ERR_HANDLE_INVALID, including after pending resources have been
// flushed. The name may be recreated immediately. The next render-thread
// present, or owner-thread compositor destroy after all API users are
// quiesced, releases pending graphics and CPU payloads. Lightweight detached
// handle shells stay valid only for rejection until compositor teardown;
// never retain/use a layer handle after its owning compositor is destroyed.
SAO_UI_API void SAO_UI_CALL sao_ui_layer_destroy(sao_ui_layer_handle_t layer);

// ── Layer content sources ──────────────────────────────────────

// Path 1: push premultiplied BGRA bytes.  Thread-safe.  Rejects a
// (w,h) that doesn't match `len(bgra)`; a mismatch means the caller
// raced their own dimension state against an in-flight capture —
// storing it would crash the next frame with an OOB texture upload.
// Geometry and existing logical input rectangles are validated under the
// owner mutex against the current layer state. Failure preserves the complete
// previous snapshot and logical geometry. Source buffers above the compositor
// byte budget are rejected before copying caller memory.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_update_bgra(
    sao_ui_layer_handle_t layer,
    const uint8_t* bgra_pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride);

// Path 2: attach a named Win32 file mapping containing a SOPF v1/v2 ring.
// Public layouts/constants above are the producer contract. The consumer
// validates the 64-byte header before mapping the checked full ring, then
// atomically replaces its BGRA cache only with a stable complete slot.
// Passing NULL / "" clears the source and generation state.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_mmf_source(
    sao_ui_layer_handle_t layer,
    const char* mmf_name_utf8);

// Path 3: attach a GPU-shared D3D11 texture as color source.  Handle
// is an NT shared handle (from CreateSharedHandle) or a DXGI keyed-
// mutex handle.  See `dcomp_bridge.h` for the interop story.  The
// texture holds STRAIGHT (non-premultiplied) RGBA; the composite
// shader multiplies alpha through — doing premultiply upstream would
// cost a CPU pass or extra plugin-side shader work.
// handle == 0 → clear the source immediately on the render thread.
// MMF source (if attached) keeps being polled purely
// for its alpha byte, feeding _sync_host_rgn. Attach/clear is render-thread-
// affine because it may release cached D3D11 objects; update_bgra remains
// thread-safe.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_shared_texture(
    sao_ui_layer_handle_t layer,
    void* shared_handle,     // HANDLE
    uint32_t width, uint32_t height);

// Set an FBO-render callback.  Layer creates a private FBO + texture
// on the render thread; on each frame `fn(gl_ctx, time_sec)` runs
// with the FBO bound, then the FBO texture is composited into the
// master pass.  Used by plugin GPU widgets and the fisheye layer.
// Passing NULL clears.
typedef void (SAO_UI_CALL* sao_ui_layer_render_fn_t)(
    void* gl_ctx, float time_sec, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_render_fn(
    sao_ui_layer_handle_t layer,
    sao_ui_layer_render_fn_t fn, void* user_data);

// ── Layer geometry / visibility / alpha ───────────────────────

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_position(
    sao_ui_layer_handle_t layer, int32_t x, int32_t y);

// Geometry cannot shrink below an existing BGRA/shared source or logical input
// rectangle. Invalid updates leave all prior layer state unchanged.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_geometry(
    sao_ui_layer_handle_t layer,
    int32_t x, int32_t y, int32_t width, int32_t height);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_z_order(
    sao_ui_layer_handle_t layer, int32_t z_order);

// Show/hide. Both transitions mark the layer dirty. Hide MUST open the render
// gate — the vacated screen area only gets repainted if a frame renders after
// the hide.
// Without this, hiding while nothing else animates leaves ghost
// residue on screen until some other layer goes dirty.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_visible(
    sao_ui_layer_handle_t layer, bool visible);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_alpha(
    sao_ui_layer_handle_t layer, float alpha);

// Animate alpha towards target over duration_sec. Alpha and duration inputs
// must be finite. done_fn fires
// on the render thread when the fade completes (or is interrupted
// by a second call).  fade_from is captured as the current alpha
// at call time; caller need not query it.
typedef void (SAO_UI_CALL* sao_ui_layer_fade_done_fn_t)(void* user_data);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_start_fade(
    sao_ui_layer_handle_t layer,
    float target_alpha,
    float duration_sec,
    sao_ui_layer_fade_done_fn_t done_fn, void* user_data);

// Toggle whether the layer participates in mouse routing.  A layer
// that opts out is drawn but its pixels don't count towards the
// click-through RGN.  Different from set_visible: this is about
// clicks, not paint.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_input_enabled(
    sao_ui_layer_handle_t layer, bool enabled);

// Override visual-alpha hit scanning with layer-local logical rectangles.
// Passing NULL with count 0 clears the override and restores the layer's
// rect_hit / BGRA-alpha behavior. Rectangles must fit inside layer bounds.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_input_rects(
    sao_ui_layer_handle_t layer,
    const SaoUiLayerInputRect* rects,
    size_t count);

// Request an explicit redraw of this layer next tick.  Layers with
// FBO callbacks and no upload_bgra do not otherwise trigger a render.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_request_redraw(
    sao_ui_layer_handle_t layer);

// ── Layer input callbacks (interactive layers) ────────────────

typedef void (SAO_UI_CALL* sao_ui_layer_cursor_pos_fn_t)(
    float layer_x, float layer_y, void* user_data);
typedef void (SAO_UI_CALL* sao_ui_layer_cursor_leave_fn_t)(void* user_data);
typedef void (SAO_UI_CALL* sao_ui_layer_button_fn_t)(
    int32_t button, int32_t action, int32_t mods,
    float layer_x, float layer_y, void* user_data);
typedef void (SAO_UI_CALL* sao_ui_layer_scroll_fn_t)(
    float dx, float dy, void* user_data);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_set_input_callbacks(
    sao_ui_layer_handle_t layer,
    sao_ui_layer_cursor_pos_fn_t cursor_pos_fn,
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn,
    sao_ui_layer_button_fn_t button_fn,
    sao_ui_layer_scroll_fn_t scroll_fn,
    void* user_data);

// Route this interactive layer's input via a per-layer input proxy
// (Tk-toplevel-style invisible window with LWA_COLORKEY hit shape)
// so the host stays click-through everywhere.  See
// `overlay_compositor.py::_proxy_shield_activation` and its long
// commentary on the WndProc-chain-of-death (never subclass twice —
// one shot per proxy or the CallWindowProc stack overflow-crashes
// the process).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_enable_input_proxy(
    sao_ui_layer_handle_t layer);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_layer_disable_input_proxy(
    sao_ui_layer_handle_t layer);

// ── Compositor-level operations ────────────────────────────────

// Composite + present. Drops frames when the host is minimised. Render and
// fade callbacks are isolated individually: all scheduled callbacks run even
// when one throws, no exception crosses the C ABI, and the final status is
// SAO_STATUS_ERR_UNKNOWN if any callback failed and the underlying present
// otherwise succeeded. Device/upload/recreate failures take priority.
// Returns SAO_STATUS_ERR_DEVICE_LOST on TDR — caller tears down
// bridge + recreates.  Called every frame from the render thread.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_present(
    sao_ui_compositor_handle_t compositor);

// Compose the current visible layers into a caller-owned premultiplied BGRA
// snapshot without requiring an overlay host or a DirectComposition target.
// This uses the same production composition path as `present`.  Pass NULL
// with capacity 0 to query dimensions and the required byte count.  A
// non-null buffer that is too small returns SAO_STATUS_ERR_BUFFER_TOO_SMALL
// after writing the required dimensions and byte count.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_snapshot_bgra(
    sao_ui_compositor_handle_t compositor,
    uint8_t* out_bgra_pixels,
    size_t capacity,
    uint32_t* out_width,
    uint32_t* out_height,
    size_t* out_bytes);

// Enforce host z-order on the compositor render thread. Route ALL topmost changes through this
// (which delegates to `z_order.h`) — never SetWindowPos directly.
// See §3 of the handoff — the reason this function exists is to be
// the SOLE authority.  Called from the frame loop; also whenever
// layer visibility changes (a fisheye popup may need to bring host
// above the game briefly).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_enforce_z_order(
    sao_ui_compositor_handle_t compositor);

// Rebuild the host SetWindowRgn from the current layer set + input-
// proxy shapes.  Called every frame after the layer composition
// pass — never mid-composition (would race).  Bug §5 of handoff:
// SetWindowRgn ctypes overflow bug is fixed in the C++ port by
// using HRGN (void*) with correct signature. Render-thread-affine;
// cross-thread calls return SAO_STATUS_ERR_ACCESS_DENIED.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_sync_host_rgn(
    sao_ui_compositor_handle_t compositor);

// Lift all layer input proxies above the host (z-order) so they
// keep capturing input after the host does its own SetWindowPos.
// Idempotent.  Called after `set_visible(true)` on any interactive
// layer.  Never issues a real WS_EX_TOPMOST — the proxies use
// HWND_TOP only, matching §3 of the handoff.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_lift_input_proxies(
    sao_ui_compositor_handle_t compositor);

// Toggle the host's WS_EX_TRANSPARENT based on the union of layer
// click_through state.  If ANY visible layer is non-click-through
// (has an active input proxy), the host must NOT be WS_EX_TRANSPARENT
// or the proxy region logic breaks. Render-thread-affine and paired
// with sync_host_rgn by the caller.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_sync_host_input_mode(
    sao_ui_compositor_handle_t compositor);

// Enumerate layers (owned iterator — do NOT release).  Used by
// diagnostic tooling.  Returns the count regardless of whether
// out_layers[] is large enough; pass NULL to just query count.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_compositor_list_layers(
    sao_ui_compositor_handle_t compositor,
    sao_ui_layer_handle_t* out_layers, size_t capacity,
    size_t* out_count);

#ifdef __cplusplus
}  // extern "C"
#endif
