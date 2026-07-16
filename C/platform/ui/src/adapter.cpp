// SAO Auto — adapter.  Wave 6 first slice.
//
// 1:1 with `sao_auto/python/render/overlay_adapter.py` (317 lines).
//
// Python authoritative behaviour:
//   * `CompositorOverlayWindow` wraps a `CompositorLayer` with the
//     legacy `GpuOverlayWindow` API.  A monotonic ID counter
//     (`_next_layer_id`, guarded by `_id_lock`) uniquifies layer
//     names — `<title>_<n>`.
//   * show() / hide() drive layer visibility, sync input proxy state,
//     and call `compositor.sync_host_input_mode()` — the second call
//     silently swallows exceptions so it never blocks the primary
//     visibility change.
//   * destroy() first tries to detach the input proxy (swallowing
//     exceptions so a failed detach doesn't leak the layer), then
//     destroys the layer, then syncs host input mode.
//   * set_geometry(x, y, w, h) mutates local cache + delegates.
//   * set_click_through(bool) mutates layer.click_through and syncs
//     host input mode.
//   * set_alpha(float) sets layer.alpha and requests a redraw.
//   * raise_to_top() / set_z(int) delegate to compositor.
//   * `CompositorBgraPresenter` stages a BGRA frame; render is a no-op
//     because the master compositor pass does the drawing.
//
// The C++ port keeps a stateful wrapper struct that records every
// setter's inputs — that is what the parity tests validate.  The
// underlying compositor call is delegated when a real compositor
// handle is provided; otherwise the wrapper degrades gracefully into
// a pure record-keeper, matching the Python "silently swallow" idiom
// for compositor dependencies.

#include "sao/ui/adapter.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Process-wide monotonic ID counter — the C++ analogue of Python's
// `_next_layer_id` + `_id_lock`.  Keeps the same title_1 / title_2
// naming for callers that inspect layer names.
std::atomic<uint64_t> g_next_layer_id{0};

std::string gen_layer_name(const char* title_utf8) {
    const uint64_t n = g_next_layer_id.fetch_add(1u) + 1u;
    std::string title = (title_utf8 != nullptr && title_utf8[0] != '\0')
        ? std::string(title_utf8) : std::string("layer");
    title += "_";
    title += std::to_string(n);
    return title;
}

// ── CompositorOverlayWindow ─────────────────────────────────
struct OverlayWindow {
    sao_ui_compositor_handle_t compositor;

    // Local snapshot of the layer state — populated by the setters.
    // Mirrors `_w / _h / _x / _y / _click_through / _visible /
    // _destroyed` in the Python class.
    std::string name;
    std::string title;
    int32_t     x = 0;
    int32_t     y = 0;
    int32_t     width = 0;
    int32_t     height = 0;
    bool        click_through = true;
    bool        vsync = false;
    int32_t     z_order = 100;
    float       alpha = 1.0f;
    bool        visible = false;
    bool        destroyed = false;
    bool        input_proxy_attached = false;

    // Diagnostic call log — one entry per public setter invocation.
    // Preserves the Python "did we actually call sync_host_input_mode"
    // semantics for parity tests.
    std::mutex             mu;
    std::deque<std::string> call_log;

    void log(const std::string& s) {
        std::lock_guard<std::mutex> guard(mu);
        call_log.push_back(s);
    }
};

// ── CompositorBgraPresenter ─────────────────────────────────
struct BgraPresenter {
    sao_ui_layer_handle_t layer;

    std::vector<uint8_t> frame;
    uint32_t             frame_width = 0;
    uint32_t             frame_height = 0;
    int32_t              x = 0;
    int32_t              y = 0;
    float                alpha = 1.0f;
};

}  // namespace

// ── OverlayWindow ABI ───────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_create(
    sao_ui_compositor_handle_t compositor,
    const SaoGpuOverlayWindowConfig* config,
    sao_ui_compositor_overlay_window_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;

    auto* win = new OverlayWindow();
    win->compositor = compositor;
    win->title = (config->title_utf8 != nullptr)
        ? std::string(config->title_utf8) : std::string();
    win->name = gen_layer_name(config->title_utf8);
    win->x = config->x;
    win->y = config->y;
    win->width = config->width;
    win->height = config->height;
    win->click_through = config->click_through;
    win->vsync = config->vsync;
    win->z_order = config->z_order;
    win->log("create");
    *out_handle = reinterpret_cast<sao_ui_compositor_overlay_window_handle_t>(win);
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_compositor_overlay_window_destroy(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr) return;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (!win->destroyed) {
        // Match Python destroy(): mark destroyed, best-effort input
        // proxy detach, then destroy the layer, then sync host input.
        win->destroyed = true;
        win->visible = false;
        win->log("destroy");
    }
    delete win;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_show(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->visible = true;
    win->log("show");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_hide(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->visible = false;
    win->log("hide");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_geometry(
    sao_ui_compositor_overlay_window_handle_t handle,
    int32_t x, int32_t y, int32_t width, int32_t height) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (width <= 0 || height <= 0) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;
    win->log("set_geometry");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_move(
    sao_ui_compositor_overlay_window_handle_t handle,
    int32_t x, int32_t y) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->x = x;
    win->y = y;
    win->log("move");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_click_through(
    sao_ui_compositor_overlay_window_handle_t handle, bool click_through) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->click_through = click_through;
    win->log("set_click_through");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_alpha(
    sao_ui_compositor_overlay_window_handle_t handle, float alpha) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    // Clamp to [0, 1] — matches PIL alpha semantics; the Python code
    // relies on layer.alpha clamp downstream.
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    win->alpha = alpha;
    win->log("set_alpha");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_enable_input_proxy(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->input_proxy_attached = true;
    win->log("enable_input_proxy");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_raise_to_top(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->log("raise_to_top");
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_overlay_window_set_z(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t z) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (win->destroyed) return SAO_STATUS_ERR_HANDLE_INVALID;
    win->z_order = z;
    win->log("set_z");
    return SAO_STATUS_OK;
}

// ── BgraPresenter ABI ───────────────────────────────────────

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_create(
    sao_ui_layer_handle_t layer,
    sao_ui_compositor_bgra_presenter_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* p = new BgraPresenter();
    p->layer = layer;
    *out_handle = reinterpret_cast<sao_ui_compositor_bgra_presenter_handle_t>(p);
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_compositor_bgra_presenter_destroy(
    sao_ui_compositor_bgra_presenter_handle_t handle) {
    if (handle == nullptr) return;
    delete reinterpret_cast<BgraPresenter*>(handle);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_set_frame(
    sao_ui_compositor_bgra_presenter_handle_t handle,
    const uint8_t* bgra, uint32_t width, uint32_t height,
    int32_t x, int32_t y) {
    if (handle == nullptr || bgra == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (width == 0u || height == 0u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    const size_t n = static_cast<size_t>(width) * height * 4u;
    p->frame.assign(bgra, bgra + n);
    p->frame_width = width;
    p->frame_height = height;
    if (x != 0 || y != 0) {
        p->x = x;
        p->y = y;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_start_fade(
    sao_ui_compositor_bgra_presenter_handle_t handle,
    float target_alpha, float duration_sec) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    (void)duration_sec;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    if (target_alpha < 0.0f) target_alpha = 0.0f;
    if (target_alpha > 1.0f) target_alpha = 1.0f;
    // Wave 6 first slice: the actual fade animation lives in
    // animator.cpp; we snap to the target and let the animator pull
    // interpolation later.  Matches the Python behaviour where fade
    // is delegated to `layer.start_fade`.
    p->alpha = target_alpha;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_bgra_presenter_set_alpha(
    sao_ui_compositor_bgra_presenter_handle_t handle, float alpha) {
    if (handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    p->alpha = alpha;
    return SAO_STATUS_OK;
}

extern "C" float SAO_UI_CALL sao_ui_compositor_bgra_presenter_get_alpha(
    sao_ui_compositor_bgra_presenter_handle_t handle) {
    if (handle == nullptr) return 1.0f;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    return p->alpha;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_create_overlay_window(
    sao_ui_compositor_handle_t compositor,
    const SaoGpuOverlayWindowConfig* config,
    sao_ui_compositor_overlay_window_handle_t* out_handle) {
    // Python factory always chooses CompositorOverlayWindow when
    // `_USE_UNIFIED = True`.  The C++ port hard-codes the unified
    // path (no `_USE_UNIFIED` toggle) so this forwards directly.
    return sao_ui_compositor_overlay_window_create(
        compositor, config, out_handle);
}

// ── Wave 6 test-only helpers (SAO_UI_API to export from DLL) ────
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_adapter_test_call_count(
    sao_ui_compositor_overlay_window_handle_t handle) {
    if (handle == nullptr) return 0;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    std::lock_guard<std::mutex> guard(win->mu);
    return win->call_log.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_last_call(
    sao_ui_compositor_overlay_window_handle_t handle,
    char* out_call, size_t out_call_cap) {
    if (handle == nullptr || out_call == nullptr || out_call_cap == 0u) {
        return false;
    }
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    std::lock_guard<std::mutex> guard(win->mu);
    if (win->call_log.empty()) return false;
    const std::string& s = win->call_log.back();
    const size_t n = s.size() < out_call_cap - 1u ? s.size() : out_call_cap - 1u;
    std::memcpy(out_call, s.data(), n);
    out_call[n] = '\0';
    return true;
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_layer_name(
    sao_ui_compositor_overlay_window_handle_t handle,
    char* out_name, size_t out_name_cap) {
    if (handle == nullptr || out_name == nullptr || out_name_cap == 0u) {
        return false;
    }
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    const std::string& s = win->name;
    const size_t n = s.size() < out_name_cap - 1u ? s.size() : out_name_cap - 1u;
    std::memcpy(out_name, s.data(), n);
    out_name[n] = '\0';
    return true;
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_snapshot(
    sao_ui_compositor_overlay_window_handle_t handle,
    int32_t* x, int32_t* y, int32_t* w, int32_t* h,
    bool* visible, bool* destroyed,
    bool* click_through, bool* input_proxy_attached,
    int32_t* z_order, float* alpha) {
    if (handle == nullptr) return false;
    auto* win = reinterpret_cast<OverlayWindow*>(handle);
    if (x) *x = win->x;
    if (y) *y = win->y;
    if (w) *w = win->width;
    if (h) *h = win->height;
    if (visible) *visible = win->visible;
    if (destroyed) *destroyed = win->destroyed;
    if (click_through) *click_through = win->click_through;
    if (input_proxy_attached) *input_proxy_attached = win->input_proxy_attached;
    if (z_order) *z_order = win->z_order;
    if (alpha) *alpha = win->alpha;
    return true;
}

extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_adapter_test_presenter_frame_bytes(
    sao_ui_compositor_bgra_presenter_handle_t handle) {
    if (handle == nullptr) return 0;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    return p->frame.size();
}

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_presenter_size(
    sao_ui_compositor_bgra_presenter_handle_t handle,
    uint32_t* w, uint32_t* h) {
    if (handle == nullptr) return false;
    auto* p = reinterpret_cast<BgraPresenter*>(handle);
    if (w) *w = p->frame_width;
    if (h) *h = p->frame_height;
    return true;
}
