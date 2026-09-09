// SAO Auto — virtual-layer compositor implementation.
//
// UI surface capability matrix
// Surface                         Z-order                 Input routing          Theme                 Raster/composition       Capture              Presentation
// panel.cpp                       layer delegate          panel-local actions    theme consumer        panel-local raster       none                 none
// panel_sdk.cpp                   panel class policy      runtime delegate       override owner        runtime delegate         none                 none
// panel_layout.cpp                none                    hit geometry only      none                  layout only              none                 none
// theme.cpp                       none                    none                   sole resolver         none                     none                 none
// compositor.cpp                  host/layer authority    cross-layer authority none                  sole composition         composition only     none
// overlay_host.cpp                OS apply only           Win32 event source     none                  none                     affinity policy only none
// gpu_overlay_window.cpp          frozen delegate         frozen delegate        none                  layer producer           none                 none
// adapter.cpp                     frozen delegate         frozen delegate        none                  diagnostics mirror only  none                 none
// legacy_webview_stub.cpp         none                    none                   none                  none                     none                 none
// sao_ui_scriptable_canvas.cpp    none                    local callback source  theme consumer        widget-local raster      none                 none
// capture_sync.cpp                none                    none                   none                  calls compositor         sole capture owner   none
// dcomp_bridge.cpp                none                    none                   none                  accepts composed BGRA    none                 sole presenter
//
// The former overlaps were the panel.cpp registry, scattered fallback
// colours, compositor-owned capture wrapping, and adapter frame mirrors.
// Registration now routes through panel_sdk, semantic lookup through theme,
// capture through capture_sync, and compatibility mirrors are non-authoritative.
// SAO Auto - compositor layer ownership and production presentation paths.
//
// The layer bookkeeping contract is defined by
// `include/sao/ui/compositor.h`.  Core lifecycle operations include:
//
//   * `sao_ui_compositor_create`      - allocate an opaque handle, keep a
//                                       vector of layers, snapshot config.
//   * `sao_ui_compositor_destroy`     - release the vector + handle.
//   * `sao_ui_layer_create`           - reject duplicate name with
//                                       SAO_STATUS_ERR_ALREADY_EXISTS
//                                       (header rule "§7 layer name-reuse
//                                       leak").  Otherwise allocate + push.
//   * `sao_ui_layer_destroy`          - remove from the owning vector.
//   * `sao_ui_layer_set_z_order`      - update z + stable-sort the vector.
//   * `sao_ui_layer_set_visible`      - flip the visibility flag.
//   * `sao_ui_compositor_list_layers` - enumerate; NULL out_layers[] gives
//                                       a size-query.  Task brief's
//                                       "get_layer_count" idiom.
//
// UTF-8 no BOM.  static_asserts guard the invariants the header banner
// documents.

#include "sao/ui/compositor.h"
#include "sao/ui/d2d_effects.h"
#include "sao/ui/d3d11_device.h"
#include "sao/ui/dcomp_bridge.h"
#include "sao/ui/z_order.h"

#include "d2d_effects_internal.h"

#include "input_router_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d11.h>
#  include <dxgi.h>
#include "sao_ui_compositor_master_ps.h"
#include "sao_ui_compositor_master_vs.h"
#endif

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

// SaoLayerConfig layout stability -- reject accidental reorder. As of ABI
// minor 8, struct_size is the first field so the platform can safely read
// the caller's declared size before dereferencing later fields. name_utf8
// sits at 16 (struct_size 0..3 + _reserved0 4..7 + 8-byte pointer align).
static_assert(offsetof(SaoLayerConfig, struct_size) == 0,
              "SaoLayerConfig.struct_size must be first field");
static_assert(offsetof(SaoLayerConfig, name_utf8) == 2 * sizeof(uint32_t),
              "SaoLayerConfig.name_utf8 must immediately follow the ABI header");
static_assert(offsetof(SaoLayerConfig, x) ==
                  offsetof(SaoLayerConfig, name_utf8) + sizeof(void*),
              "SaoLayerConfig.x must immediately follow name_utf8");
// Guard the z_order field position (used by set_z_order semantics).
static_assert(offsetof(SaoLayerConfig, z_order)
              == offsetof(SaoLayerConfig, x) + sizeof(int32_t) * 4,
              "SaoLayerConfig.z_order offset drifted");

// Handles must be pointer-width -- caller uses them as opaque tokens.
static_assert(sizeof(sao_ui_compositor_handle_t) == sizeof(void*),
              "compositor handle must be pointer-width");
static_assert(sizeof(sao_ui_layer_handle_t) == sizeof(void*),
              "layer handle must be pointer-width");

// ---------------------------------------------------------------------------
// Internal types.
// ---------------------------------------------------------------------------

struct PendingFadeCall {
    sao_ui_layer_fade_done_fn_t fn{nullptr};
    void* user{nullptr};
};

struct sao_ui_layer_s;
struct sao_ui_compositor_s;

template <typename T>
concept CompleteType = requires { sizeof(T); };

static_assert(!CompleteType<sao::ui::input_router_detail::LayerInputState>);

struct InputCallbackInvocation {
    enum class Kind { cursor, leave, button, scroll } kind{};
    sao_ui_compositor_s* compositor{};
    sao_ui_layer_s* layer{};
    uint64_t generation{};
    void* user_data{};
    sao_ui_layer_cursor_pos_fn_t cursor{};
    sao_ui_layer_cursor_leave_fn_t leave{};
    sao_ui_layer_button_fn_t button{};
    sao_ui_layer_scroll_fn_t scroll{};
    float x{};
    float y{};
    float scroll_dx{};
    float scroll_dy{};
    int32_t button_id{-1};
    int32_t action{};
};

struct PostInputTask {
    sao_ui_compositor_post_input_fn_t fn{nullptr};
    void* user{nullptr};
};

struct sao_ui_layer_s {
    std::string name;                // owning copy (config's name_utf8 is caller memory).
    int32_t     x{0};
    int32_t     y{0};
    int32_t     width{0};
    int32_t     height{0};
    int32_t     z_order{0};
    bool        click_through{true};
    bool        rect_hit{false};
    bool        bgra_swizzle{false};
    bool        high_fps{false};
    int32_t     target_fps{0};
    bool        visible{true};
    float       alpha{1.0f};
    bool        input_enabled{true};
    std::vector<SaoUiLayerInputRect> input_rects;
    std::vector<uint8_t> bgra_pixels;
    uint32_t    bgra_width{0};
    uint32_t    bgra_height{0};
    uint32_t    bgra_stride{0};
    bool        bgra_dirty{false};
    uint64_t    visual_revision{0};
    // ── RGN sync state (Python authority: overlay_compositor.py
    // _rgn_cache_key/_rgn_cached_spans/_rgn_union_prev/_rgn_emit_spans/
    // _rgn_static_ticks).  Cached spans are LAYER-LOCAL and keyed only on
    // visual_revision so a pure translation never forces a rescan.
    uint64_t    rgn_cache_revision{UINT64_MAX};
    std::vector<SaoOverlayHostInputRect> rgn_cached_spans;
    std::vector<SaoOverlayHostInputRect> rgn_union_prev;
    std::vector<SaoOverlayHostInputRect> rgn_emit_spans;
    uint32_t    rgn_static_ticks{0};
    int32_t     rgn_prev_x{std::numeric_limits<int32_t>::min()};
    int32_t     rgn_prev_y{std::numeric_limits<int32_t>::min()};
    std::string mmf_name;
    uint64_t    mmf_last_generation{0};
    bool        mmf_has_last_generation{false};
    // Reconnect miss counter. Reset to 0 on any successful frame publish or
    // structural fail-closed reset; bumped on each OpenFileMappingA /
    // MapViewOfFile miss so the reconnect path can retain the last-good
    // frame and diagnostics can observe how long a producer has been away.
    uint32_t    mmf_reconnect_attempts{0};
    void*       shared_handle{nullptr};
    uint32_t    shared_width{0};
    uint32_t    shared_height{0};
#if defined(_WIN32)
    ID3D11Texture2D* shared_texture{nullptr};
    ID3D11Texture2D* shared_staging{nullptr};
    IDXGIKeyedMutex* shared_keyed_mutex{nullptr};
#endif
    sao_ui_layer_render_fn_t render_fn{nullptr};
    void*       render_user_data{nullptr};
    sao_ui_layer_d3d11_render_fn_t d3d11_render_fn{nullptr};
    void* d3d11_render_user_data{nullptr};
#if defined(_WIN32)
    ID3D11Texture2D* gpu_texture{nullptr};
    ID3D11RenderTargetView* gpu_rtv{nullptr};
    ID3D11ShaderResourceView* gpu_srv{nullptr};
    uint32_t gpu_width{0};
    uint32_t gpu_height{0};
    uint64_t gpu_uploaded_revision{UINT64_MAX};
#endif
    bool        redraw_requested{false};
    bool        fade_active{false};
    float       fade_from{1.0f};
    float       fade_target{1.0f};
    float       fade_duration_sec{0.0f};
    std::chrono::steady_clock::time_point fade_started{};
    sao_ui_layer_fade_done_fn_t fade_done_fn{nullptr};
    void*       fade_done_user_data{nullptr};
    sao_ui_layer_cursor_pos_fn_t cursor_pos_fn{nullptr};
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn{nullptr};
    sao_ui_layer_button_fn_t button_fn{nullptr};
    sao_ui_layer_scroll_fn_t scroll_fn{nullptr};
    void*       input_user_data{nullptr};
    uint64_t    input_callback_generation{1};
    size_t      input_callbacks_in_flight{0};
    std::unordered_map<uint64_t, size_t> input_callbacks_by_generation;
    bool        detached_payload_released{false};
    bool        input_proxy_enabled{false};
    SaoUiLayerEffects effects{};

    // Back-pointer to the owning compositor -- used by
    // sao_ui_layer_destroy(layer) which does not receive the compositor.
    struct sao_ui_compositor_s* owner{nullptr};

    // Insertion sequence for stable ordering across equal z values.
    uint64_t    creation_seq{0};
};

struct sao_ui_compositor_s {
    sao_ui_overlay_host_handle_t host{nullptr};
    SaoCompositorConfig          config{};
    // Own the layers by unique_ptr so caller-held layer handles stay
    // pointer-stable across vector reallocations (adds / removes /
    // stable_sort).  Without this, a stable_sort of the vector would
    // rearrange the underlying storage and invalidate caller pointers.
    std::vector<std::unique_ptr<sao_ui_layer_s>> layers;
    std::vector<std::unique_ptr<sao_ui_layer_s>> pending_layer_destroys;
    // Keep detached handle shells alive until compositor teardown. Public
    // handles are raw pointers, so freeing a detached layer during present
    // would make later stale-handle validation dereference freed memory.
    size_t                         released_pending_count{0};
    std::vector<PendingFadeCall>   pending_fade_callbacks;
    std::vector<InputCallbackInvocation> pending_owner_input_callbacks;
    std::vector<PostInputTask>     post_input_tasks;
    mutable std::mutex           mtx;
    std::condition_variable      input_callbacks_idle;
    uint64_t                     seq{0};
    sao_ui_d3d11_device_handle_t d3d11_device{nullptr};
    sao_ui_dcomp_bridge_handle_t dcomp_bridge{nullptr};
#if defined(_WIN32)
    ID3D11Texture2D* gpu_master_texture{nullptr};
    ID3D11RenderTargetView* gpu_master_rtv{nullptr};
    ID3D11Texture2D* gpu_readback_texture{nullptr};
    ID3D11Texture2D* gpu_cpu_prefix_texture{nullptr};
    ID3D11ShaderResourceView* gpu_cpu_prefix_srv{nullptr};
    ID3D11VertexShader* gpu_master_vs{nullptr};
    ID3D11PixelShader* gpu_master_ps{nullptr};
    ID3D11Buffer* gpu_master_constants{nullptr};
    ID3D11SamplerState* gpu_master_sampler{nullptr};
    ID3D11BlendState* gpu_master_blend{nullptr};
    uint32_t gpu_master_width{0};
    uint32_t gpu_master_height{0};
    uint32_t gpu_cpu_prefix_width{0};
    uint32_t gpu_cpu_prefix_height{0};
    uint64_t gpu_cpu_prefix_revision{UINT64_MAX};
#endif
    sao_ui_z_order_manager_handle_t z_order{nullptr};
    void*                         game_hwnd{nullptr};
    std::thread::id               render_thread{};
    bool                          presented_visible_content{false};
    uint32_t                      last_present_width{0};
    uint32_t                      last_present_height{0};
    sao::ui::input_router_detail::LayerInputState* input_state{nullptr};
    size_t                        input_dispatch_depth{0};
    bool                          host_callbacks_bound{false};
    std::atomic_bool              present_in_progress{false};
    sao_ui_compositor_s*          registry_next{nullptr};
};

namespace {

std::mutex g_compositor_registry_mutex;
sao_ui_compositor_s* g_compositor_registry_head = nullptr;
std::unordered_map<sao_ui_overlay_host_handle_t, sao_ui_compositor_s*>
    g_host_compositor_claims;
std::atomic_bool g_fail_next_compositor_destroy_after_preflight{};

sao_status_t claim_compositor_host(sao_ui_overlay_host_handle_t host,
                                   sao_ui_compositor_s* owner) noexcept {
    if (host == nullptr || owner == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_compositor_registry_mutex);
        const auto [_, inserted] = g_host_compositor_claims.emplace(host, owner);
        return inserted ? SAO_STATUS_OK : SAO_STATUS_ERR_ALREADY_EXISTS;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

void release_compositor_host_claim(sao_ui_overlay_host_handle_t host,
                                   const sao_ui_compositor_s* owner) noexcept {
    if (host == nullptr || owner == nullptr)
        return;
    try {
        std::lock_guard lock(g_compositor_registry_mutex);
        const auto found = g_host_compositor_claims.find(host);
        if (found != g_host_compositor_claims.end() && found->second == owner)
            g_host_compositor_claims.erase(found);
    } catch (...) {
    }
}

class HostCompositorClaimGuard final {
  public:
        HostCompositorClaimGuard() = default;

    ~HostCompositorClaimGuard() {
        if (armed_)
            release_compositor_host_claim(host_, owner_);
    }

    sao_status_t acquire(sao_ui_overlay_host_handle_t host,
                         sao_ui_compositor_s* owner) noexcept {
        const sao_status_t status = claim_compositor_host(host, owner);
        if (status == SAO_STATUS_OK) {
            host_ = host;
            owner_ = owner;
            armed_ = true;
        }
        return status;
    }

    void commit() noexcept {
        armed_ = false;
    }

    HostCompositorClaimGuard(const HostCompositorClaimGuard&) = delete;
    HostCompositorClaimGuard& operator=(const HostCompositorClaimGuard&) = delete;

  private:
    sao_ui_overlay_host_handle_t host_{};
    sao_ui_compositor_s* owner_{};
    bool armed_{};
};

bool compositor_registered_locked(const sao_ui_compositor_s* compositor) noexcept {
    for (auto* current = g_compositor_registry_head; current != nullptr;
         current = current->registry_next) {
        if (current == compositor)
            return true;
    }
    return false;
}

void register_compositor(sao_ui_compositor_s* compositor) {
    std::lock_guard lock(g_compositor_registry_mutex);
    compositor->registry_next = g_compositor_registry_head;
    g_compositor_registry_head = compositor;
}

void unregister_compositor(sao_ui_compositor_s* compositor) noexcept {
    try {
        std::lock_guard lock(g_compositor_registry_mutex);
        auto** link = &g_compositor_registry_head;
        while (*link != nullptr) {
            if (*link == compositor) {
                *link = compositor->registry_next;
                compositor->registry_next = nullptr;
                return;
            }
            link = &(*link)->registry_next;
        }
    } catch (...) {
    }
}

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kLeftButtonDoubleClick = 0x0203;
constexpr uint32_t kRightButtonDown = 0x0204;
constexpr uint32_t kRightButtonUp = 0x0205;
constexpr uint32_t kRightButtonDoubleClick = 0x0206;
constexpr uint32_t kMiddleButtonDown = 0x0207;
constexpr uint32_t kMiddleButtonUp = 0x0208;
constexpr uint32_t kMiddleButtonDoubleClick = 0x0209;
constexpr uint32_t kMouseWheel = 0x020A;
constexpr uint32_t kCaptureChanged = 0x0215;
constexpr uint32_t kMouseLeave = 0x02A3;
constexpr uint32_t kCancelMode = 0x001F;

struct ActiveInputCallback {
    sao_ui_layer_s* layer{};
    uint64_t generation{};
    ActiveInputCallback* previous{};
};

thread_local ActiveInputCallback* g_active_input_callback = nullptr;

size_t input_callback_active_count(sao_ui_layer_s* layer, uint64_t generation) noexcept {
    size_t count = 0;
    for (const ActiveInputCallback* active = g_active_input_callback; active != nullptr;
         active = active->previous) {
        if (active->layer == layer && active->generation == generation)
            ++count;
    }
    return count;
}

thread_local const InputCallbackInvocation* g_active_input_invocation = nullptr;

bool capture_input_callback_locked(sao_ui_compositor_s* compositor, sao_ui_layer_s* layer,
                                   InputCallbackInvocation::Kind kind, float x, float y,
                                   int32_t button_id, int32_t action,
                                   InputCallbackInvocation* out) {
    if (layer == nullptr || out == nullptr)
        return false;
    InputCallbackInvocation invocation{};
    invocation.kind = kind;
    invocation.compositor = compositor;
    invocation.layer = layer;
    invocation.generation = layer->input_callback_generation;
    invocation.user_data = layer->input_user_data;
    invocation.x = x;
    invocation.y = y;
    invocation.button_id = button_id;
    invocation.action = action;
    switch (kind) {
    case InputCallbackInvocation::Kind::cursor:
        invocation.cursor = layer->cursor_pos_fn;
        if (invocation.cursor == nullptr)
            return false;
        break;
    case InputCallbackInvocation::Kind::leave:
        invocation.leave = layer->cursor_leave_fn;
        if (invocation.leave == nullptr)
            return false;
        break;
    case InputCallbackInvocation::Kind::button:
        invocation.button = layer->button_fn;
        if (invocation.button == nullptr)
            return false;
        break;
    case InputCallbackInvocation::Kind::scroll:
        invocation.scroll = layer->scroll_fn;
        if (invocation.scroll == nullptr)
            return false;
        break;
    }
    const auto generation = layer->input_callbacks_by_generation.find(invocation.generation);
    if (generation == layer->input_callbacks_by_generation.end())
        return false;
    ++generation->second;
    ++layer->input_callbacks_in_flight;
    *out = invocation;
    return true;
}

bool invoke_input_callback(const InputCallbackInvocation& invocation) noexcept {
    ActiveInputCallback marker{invocation.layer, invocation.generation,
                               g_active_input_callback};
    const InputCallbackInvocation* const previous_invocation = g_active_input_invocation;
    g_active_input_callback = &marker;
    g_active_input_invocation = &invocation;
    bool failed = false;
    try {
        switch (invocation.kind) {
        case InputCallbackInvocation::Kind::cursor:
            invocation.cursor(invocation.x, invocation.y, invocation.user_data);
            break;
        case InputCallbackInvocation::Kind::leave:
            invocation.leave(invocation.user_data);
            break;
        case InputCallbackInvocation::Kind::button:
            invocation.button(invocation.button_id, invocation.action, 0, invocation.x,
                              invocation.y, invocation.user_data);
            break;
        case InputCallbackInvocation::Kind::scroll:
            invocation.scroll(invocation.scroll_dx, invocation.scroll_dy, invocation.user_data);
            break;
        }
    } catch (...) {
        failed = true;
    }
    g_active_input_invocation = previous_invocation;
    g_active_input_callback = marker.previous;
    try {
        std::lock_guard lock(invocation.compositor->mtx);
        auto& generations = invocation.layer->input_callbacks_by_generation;
        const auto found = generations.find(invocation.generation);
        if (found != generations.end() && --found->second == 0 &&
            invocation.generation != invocation.layer->input_callback_generation) {
            generations.erase(found);
        }
        if (invocation.layer->input_callbacks_in_flight > 0)
            --invocation.layer->input_callbacks_in_flight;
    } catch (...) {
        failed = true;
    }
    invocation.compositor->input_callbacks_idle.notify_all();
    return !failed;
}

bool layer_accepts_input_at_locked(const sao_ui_layer_s* layer, int32_t host_x, int32_t host_y,
                                   float* out_layer_x, float* out_layer_y) {
    if (layer == nullptr || !layer->visible || !layer->input_enabled || layer->click_through ||
        layer->input_proxy_enabled || layer->alpha <= 0.0F || layer->width <= 0 ||
        layer->height <= 0) {
        return false;
    }
    const int64_t local_x = static_cast<int64_t>(host_x) - layer->x;
    const int64_t local_y = static_cast<int64_t>(host_y) - layer->y;
    if (local_x < 0 || local_y < 0 || local_x >= layer->width || local_y >= layer->height)
        return false;

    bool hit = false;
    if (!layer->input_rects.empty()) {
        for (const auto& rect : layer->input_rects) {
            if (local_x >= rect.x && local_y >= rect.y &&
                local_x < static_cast<int64_t>(rect.x) + rect.width &&
                local_y < static_cast<int64_t>(rect.y) + rect.height) {
                hit = true;
                break;
            }
        }
    } else if (layer->rect_hit || layer->bgra_pixels.empty() || layer->bgra_width == 0 ||
               layer->bgra_height == 0) {
        hit = true;
    } else if (local_x < layer->bgra_width && local_y < layer->bgra_height) {
        const size_t offset = static_cast<size_t>(local_y) * layer->bgra_stride +
                              static_cast<size_t>(local_x) * 4U + 3U;
        hit = offset < layer->bgra_pixels.size() && layer->bgra_pixels[offset] != 0;
    }
    if (!hit)
        return false;
    if (out_layer_x != nullptr)
        *out_layer_x = static_cast<float>(local_x);
    if (out_layer_y != nullptr)
        *out_layer_y = static_cast<float>(local_y);
    return true;
}

void captured_layer_coordinates_locked(const sao_ui_layer_s* layer, int32_t host_x, int32_t host_y,
                                       float* out_layer_x, float* out_layer_y) noexcept {
    if (out_layer_x != nullptr)
        *out_layer_x = static_cast<float>(static_cast<int64_t>(host_x) - layer->x);
    if (out_layer_y != nullptr)
        *out_layer_y = static_cast<float>(static_cast<int64_t>(host_y) - layer->y);
}

bool capture_router_action_locked(
    sao_ui_compositor_s* compositor,
    const sao::ui::input_router_detail::LayerInputAction& action,
    int32_t host_x, int32_t host_y,
    InputCallbackInvocation* out) {
    auto* layer = static_cast<sao_ui_layer_s*>(action.layer);
    if (layer == nullptr || out == nullptr)
        return false;
    float layer_x = action.x;
    float layer_y = action.y;
    if (action.coordinates_are_host)
        captured_layer_coordinates_locked(layer, host_x, host_y, &layer_x, &layer_y);
    switch (action.kind) {
    case sao::ui::input_router_detail::LayerInputActionKind::cursor:
        return capture_input_callback_locked(
            compositor, layer, InputCallbackInvocation::Kind::cursor, layer_x, layer_y, -1, 0,
            out);
    case sao::ui::input_router_detail::LayerInputActionKind::leave:
        return capture_input_callback_locked(
            compositor, layer, InputCallbackInvocation::Kind::leave, 0.0F, 0.0F, -1, 0, out);
    case sao::ui::input_router_detail::LayerInputActionKind::button:
        return capture_input_callback_locked(
            compositor, layer, InputCallbackInvocation::Kind::button, layer_x, layer_y,
            action.button, action.action, out);
    case sao::ui::input_router_detail::LayerInputActionKind::scroll:
        if (!capture_input_callback_locked(
                compositor, layer, InputCallbackInvocation::Kind::scroll, layer_x, layer_y, -1, 0,
                out)) {
            return false;
        }
        out->scroll_dx = action.scroll_dx;
        out->scroll_dy = action.scroll_dy;
        return true;
    }
    return false;
}

sao_status_t detach_invalid_input_layer_locked(
    sao_ui_compositor_s* compositor, sao_ui_layer_s* layer,
    std::vector<InputCallbackInvocation>* invocations) {
    if (layer == nullptr || !sao::ui::input_router_detail::layer_input_references(
                                compositor->input_state, layer)) {
        return SAO_STATUS_OK;
    }
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    bool still_accepts = false;
    if (sao::ui::input_router_detail::layer_input_last_pointer(
            compositor->input_state, &pointer_x, &pointer_y)) {
        still_accepts = layer_accepts_input_at_locked(
            layer, pointer_x, pointer_y, nullptr, nullptr);
    }
    std::array<sao::ui::input_router_detail::LayerInputAction, 1> actions{};
    size_t action_count = 0;
    const sao_status_t route_status = sao::ui::input_router_detail::invalidate_layer_input(
        compositor->input_state, layer, still_accepts, actions.data(), actions.size(),
        &action_count);
    if (route_status != SAO_STATUS_OK)
        return route_status;
    if (invocations == nullptr)
        return SAO_STATUS_OK;
    for (size_t index = 0; index < action_count; ++index) {
        InputCallbackInvocation invocation{};
        if (capture_router_action_locked(compositor, actions[index], pointer_x, pointer_y,
                                         &invocation)) {
            invocations->push_back(invocation);
        }
    }
    return SAO_STATUS_OK;
}

template <typename Fn>
sao_status_t mutate_input_layer(sao_ui_layer_handle_t layer, Fn&& fn) noexcept {
    if (layer == nullptr || layer->owner == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* compositor = layer->owner;
    std::vector<InputCallbackInvocation> invocations;
    try {
        {
            std::lock_guard lock(compositor->mtx);
            const auto active = std::find_if(
                compositor->layers.begin(), compositor->layers.end(),
                [layer](const std::unique_ptr<sao_ui_layer_s>& candidate) {
                    return candidate.get() == layer;
                });
            if (active == compositor->layers.end())
                return SAO_STATUS_ERR_HANDLE_INVALID;
            const sao_status_t status = std::forward<Fn>(fn)(compositor, layer);
            if (status != SAO_STATUS_OK)
                return status;
            const sao_status_t input_status =
                detach_invalid_input_layer_locked(compositor, layer, &invocations);
            if (input_status != SAO_STATUS_OK)
                return input_status;
        }
        if (!invocations.empty() &&
            std::this_thread::get_id() != compositor->render_thread) {
            std::lock_guard lock(compositor->mtx);
            compositor->pending_owner_input_callbacks.insert(
                compositor->pending_owner_input_callbacks.end(),
                std::make_move_iterator(invocations.begin()),
                std::make_move_iterator(invocations.end()));
            return SAO_STATUS_OK;
        }
        bool callback_failed = false;
        for (const auto& invocation : invocations)
            callback_failed = !invoke_input_callback(invocation) || callback_failed;
        return callback_failed ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

bool drain_owner_input_callbacks(sao_ui_compositor_s* compositor) {
    bool callback_failed = false;
    for (;;) {
        std::vector<InputCallbackInvocation> invocations;
        {
            std::lock_guard lock(compositor->mtx);
            invocations.swap(compositor->pending_owner_input_callbacks);
        }
        if (invocations.empty())
            return callback_failed;
        for (const auto& invocation : invocations)
            callback_failed = !invoke_input_callback(invocation) || callback_failed;
    }
}

std::vector<PostInputTask> end_input_dispatch(sao_ui_compositor_s* compositor) {
    std::vector<PostInputTask> tasks;
    std::lock_guard lock(compositor->mtx);
    if (compositor->input_dispatch_depth > 0)
        --compositor->input_dispatch_depth;
    if (compositor->input_dispatch_depth == 0)
        tasks.swap(compositor->post_input_tasks);
    return tasks;
}

void run_post_input_tasks(std::vector<PostInputTask> tasks) noexcept {
    for (const auto& task : tasks) {
        try {
            task.fn(task.user);
        } catch (...) {
        }
    }
}

struct PendingInputFlushResult {
    sao_status_t status{SAO_STATUS_OK};
    bool ran_post_tasks{};
};

PendingInputFlushResult flush_pending_owner_input(sao_ui_compositor_s* compositor) {
    {
        std::lock_guard lock(compositor->mtx);
        ++compositor->input_dispatch_depth;
    }
    const bool callback_failed = drain_owner_input_callbacks(compositor);
    auto tasks = end_input_dispatch(compositor);
    const bool ran_tasks = !tasks.empty();
    run_post_input_tasks(std::move(tasks));
    return {callback_failed ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK, ran_tasks};
}

sao_status_t screen_to_host(sao_ui_compositor_s* compositor, int32_t screen_x, int32_t screen_y,
                            int32_t* out_host_x, int32_t* out_host_y) {
    if (out_host_x == nullptr || out_host_y == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (compositor->host == nullptr) {
        *out_host_x = screen_x;
        *out_host_y = screen_y;
        return SAO_STATUS_OK;
    }
    SaoOverlayHostState state{};
    const sao_status_t status = sao_ui_overlay_host_get_state(compositor->host, &state);
    if (status != SAO_STATUS_OK)
        return status;
    const int64_t host_x = static_cast<int64_t>(screen_x) - state.geometry.x;
    const int64_t host_y = static_cast<int64_t>(screen_y) - state.geometry.y;
    if (host_x < INT32_MIN || host_x > INT32_MAX || host_y < INT32_MIN || host_y > INT32_MAX)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_host_x = static_cast<int32_t>(host_x);
    *out_host_y = static_cast<int32_t>(host_y);
    return SAO_STATUS_OK;
}

sao_ui_layer_s* top_input_layer_locked(sao_ui_compositor_s* compositor, int32_t host_x,
                                       int32_t host_y, float* out_layer_x, float* out_layer_y) {
    for (auto it = compositor->layers.rbegin(); it != compositor->layers.rend(); ++it) {
        if (layer_accepts_input_at_locked(it->get(), host_x, host_y, out_layer_x, out_layer_y))
            return it->get();
    }
    return nullptr;
}

bool SAO_UI_CALL compositor_host_hit_test(int32_t screen_x, int32_t screen_y, void* user_data) {
    bool hit = false;
    return sao_ui_compositor_hit_test(static_cast<sao_ui_compositor_handle_t>(user_data), screen_x,
                                      screen_y, &hit) == SAO_STATUS_OK &&
           hit;
}

void SAO_UI_CALL compositor_host_mouse(uint32_t message, int32_t screen_x, int32_t screen_y,
                                       int32_t button, int32_t wheel_delta, void* user_data) {
    (void)sao_ui_compositor_dispatch_mouse(static_cast<sao_ui_compositor_handle_t>(user_data),
                                           message, screen_x, screen_y, button, wheel_delta);
}

constexpr size_t kMaxBgraBufferBytes =
    static_cast<size_t>(SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES);

// Comparator for the "smaller z draws first, larger z draws on top" rule
// documented on SaoLayerConfig::z_order.  Ties break on creation_seq so
// two layers created with the same z keep their insertion order.
struct LayerLess {
    bool operator()(const std::unique_ptr<sao_ui_layer_s>& a,
                    const std::unique_ptr<sao_ui_layer_s>& b) const {
        if (a->z_order != b->z_order) return a->z_order < b->z_order;
        return a->creation_seq < b->creation_seq;
    }
};

struct PresentGuard {
    std::atomic_bool* flag;
    ~PresentGuard() {
        flag->store(false, std::memory_order_release);
    }
};

// Find a layer inside a compositor's vector by pointer.  Returns end()
// when not present.  Caller must already hold compositor->mtx.
auto find_layer_it(sao_ui_compositor_s* comp,
                   sao_ui_layer_handle_t layer) {
    return std::find_if(
        comp->layers.begin(), comp->layers.end(),
        [layer](const std::unique_ptr<sao_ui_layer_s>& p) {
            return p.get() == layer;
        });
}

template <typename Fn>
sao_status_t with_active_layer_locked(
    sao_ui_compositor_s* comp, sao_ui_layer_handle_t layer, Fn&& fn) noexcept {
    if (comp == nullptr || layer == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    try {
        std::lock_guard<std::mutex> lock(comp->mtx);
        if (find_layer_it(comp, layer) == comp->layers.end()) {
            return SAO_STATUS_ERR_HANDLE_INVALID;
        }
        return std::forward<Fn>(fn)(comp, layer);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

template <typename Fn>
sao_status_t with_active_layer_locked(
    sao_ui_layer_handle_t layer, Fn&& fn) noexcept {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    return with_active_layer_locked(
        layer->owner, layer, std::forward<Fn>(fn));
}

void mark_layer_dirty(sao_ui_layer_s* layer) {
    layer->bgra_dirty = true;
    ++layer->visual_revision;
}

#if defined(_WIN32)
void release_shared_texture_objects(sao_ui_layer_s* layer);

template <typename T> void release_com(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

void release_layer_gpu_surface(sao_ui_layer_s* layer) noexcept {
    if (layer == nullptr)
        return;
    release_com(layer->gpu_srv);
    release_com(layer->gpu_rtv);
    release_com(layer->gpu_texture);
    layer->gpu_width = 0;
    layer->gpu_height = 0;
    layer->gpu_uploaded_revision = UINT64_MAX;
}

void release_master_gpu_resources(sao_ui_compositor_s* compositor) noexcept {
    if (compositor == nullptr)
        return;
    release_com(compositor->gpu_master_blend);
    release_com(compositor->gpu_master_sampler);
    release_com(compositor->gpu_master_constants);
    release_com(compositor->gpu_master_ps);
    release_com(compositor->gpu_master_vs);
    release_com(compositor->gpu_readback_texture);
    release_com(compositor->gpu_cpu_prefix_srv);
    release_com(compositor->gpu_cpu_prefix_texture);
    release_com(compositor->gpu_master_rtv);
    release_com(compositor->gpu_master_texture);
    compositor->gpu_master_width = 0;
    compositor->gpu_master_height = 0;
    compositor->gpu_cpu_prefix_width = 0;
    compositor->gpu_cpu_prefix_height = 0;
    compositor->gpu_cpu_prefix_revision = UINT64_MAX;
}

sao_status_t gpu_failure_status(ID3D11Device* device) noexcept {
    return device != nullptr && FAILED(device->GetDeviceRemovedReason())
               ? SAO_STATUS_ERR_DEVICE_LOST
               : SAO_STATUS_ERR_OS_CALL_FAILED;
}
#endif

bool clear_bgra_cache(sao_ui_layer_s* layer) {
    const bool had_cache = !layer->bgra_pixels.empty() ||
        layer->bgra_width != 0 || layer->bgra_height != 0 ||
        layer->bgra_stride != 0;
    std::vector<uint8_t>{}.swap(layer->bgra_pixels);
    layer->bgra_width = 0;
    layer->bgra_height = 0;
    layer->bgra_stride = 0;
    return had_cache;
}

void release_detached_layer_payload(sao_ui_layer_s* layer) {
#if defined(_WIN32)
    release_shared_texture_objects(layer);
    release_layer_gpu_surface(layer);
#endif
    std::vector<SaoUiLayerInputRect>{}.swap(layer->input_rects);
    clear_bgra_cache(layer);
    std::string{}.swap(layer->name);
    std::string{}.swap(layer->mmf_name);
    layer->render_fn = nullptr;
    layer->render_user_data = nullptr;
    layer->d3d11_render_fn = nullptr;
    layer->d3d11_render_user_data = nullptr;
    layer->fade_done_fn = nullptr;
    layer->fade_done_user_data = nullptr;
    layer->cursor_pos_fn = nullptr;
    layer->cursor_leave_fn = nullptr;
    layer->button_fn = nullptr;
    layer->scroll_fn = nullptr;
    layer->input_user_data = nullptr;
    layer->detached_payload_released = true;
}

void flush_pending_layer_destroys_locked(sao_ui_compositor_s* comp) {
    for (const auto& pending : comp->pending_layer_destroys) {
        if (!pending->detached_payload_released && pending->input_callbacks_in_flight == 0) {
            release_detached_layer_payload(pending.get());
            ++comp->released_pending_count;
        }
    }
}

uint8_t scale_alpha(uint8_t value, uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * alpha + 127u) /
                                255u);
}

#if defined(_WIN32)
struct D3dUnmapGuard {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~D3dUnmapGuard() {
        context->Unmap(texture, 0);
    }
};

struct KeyedMutexReleaseGuard {
    IDXGIKeyedMutex* mutex;
    bool acquired;
    ~KeyedMutexReleaseGuard() {
        if (acquired) {
            (void)mutex->ReleaseSync(
                SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY);
        }
    }
};

struct WinHandleGuard {
    HANDLE handle;
    ~WinHandleGuard() {
        if (handle != nullptr) ::CloseHandle(handle);
    }
};

struct MappedViewGuard {
    const void* view;
    ~MappedViewGuard() {
        if (view != nullptr) ::UnmapViewOfFile(view);
    }
};

void release_shared_texture_objects(sao_ui_layer_s* layer) {
    if (layer->shared_keyed_mutex != nullptr) {
        layer->shared_keyed_mutex->Release();
        layer->shared_keyed_mutex = nullptr;
    }
    if (layer->shared_staging != nullptr) {
        layer->shared_staging->Release();
        layer->shared_staging = nullptr;
    }
    if (layer->shared_texture != nullptr) {
        layer->shared_texture->Release();
        layer->shared_texture = nullptr;
    }
}

bool refresh_shared_texture_locked(sao_ui_layer_s* layer,
                                   sao_ui_d3d11_device_handle_t device_handle) {
    if (layer->shared_handle == nullptr || layer->shared_width == 0 ||
        layer->shared_height == 0 || device_handle == nullptr) {
        return false;
    }
    auto* device = static_cast<ID3D11Device*>(sao_ui_d3d11_device_ptr(device_handle));
    if (device == nullptr) {
        clear_bgra_cache(layer);
        return false;
    }
    if (layer->shared_texture == nullptr) {
        if (FAILED(device->OpenSharedResource(
                layer->shared_handle, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&layer->shared_texture))) ||
            layer->shared_texture == nullptr) {
            release_shared_texture_objects(layer);
            clear_bgra_cache(layer);
            return false;
        }
        IDXGIKeyedMutex* keyed_mutex = nullptr;
        if (SUCCEEDED(layer->shared_texture->QueryInterface(
                __uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&keyed_mutex)))) {
            layer->shared_keyed_mutex = keyed_mutex;
        }
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    layer->shared_texture->GetDesc(&source_desc);
    if (source_desc.Width != layer->shared_width ||
        source_desc.Height != layer->shared_height) {
        release_shared_texture_objects(layer);
        clear_bgra_cache(layer);
        return false;
    }
    if (layer->shared_staging == nullptr) {
        D3D11_TEXTURE2D_DESC staging_desc = source_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &layer->shared_staging)) ||
            layer->shared_staging == nullptr) {
            release_shared_texture_objects(layer);
            clear_bgra_cache(layer);
            return false;
        }
    }
    auto* context = static_cast<ID3D11DeviceContext*>(
        sao_ui_d3d11_device_context_ptr(device_handle));
    if (context == nullptr) {
        clear_bgra_cache(layer);
        return false;
    }
    if (FAILED(device->GetDeviceRemovedReason())) {
        // Device was removed after we opened / allocated the shared texture,
        // staging texture, and keyed mutex earlier in this call (or on a
        // prior tick). Every one of those COM pointers is now dangling: the
        // caller will observe DEVICE_LOST, tear the compositor down, and
        // rebuild fresh. Without this cleanup the layer would keep holding
        // released COM refs, so both the immediate rebuild and the next
        // refresh would silently reuse invalid memory.
        release_shared_texture_objects(layer);
        clear_bgra_cache(layer);
        return false;
    }
    KeyedMutexReleaseGuard keyed_guard{
        layer->shared_keyed_mutex, false};
    if (layer->shared_keyed_mutex != nullptr) {
        const HRESULT acquire_status =
            layer->shared_keyed_mutex->AcquireSync(
                SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY,
                SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_TIMEOUT_MS);
        if (acquire_status == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            return false;
        }
        if (acquire_status != S_OK) {
            release_shared_texture_objects(layer);
            clear_bgra_cache(layer);
            return false;
        }
        keyed_guard.acquired = true;
    }
    context->CopyResource(layer->shared_staging, layer->shared_texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(layer->shared_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        clear_bgra_cache(layer);
        return false;
    }
    const D3dUnmapGuard unmap_guard{context, layer->shared_staging};
    const size_t pixel_count = static_cast<size_t>(layer->shared_width) * layer->shared_height;
    std::vector<uint8_t> converted(pixel_count * 4u);
    for (uint32_t y = 0; y < layer->shared_height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst = converted.data() + static_cast<size_t>(y) * layer->shared_width * 4u;
        for (uint32_t x = 0; x < layer->shared_width; ++x) {
            const uint8_t alpha = src[x * 4u + 3u];
            dst[x * 4u + 0u] = scale_alpha(src[x * 4u + 2u], alpha);
            dst[x * 4u + 1u] = scale_alpha(src[x * 4u + 1u], alpha);
            dst[x * 4u + 2u] = scale_alpha(src[x * 4u + 0u], alpha);
            dst[x * 4u + 3u] = alpha;
        }
    }
    layer->bgra_pixels = std::move(converted);
    layer->bgra_width = layer->shared_width;
    layer->bgra_height = layer->shared_height;
    layer->bgra_stride = layer->shared_width * 4u;
    mark_layer_dirty(layer);
    return true;
}

bool compose_premultiplied_bgra_locked(const sao_ui_compositor_s* comp, void* d3d11_device_ptr,
                                       std::vector<uint8_t>* out_pixels, uint32_t* out_width,
                                       uint32_t* out_height, bool* out_has_visible_alpha,
                                       size_t begin_layer, size_t end_layer);

struct MasterLayerConstants {
    float viewport[2];
    float origin[2];
    float size[2];
    float opacity;
    float padding;
};
static_assert(sizeof(MasterLayerConstants) == 32u);

bool ensure_master_pipeline_locked(sao_ui_compositor_s* compositor, ID3D11Device* device,
                                   uint32_t width, uint32_t height) {
    if (compositor == nullptr || device == nullptr || width == 0 || height == 0)
        return false;
    const bool pipeline_ready =
        compositor->gpu_master_vs != nullptr && compositor->gpu_master_ps != nullptr &&
        compositor->gpu_master_constants != nullptr && compositor->gpu_master_sampler != nullptr &&
        compositor->gpu_master_blend != nullptr;
    if (!pipeline_ready) {
        release_master_gpu_resources(compositor);
        const HRESULT vs_hr = device->CreateVertexShader(g_sao_ui_compositor_master_vs,
                                                         sizeof(g_sao_ui_compositor_master_vs),
                                                         nullptr, &compositor->gpu_master_vs);
        const HRESULT ps_hr = device->CreatePixelShader(g_sao_ui_compositor_master_ps,
                                                        sizeof(g_sao_ui_compositor_master_ps),
                                                        nullptr, &compositor->gpu_master_ps);
        if (FAILED(vs_hr) || FAILED(ps_hr)) {
            release_master_gpu_resources(compositor);
            return false;
        }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(MasterLayerConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &compositor->gpu_master_constants))) {
            release_master_gpu_resources(compositor);
            return false;
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sampler, &compositor->gpu_master_sampler))) {
            release_master_gpu_resources(compositor);
            return false;
        }
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blend, &compositor->gpu_master_blend))) {
            release_master_gpu_resources(compositor);
            return false;
        }
    }
    if (compositor->gpu_master_texture != nullptr && compositor->gpu_master_width == width &&
        compositor->gpu_master_height == height)
        return true;
    release_com(compositor->gpu_readback_texture);
    release_com(compositor->gpu_master_rtv);
    release_com(compositor->gpu_master_texture);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &compositor->gpu_master_texture)) ||
        FAILED(device->CreateRenderTargetView(compositor->gpu_master_texture, nullptr,
                                              &compositor->gpu_master_rtv))) {
        release_com(compositor->gpu_master_rtv);
        release_com(compositor->gpu_master_texture);
        return false;
    }
    compositor->gpu_master_width = width;
    compositor->gpu_master_height = height;
    return true;
}

bool ensure_layer_gpu_surface(sao_ui_layer_s* layer, ID3D11Device* device, uint32_t width,
                              uint32_t height, bool render_target) {
    if (layer == nullptr || device == nullptr || width == 0 || height == 0)
        return false;
    if (layer->gpu_texture != nullptr && layer->gpu_width == width && layer->gpu_height == height &&
        (!render_target || layer->gpu_rtv != nullptr))
        return true;
    release_layer_gpu_surface(layer);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (render_target ? D3D11_BIND_RENDER_TARGET : 0u);
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &layer->gpu_texture)) ||
        FAILED(device->CreateShaderResourceView(layer->gpu_texture, nullptr, &layer->gpu_srv)) ||
        (render_target &&
         FAILED(device->CreateRenderTargetView(layer->gpu_texture, nullptr, &layer->gpu_rtv)))) {
        release_layer_gpu_surface(layer);
        return false;
    }
    layer->gpu_width = width;
    layer->gpu_height = height;
    return true;
}

bool ensure_cpu_prefix_surface(sao_ui_compositor_s* compositor, ID3D11Device* device,
                               uint32_t width, uint32_t height) {
    if (compositor->gpu_cpu_prefix_texture != nullptr &&
        compositor->gpu_cpu_prefix_width == width && compositor->gpu_cpu_prefix_height == height)
        return true;
    release_com(compositor->gpu_cpu_prefix_srv);
    release_com(compositor->gpu_cpu_prefix_texture);
    compositor->gpu_cpu_prefix_width = 0;
    compositor->gpu_cpu_prefix_height = 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &compositor->gpu_cpu_prefix_texture)) ||
        FAILED(device->CreateShaderResourceView(compositor->gpu_cpu_prefix_texture, nullptr,
                                                &compositor->gpu_cpu_prefix_srv))) {
        release_com(compositor->gpu_cpu_prefix_srv);
        release_com(compositor->gpu_cpu_prefix_texture);
        return false;
    }
    compositor->gpu_cpu_prefix_width = width;
    compositor->gpu_cpu_prefix_height = height;
    return true;
}

bool has_visible_native_layer_locked(const sao_ui_compositor_s* compositor) {
    return std::ranges::any_of(compositor->layers, [](const auto& layer) {
        return layer->visible && layer->alpha > 0.0F && layer->d3d11_render_fn != nullptr;
    });
}

bool gpu_composition_extent_locked(const sao_ui_compositor_s* compositor, uint32_t* out_width,
                                   uint32_t* out_height) {
    int64_t right = 0;
    int64_t bottom = 0;
    for (const auto& layer : compositor->layers) {
        if (!layer->visible || layer->alpha <= 0.0F)
            continue;
        const uint32_t width = layer->d3d11_render_fn != nullptr
                                   ? static_cast<uint32_t>(std::max(0, layer->width))
                                   : layer->bgra_width;
        const uint32_t height = layer->d3d11_render_fn != nullptr
                                    ? static_cast<uint32_t>(std::max(0, layer->height))
                                    : layer->bgra_height;
        right = std::max(right, static_cast<int64_t>(layer->x) + width);
        bottom = std::max(bottom, static_cast<int64_t>(layer->y) + height);
    }
    if (right <= 0 || bottom <= 0 || right > UINT32_MAX || bottom > UINT32_MAX)
        return false;
    *out_width = static_cast<uint32_t>(right);
    *out_height = static_cast<uint32_t>(bottom);
    return true;
}

sao_status_t compose_native_layers_gpu_locked(sao_ui_compositor_s* compositor, float time_seconds,
                                              uint32_t* out_width, uint32_t* out_height) {
    auto* device = static_cast<ID3D11Device*>(sao_ui_d3d11_device_ptr(compositor->d3d11_device));
    auto* context = static_cast<ID3D11DeviceContext*>(
        sao_ui_d3d11_device_context_ptr(compositor->d3d11_device));
    if (device == nullptr || context == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;

    size_t first_native = compositor->layers.size();
    for (size_t index = 0; index < compositor->layers.size(); ++index) {
        const auto& layer = compositor->layers[index];
        if (layer->visible && layer->alpha > 0.0F && layer->d3d11_render_fn != nullptr) {
            first_native = index;
            break;
        }
    }
    if (first_native == compositor->layers.size())
        return SAO_STATUS_OK;

    // Backdrop blur and shadow depend on everything already composed below a
    // layer. Preserve that legacy semantic by flattening the unchanged CPU
    // prefix with the established effect path, then upload only when a prefix
    // revision changes. Effects above a native surface require a GPU-native
    // effect graph; report the unsupported mix rather than silently dropping it.
    for (size_t index = first_native; index < compositor->layers.size(); ++index) {
        const auto& layer = compositor->layers[index];
        if (layer->visible && layer->alpha > 0.0F && layer->d3d11_render_fn == nullptr &&
            layer->effects.flags != SAO_UI_LAYER_EFFECT_NONE)
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    }
    uint64_t prefix_revision = 1469598103934665603ull;
    for (size_t index = 0; index < first_native; ++index) {
        prefix_revision ^= compositor->layers[index]->visual_revision;
        prefix_revision *= 1099511628211ull;
    }
    prefix_revision ^= static_cast<uint64_t>(first_native);
    std::vector<uint8_t> prefix_pixels;
    uint32_t prefix_width = compositor->gpu_cpu_prefix_width;
    uint32_t prefix_height = compositor->gpu_cpu_prefix_height;
    bool prefix_has_alpha = prefix_width != 0 && prefix_height != 0;
    const bool rebuild_prefix = compositor->gpu_cpu_prefix_revision != prefix_revision;
    if (rebuild_prefix) {
        prefix_width = 0;
        prefix_height = 0;
        prefix_has_alpha = false;
        (void)compose_premultiplied_bgra_locked(compositor, device, &prefix_pixels, &prefix_width,
                                                &prefix_height, &prefix_has_alpha, 0u,
                                                first_native);
    }

    if (!gpu_composition_extent_locked(compositor, out_width, out_height))
        return SAO_STATUS_OK;
    *out_width = std::max(*out_width, prefix_width);
    *out_height = std::max(*out_height, prefix_height);
    if (!ensure_master_pipeline_locked(compositor, device, *out_width, *out_height))
        return gpu_failure_status(device);
    if (rebuild_prefix) {
        release_com(compositor->gpu_cpu_prefix_srv);
        release_com(compositor->gpu_cpu_prefix_texture);
        compositor->gpu_cpu_prefix_width = 0;
        compositor->gpu_cpu_prefix_height = 0;
        if (prefix_has_alpha) {
            if (!ensure_cpu_prefix_surface(compositor, device, prefix_width, prefix_height))
                return gpu_failure_status(device);
            context->UpdateSubresource(compositor->gpu_cpu_prefix_texture, 0, nullptr,
                                       prefix_pixels.data(), prefix_width * 4u, 0);
        }
        compositor->gpu_cpu_prefix_revision = prefix_revision;
    }
    constexpr float transparent[4]{0, 0, 0, 0};
    context->ClearRenderTargetView(compositor->gpu_master_rtv, transparent);
    D3D11_VIEWPORT viewport{
        0.0F, 0.0F, static_cast<float>(*out_width), static_cast<float>(*out_height), 0.0F, 1.0F};
    const auto draw_surface = [&](ID3D11ShaderResourceView* surface, int32_t x, int32_t y,
                                  uint32_t width, uint32_t height, float opacity) -> sao_status_t {
        ID3D11RenderTargetView* target = compositor->gpu_master_rtv;
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(compositor->gpu_master_vs, nullptr, 0);
        context->PSSetShader(compositor->gpu_master_ps, nullptr, 0);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(compositor->gpu_master_constants, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                &mapped)))
            return gpu_failure_status(device);
        auto* constants = static_cast<MasterLayerConstants*>(mapped.pData);
        *constants = {{static_cast<float>(*out_width), static_cast<float>(*out_height)},
                      {static_cast<float>(x), static_cast<float>(y)},
                      {static_cast<float>(width), static_cast<float>(height)},
                      opacity,
                      0.0F};
        context->Unmap(compositor->gpu_master_constants, 0);
        ID3D11Buffer* cb = compositor->gpu_master_constants;
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetShaderResources(0, 1, &surface);
        context->PSSetSamplers(0, 1, &compositor->gpu_master_sampler);
        constexpr float blend_factor[4]{0, 0, 0, 0};
        context->OMSetBlendState(compositor->gpu_master_blend, blend_factor, 0xffffffffu);
        context->Draw(6, 0);
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(0, 1, &null_srv);
        return SAO_STATUS_OK;
    };
    if (compositor->gpu_cpu_prefix_srv != nullptr) {
        const sao_status_t draw_status =
            draw_surface(compositor->gpu_cpu_prefix_srv, 0, 0, compositor->gpu_cpu_prefix_width,
                         compositor->gpu_cpu_prefix_height, 1.0F);
        if (draw_status != SAO_STATUS_OK)
            return draw_status;
    }
    for (size_t index = first_native; index < compositor->layers.size(); ++index) {
        const auto& owned = compositor->layers[index];
        sao_ui_layer_s* layer = owned.get();
        if (!layer->visible || layer->alpha <= 0.0F)
            continue;
        uint32_t layer_width = layer->bgra_width;
        uint32_t layer_height = layer->bgra_height;
        if (layer->d3d11_render_fn != nullptr) {
            layer_width = static_cast<uint32_t>(std::max(0, layer->width));
            layer_height = static_cast<uint32_t>(std::max(0, layer->height));
            if (!ensure_layer_gpu_surface(layer, device, layer_width, layer_height, true))
                return gpu_failure_status(device);
            if (layer->gpu_uploaded_revision != layer->visual_revision) {
                context->ClearRenderTargetView(layer->gpu_rtv, transparent);
                SaoUiD3d11LayerRenderContext render_context{};
                render_context.struct_size = SAO_UI_D3D11_LAYER_RENDER_CONTEXT_V1_SIZE;
                render_context.width_px = layer_width;
                render_context.height_px = layer_height;
                render_context.time_seconds = time_seconds;
                render_context.d3d11_device = device;
                render_context.d3d11_context = context;
                render_context.render_target_view = layer->gpu_rtv;
                const sao_status_t render_status =
                    layer->d3d11_render_fn(&render_context, layer->d3d11_render_user_data);
                if (render_status != SAO_STATUS_OK)
                    return render_status;
                layer->gpu_uploaded_revision = layer->visual_revision;
            }
        } else {
            if (layer->bgra_pixels.empty() || layer_width == 0 || layer_height == 0)
                continue;
            if (!ensure_layer_gpu_surface(layer, device, layer_width, layer_height, false))
                return gpu_failure_status(device);
            if (layer->gpu_uploaded_revision != layer->visual_revision) {
                context->UpdateSubresource(layer->gpu_texture, 0, nullptr,
                                           layer->bgra_pixels.data(), layer->bgra_stride, 0);
                layer->gpu_uploaded_revision = layer->visual_revision;
            }
        }
        const sao_status_t draw_status = draw_surface(layer->gpu_srv, layer->x, layer->y,
                                                      layer_width, layer_height, layer->alpha);
        if (draw_status != SAO_STATUS_OK)
            return draw_status;
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return FAILED(device->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST : SAO_STATUS_OK;
}

sao_status_t readback_master_locked(sao_ui_compositor_s* compositor,
                                    std::vector<uint8_t>* out_pixels) {
    auto* context = static_cast<ID3D11DeviceContext*>(
        sao_ui_d3d11_device_context_ptr(compositor->d3d11_device));
    auto* device = static_cast<ID3D11Device*>(sao_ui_d3d11_device_ptr(compositor->d3d11_device));
    if (context == nullptr || device == nullptr || compositor->gpu_master_texture == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (compositor->gpu_readback_texture == nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        compositor->gpu_master_texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        const HRESULT create_status =
            device->CreateTexture2D(&desc, nullptr, &compositor->gpu_readback_texture);
        if (FAILED(create_status))
            return gpu_failure_status(device);
    }
    context->CopyResource(compositor->gpu_readback_texture, compositor->gpu_master_texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(compositor->gpu_readback_texture, 0, D3D11_MAP_READ, 0, &mapped)))
        return gpu_failure_status(device);
    const size_t row_bytes = static_cast<size_t>(compositor->gpu_master_width) * 4u;
    out_pixels->resize(row_bytes * compositor->gpu_master_height);
    for (uint32_t row = 0; row < compositor->gpu_master_height; ++row) {
        std::memcpy(out_pixels->data() + static_cast<size_t>(row) * row_bytes,
                    static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(row) * mapped.RowPitch,
                    row_bytes);
    }
    context->Unmap(compositor->gpu_readback_texture, 0);
    return SAO_STATUS_OK;
}
#endif

struct MmfHeaderValues {
    uint32_t version{0};
    uint32_t frame_width{0};
    uint32_t frame_height{0};
    uint32_t slot_count{0};
    uint32_t slot_stride{0};
    uint64_t published_generation{0};
    uint32_t published_slot{0};
    uint32_t header_bytes{0};
};

void reset_mmf_generation(sao_ui_layer_s* layer) {
    layer->mmf_last_generation = 0;
    layer->mmf_has_last_generation = false;
}

// Hard fail-closed reset: the header decoded but validation exposed a
// structural error the producer must never emit. Clears the cached frame
// and the last generation. Used for magic/version/stride/geometry breakage
// and any second-decode disagreement mid-copy.
void fail_closed_mmf_source_hard(sao_ui_layer_s* layer) {
    layer->mmf_reconnect_attempts = 0;
    reset_mmf_generation(layer);
    if (clear_bgra_cache(layer)) mark_layer_dirty(layer);
}

// Reconnect fail-closed: the mapping is temporarily unavailable
// (OpenFileMappingA / MapViewOfFile returned NULL) but the layer state we
// have is still the last known-good producer frame. Retain the cached BGRA
// and the last generation, and bump an attempt counter so the caller can
// observe how many polls have passed without a reconnect. This lets the
// visible layer keep showing the last frame instead of collapsing to empty
// pixels the moment the producer restarts.
void fail_closed_mmf_source_reconnect(sao_ui_layer_s* layer) {
    if (layer->mmf_reconnect_attempts != std::numeric_limits<uint32_t>::max()) {
        ++layer->mmf_reconnect_attempts;
    }
}

// Legacy alias retained for existing call sites that still bundle both
// semantics into a single hard clear. Prefer the split entry points above.
void fail_closed_mmf_source(sao_ui_layer_s* layer) {
    fail_closed_mmf_source_hard(layer);
}

bool decode_mmf_header(const void* bytes, MmfHeaderValues* out) {
    if (bytes == nullptr || out == nullptr) return false;
    SaoUiSopfMmfHeaderV1 prefix{};
    std::memcpy(&prefix, bytes, sizeof(prefix));
    if (prefix.magic != SAO_UI_SOPF_MMF_MAGIC) return false;
    out->version = prefix.version;
    out->frame_width = prefix.frame_width;
    out->frame_height = prefix.frame_height;
    out->slot_count = prefix.slot_count;
    out->slot_stride = prefix.slot_stride;
    out->published_generation = prefix.published_generation;
    out->published_slot = prefix.published_slot;
    if (prefix.version == SAO_UI_SOPF_MMF_VERSION_V1) {
        out->header_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES;
        return true;
    }
    if (prefix.version != SAO_UI_SOPF_MMF_VERSION_V2) return false;
    SaoUiSopfMmfHeaderV2 header{};
    std::memcpy(&header, bytes, sizeof(header));
    out->published_slot = header.latest_completed_slot;
    out->header_bytes = header.header_bytes;
    return true;
}

bool same_mmf_structure(const MmfHeaderValues& lhs,
                        const MmfHeaderValues& rhs) {
    return lhs.version == rhs.version &&
           lhs.frame_width == rhs.frame_width &&
           lhs.frame_height == rhs.frame_height &&
           lhs.slot_count == rhs.slot_count &&
           lhs.slot_stride == rhs.slot_stride &&
           lhs.header_bytes == rhs.header_bytes;
}

bool validate_mmf_header_locked(const sao_ui_layer_s* layer,
                                const MmfHeaderValues& header,
                                size_t* out_frame_bytes,
                                size_t* out_mapping_bytes) {
    if (out_frame_bytes == nullptr || out_mapping_bytes == nullptr ||
        header.header_bytes != SAO_UI_SOPF_MMF_HEADER_BYTES ||
        header.frame_width == 0 || header.frame_height == 0 ||
        header.frame_width > static_cast<uint32_t>(
            std::numeric_limits<int32_t>::max()) ||
        header.frame_height > static_cast<uint32_t>(
            std::numeric_limits<int32_t>::max())) {
        return false;
    }
    const uint32_t minimum_slots =
        header.version == SAO_UI_SOPF_MMF_VERSION_V1
        ? SAO_UI_SOPF_MMF_V1_MIN_SLOT_COUNT
        : SAO_UI_SOPF_MMF_V2_MIN_SLOT_COUNT;
    if (header.slot_count < minimum_slots ||
        header.slot_count > SAO_UI_SOPF_MMF_MAX_SLOT_COUNT ||
        header.published_slot >= header.slot_count) {
        return false;
    }
    const uint64_t row_bytes = static_cast<uint64_t>(header.frame_width) * 4u;
    const uint64_t frame_bytes =
        row_bytes * static_cast<uint64_t>(header.frame_height);
    const uint64_t footer_bytes =
        header.version == SAO_UI_SOPF_MMF_VERSION_V2
        ? SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES
        : 0u;
    if (frame_bytes == 0 ||
        frame_bytes > SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES ||
        frame_bytes > std::numeric_limits<size_t>::max() ||
        frame_bytes + footer_bytes > header.slot_stride) {
        return false;
    }
    const uint64_t mapping_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES +
        static_cast<uint64_t>(header.slot_count) * header.slot_stride;
    if (mapping_bytes > SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES ||
        mapping_bytes > std::numeric_limits<size_t>::max()) {
        return false;
    }
    if (layer->width <= 0 || layer->height <= 0 ||
        header.frame_width != static_cast<uint32_t>(layer->width) ||
        header.frame_height != static_cast<uint32_t>(layer->height)) {
        return false;
    }
    for (const auto& rect : layer->input_rects) {
        if (static_cast<int64_t>(rect.x) + rect.width >
                header.frame_width ||
            static_cast<int64_t>(rect.y) + rect.height >
                header.frame_height) {
            return false;
        }
    }
    if (layer->shared_handle != nullptr &&
        (layer->shared_width != header.frame_width ||
         layer->shared_height != header.frame_height)) {
        return false;
    }
    *out_frame_bytes = static_cast<size_t>(frame_bytes);
    *out_mapping_bytes = static_cast<size_t>(mapping_bytes);
    return true;
}

uint64_t read_mmf_generation(const uint8_t* address) {
    uint64_t generation = 0;
    std::memcpy(&generation, address, sizeof(generation));
    std::atomic_thread_fence(std::memory_order_acquire);
    return generation;
}

void refresh_mmf_source_locked(sao_ui_layer_s* layer) {
#if defined(_WIN32)
    if (layer->mmf_name.empty()) return;
    HANDLE mapping = ::OpenFileMappingA(FILE_MAP_READ, FALSE, layer->mmf_name.c_str());
    if (mapping == nullptr) {
        // Producer restart or race: retain the last-good frame; the
        // reconnect counter tracks how many polls have missed.
        fail_closed_mmf_source_reconnect(layer);
        return;
    }
    const WinHandleGuard mapping_guard{mapping};
    const void* header_view = ::MapViewOfFile(
        mapping, FILE_MAP_READ, 0, 0, SAO_UI_SOPF_MMF_HEADER_BYTES);
    if (header_view == nullptr) {
        // Mapping opened but MapViewOfFile failed - still transient; treat
        // as a reconnect miss so we don't drop the last good frame.
        fail_closed_mmf_source_reconnect(layer);
        return;
    }
    MmfHeaderValues initial_header{};
    {
        const MappedViewGuard header_guard{header_view};
        if (!decode_mmf_header(header_view, &initial_header)) {
            fail_closed_mmf_source_hard(layer);
            return;
        }
    }
    size_t frame_bytes = 0;
    size_t mapping_bytes = 0;
    if (!validate_mmf_header_locked(
            layer, initial_header, &frame_bytes, &mapping_bytes)) {
        fail_closed_mmf_source_hard(layer);
        return;
    }
    const void* ring_view = ::MapViewOfFile(
        mapping, FILE_MAP_READ, 0, 0, mapping_bytes);
    if (ring_view == nullptr) {
        // Full ring view failed; header already decoded so treat as
        // transient reconnect.
        fail_closed_mmf_source_reconnect(layer);
        return;
    }
    const MappedViewGuard ring_guard{ring_view};
    const auto* ring = static_cast<const uint8_t*>(ring_view);
    MmfHeaderValues before{};
    if (!decode_mmf_header(ring, &before)) {
        fail_closed_mmf_source_hard(layer);
        return;
    }
    size_t checked_frame_bytes = 0;
    size_t checked_mapping_bytes = 0;
    if (!validate_mmf_header_locked(
            layer, before, &checked_frame_bytes, &checked_mapping_bytes)) {
        fail_closed_mmf_source_hard(layer);
        return;
    }
    if (!same_mmf_structure(initial_header, before) ||
        frame_bytes != checked_frame_bytes ||
        mapping_bytes != checked_mapping_bytes) {
        fail_closed_mmf_source_hard(layer);
        return;
    }
    if (layer->mmf_has_last_generation &&
        layer->mmf_last_generation == before.published_generation) {
        return;
    }

    if (before.version == SAO_UI_SOPF_MMF_VERSION_V2 &&
        (before.published_generation == 0 ||
         (before.published_generation & 1u) != 0)) {
        return;
    }
    const uint32_t read_slot =
        before.version == SAO_UI_SOPF_MMF_VERSION_V1
        ? (before.published_slot + before.slot_count - 1u) % before.slot_count
        : before.published_slot;
    const size_t slot_offset = SAO_UI_SOPF_MMF_HEADER_BYTES +
        static_cast<size_t>(read_slot) * before.slot_stride;
    const uint8_t* slot = ring + slot_offset;
    if (before.version == SAO_UI_SOPF_MMF_VERSION_V2) {
        const uint8_t* footer = slot + before.slot_stride -
            SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES;
        const uint64_t footer_before = read_mmf_generation(footer);
        if (footer_before == 0 || (footer_before & 1u) != 0 ||
            footer_before != before.published_generation) {
            return;
        }
    }

    std::vector<uint8_t> snapshot(slot, slot + frame_bytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    MmfHeaderValues after{};
    if (!decode_mmf_header(ring, &after) ||
        !same_mmf_structure(before, after)) {
        // Header changed mid-copy: producer restructured the ring. This is
        // a hard invariant violation, not a reconnect - drop the frame.
        fail_closed_mmf_source_hard(layer);
        return;
    }
    if (after.published_generation != before.published_generation ||
        after.published_slot != before.published_slot) {
        return;
    }
    if (before.version == SAO_UI_SOPF_MMF_VERSION_V2) {
        const uint8_t* footer = slot + before.slot_stride -
            SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES;
        if (read_mmf_generation(footer) != before.published_generation) {
            return;
        }
    }
    layer->bgra_pixels = std::move(snapshot);
    layer->bgra_width = before.frame_width;
    layer->bgra_height = before.frame_height;
    layer->bgra_stride = before.frame_width * 4u;
    layer->mmf_last_generation = before.published_generation;
    layer->mmf_has_last_generation = true;
    // Successful frame publish resets the reconnect attempt counter so a
    // fresh producer restart starts observing misses from zero again.
    layer->mmf_reconnect_attempts = 0;
    mark_layer_dirty(layer);
#else
    (void)layer;
#endif
}

bool has_nonzero_bgra_alpha(const sao_ui_layer_s* layer) {
    if (layer->bgra_width == 0 || layer->bgra_height == 0 ||
        layer->bgra_stride < layer->bgra_width * 4u) {
        return false;
    }
    const size_t required =
        static_cast<size_t>(layer->bgra_height - 1u) * layer->bgra_stride +
        static_cast<size_t>(layer->bgra_width) * 4u;
    if (required > layer->bgra_pixels.size()) return false;
    for (uint32_t y = 0; y < layer->bgra_height; ++y) {
        const uint8_t* row = layer->bgra_pixels.data() +
            static_cast<size_t>(y) * layer->bgra_stride;
        for (uint32_t x = 0; x < layer->bgra_width; ++x) {
            if (row[x * 4u + 3u] != 0) return true;
        }
    }
    return false;
}

bool append_host_input_rect(std::vector<SaoOverlayHostInputRect>* out,
                            int64_t x, int64_t y,
                            int64_t width, int64_t height) {
    if (out == nullptr || width <= 0 || height <= 0) return false;
    const int64_t right = x + width;
    const int64_t bottom = y + height;
    constexpr int64_t kMin = std::numeric_limits<int32_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int32_t>::max();
    if (x < kMin || y < kMin || x > kMax || y > kMax ||
        right < kMin || bottom < kMin || right > kMax || bottom > kMax ||
        width > kMax || height > kMax) {
        return false;
    }
    out->push_back({static_cast<int32_t>(x), static_cast<int32_t>(y),
                    static_cast<int32_t>(width),
                    static_cast<int32_t>(height)});
    return true;
}

bool valid_layer_geometry(int32_t x, int32_t y,
                          int64_t width, int64_t height) {
    if (width < 0 || height < 0 ||
        width > std::numeric_limits<int32_t>::max() ||
        height > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    const int64_t right = static_cast<int64_t>(x) + width;
    const int64_t bottom = static_cast<int64_t>(y) + height;
    return right >= std::numeric_limits<int32_t>::min() &&
           right <= std::numeric_limits<int32_t>::max() &&
           bottom >= std::numeric_limits<int32_t>::min() &&
           bottom <= std::numeric_limits<int32_t>::max();
}

bool checked_bgra_buffer_size(uint32_t width, uint32_t height,
                              size_t* out_size) {
    if (out_size == nullptr || width == 0 || height == 0) return false;
    const uint64_t row_bytes = static_cast<uint64_t>(width) * 4u;
    if (row_bytes > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(height) >
            std::numeric_limits<size_t>::max() / row_bytes) {
        return false;
    }
    const size_t total_size = static_cast<size_t>(row_bytes) * height;
    if (total_size > kMaxBgraBufferBytes) return false;
    *out_size = total_size;
    return true;
}

bool valid_composition_extent(int32_t x, int32_t y,
                              int64_t width, int64_t height) {
    if (!valid_layer_geometry(x, y, width, height)) return false;
    if (width == 0 || height == 0) return true;
    const int64_t right = static_cast<int64_t>(x) + width;
    const int64_t bottom = static_cast<int64_t>(y) + height;
    if (right <= 0 || bottom <= 0) return true;
    size_t ignored_size = 0;
    return checked_bgra_buffer_size(
        static_cast<uint32_t>(right), static_cast<uint32_t>(bottom),
        &ignored_size);
}

bool advance_layer_animations_and_callbacks(sao_ui_compositor_s* comp) {
    struct RenderCall {
        sao_ui_layer_render_fn_t fn;
        void* user;
        sao_ui_layer_s* layer;
    };
    std::vector<RenderCall> renders;
    std::vector<PendingFadeCall> done;
    std::vector<InputCallbackInvocation> input_invocations;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(comp->mtx);
        done.swap(comp->pending_fade_callbacks);
        for (const auto& entry : comp->layers) {
            sao_ui_layer_s* layer = entry.get();
            refresh_mmf_source_locked(layer);
#if defined(_WIN32)
            (void)refresh_shared_texture_locked(layer, comp->d3d11_device);
#endif
            if (layer->fade_active) {
                const float elapsed = std::chrono::duration<float>(now - layer->fade_started).count();
                const float progress = layer->fade_duration_sec <= 0.0f ? 1.0f
                    : std::clamp(elapsed / layer->fade_duration_sec, 0.0f, 1.0f);
                layer->alpha = layer->fade_from +
                    (layer->fade_target - layer->fade_from) * progress;
                mark_layer_dirty(layer);
                detach_invalid_input_layer_locked(comp, layer, &input_invocations);
                if (progress >= 1.0f) {
                    layer->fade_active = false;
                    if (layer->fade_done_fn != nullptr) {
                        done.push_back({layer->fade_done_fn, layer->fade_done_user_data});
                    }
                    layer->fade_done_fn = nullptr;
                    layer->fade_done_user_data = nullptr;
                }
            }
            if (layer->render_fn != nullptr && (layer->visible || layer->redraw_requested)) {
                renders.push_back(
                    {layer->render_fn, layer->render_user_data, layer});
                layer->redraw_requested = false;
            }
        }
    }
    const float seconds = std::chrono::duration<float>(now.time_since_epoch()).count();
    bool callback_failed = false;
    for (const auto& invocation : input_invocations)
        callback_failed = !invoke_input_callback(invocation) || callback_failed;
    for (const auto& call : renders) {
        try {
            call.fn(nullptr, seconds, call.user);
        } catch (...) {
            callback_failed = true;
            std::lock_guard<std::mutex> lock(comp->mtx);
            if (find_layer_it(comp, call.layer) != comp->layers.end()) {
                call.layer->redraw_requested = true;
                mark_layer_dirty(call.layer);
            }
        }
    }
    for (const auto& call : done) {
        try {
            call.fn(call.user);
        } catch (...) {
            callback_failed = true;
        }
    }
    return callback_failed;
}

bool compose_premultiplied_bgra_locked(const sao_ui_compositor_s* comp, void* d3d11_device_ptr,
                                       std::vector<uint8_t>* out_pixels, uint32_t* out_width,
                                       uint32_t* out_height, bool* out_has_visible_alpha,
                                       size_t begin_layer, size_t end_layer) {
    const size_t bounded_begin = std::min(begin_layer, comp->layers.size());
    const size_t bounded_end = std::min(std::max(end_layer, bounded_begin), comp->layers.size());
    int64_t right = 0;
    int64_t bottom = 0;
    *out_has_visible_alpha = false;
    for (size_t index = bounded_begin; index < bounded_end; ++index) {
        const auto& layer = comp->layers[index];
        if (!layer->visible || layer->bgra_pixels.empty() ||
            layer->bgra_width == 0 || layer->bgra_height == 0) {
            continue;
        }
        right = std::max(
            right, static_cast<int64_t>(layer->x) + layer->bgra_width);
        bottom = std::max(
            bottom, static_cast<int64_t>(layer->y) + layer->bgra_height);
        if ((layer->effects.flags & SAO_UI_LAYER_EFFECT_SHADOW) != 0u) {
            const int64_t margin = static_cast<int64_t>(
                std::ceil(layer->effects.shadow_sigma * 3.0F));
            right = std::max(
                right, static_cast<int64_t>(layer->x) + layer->bgra_width +
                           margin + std::max<int64_t>(
                                        0, static_cast<int64_t>(std::ceil(
                                               layer->effects.shadow_offset_x))));
            bottom = std::max(
                bottom, static_cast<int64_t>(layer->y) + layer->bgra_height +
                            margin + std::max<int64_t>(
                                         0, static_cast<int64_t>(std::ceil(
                                                layer->effects.shadow_offset_y))));
        }
    }
    if (right <= 0 || bottom <= 0) {
        out_pixels->clear();
        *out_width = 0;
        *out_height = 0;
        return false;
    }
    if (right > std::numeric_limits<uint32_t>::max() ||
        bottom > std::numeric_limits<uint32_t>::max()) {
        out_pixels->clear();
        *out_width = 0;
        *out_height = 0;
        return false;
    }

    *out_width = static_cast<uint32_t>(right);
    *out_height = static_cast<uint32_t>(bottom);
    size_t output_size = 0;
    if (!checked_bgra_buffer_size(*out_width, *out_height, &output_size)) {
        throw std::length_error("compositor BGRA extent is not representable");
    }
    out_pixels->assign(output_size, 0u);

    for (size_t index = bounded_begin; index < bounded_end; ++index) {
        const auto& layer = comp->layers[index];
        if (!layer->visible || layer->bgra_pixels.empty() ||
            layer->bgra_width == 0 || layer->bgra_height == 0 ||
            layer->alpha <= 0.0f) {
            continue;
        }
        const uint8_t layer_alpha = static_cast<uint8_t>(
            std::clamp(layer->alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        sao::ui::effects::apply_precompose(
            *out_pixels, *out_width, *out_height, layer->bgra_pixels.data(),
            layer->bgra_width, layer->bgra_height, layer->bgra_stride,
            layer->x, layer->y, layer->alpha, layer->effects,
            d3d11_device_ptr);
        for (uint32_t src_y = 0; src_y < layer->bgra_height; ++src_y) {
            const int64_t dst_y = static_cast<int64_t>(layer->y) + src_y;
            if (dst_y < 0 || dst_y >= *out_height) continue;
            const uint8_t* source_row = layer->bgra_pixels.data() +
                static_cast<size_t>(src_y) * layer->bgra_stride;
            for (uint32_t src_x = 0; src_x < layer->bgra_width; ++src_x) {
                const int64_t dst_x = static_cast<int64_t>(layer->x) + src_x;
                if (dst_x < 0 || dst_x >= *out_width) continue;
                const uint8_t* source = source_row + static_cast<size_t>(src_x) * 4u;
                uint8_t* destination = out_pixels->data() +
                    (static_cast<size_t>(dst_y) * *out_width +
                     static_cast<size_t>(dst_x)) * 4u;
                const uint8_t src_alpha = scale_alpha(source[3], layer_alpha);
                if (src_alpha != 0) *out_has_visible_alpha = true;
                const uint8_t inverse_alpha = static_cast<uint8_t>(255u - src_alpha);
                destination[0] = static_cast<uint8_t>(
                    scale_alpha(source[0], layer_alpha) +
                    scale_alpha(destination[0], inverse_alpha));
                destination[1] = static_cast<uint8_t>(
                    scale_alpha(source[1], layer_alpha) +
                    scale_alpha(destination[1], inverse_alpha));
                destination[2] = static_cast<uint8_t>(
                    scale_alpha(source[2], layer_alpha) +
                    scale_alpha(destination[2], inverse_alpha));
                destination[3] = static_cast<uint8_t>(
                    src_alpha + scale_alpha(destination[3], inverse_alpha));
            }
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Compositor lifecycle.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_create(
    sao_ui_overlay_host_handle_t host,
    const SaoCompositorConfig* config,
    sao_ui_compositor_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    // ABI v1 guard. struct_size == 0 keeps legacy callers (compiled before
    // ABI minor 8) working: their record is exactly the compile-time size.
    // Any other value must be within [V1_SIZE, sizeof(SaoCompositorConfig)]
    // so the caller cannot claim to send fields the platform does not know.
    if (config != nullptr) {
        const uint32_t declared =
            config->struct_size == 0u ? SAO_UI_COMPOSITOR_CONFIG_V1_SIZE : config->struct_size;
        if (declared < SAO_UI_COMPOSITOR_CONFIG_V1_SIZE ||
            declared > sizeof(SaoCompositorConfig)) {
            return SAO_STATUS_ERR_ABI_MISMATCH;
        }
    }
    try {
        // Host can be null in headless/tests; the compositor holds a
        // reference but headless paths never dereference it.  RGN sync /
        // presentation paths require a non-null host.
        auto comp = std::unique_ptr<sao_ui_compositor_s>(
            new (std::nothrow) sao_ui_compositor_s{});
        if (comp == nullptr)
            return SAO_STATUS_ERR_UNKNOWN;
        std::unique_ptr<sao::ui::input_router_detail::LayerInputState,
                        decltype(&sao::ui::input_router_detail::destroy_layer_input_state)>
            input_state(sao::ui::input_router_detail::create_layer_input_state(),
                        &sao::ui::input_router_detail::destroy_layer_input_state);
        if (input_state == nullptr)
            return SAO_STATUS_ERR_UNKNOWN;
        comp->input_state = input_state.get();
        comp->host = host;
        if (config != nullptr) {
            comp->config = *config;
            // Force our internal record to advertise the platform's compiled
            // size regardless of the caller's declaration.
            comp->config.struct_size = sizeof(SaoCompositorConfig);
        } else {
            comp->config.struct_size = sizeof(SaoCompositorConfig);
            comp->config.target_hz = 0;
            comp->config.enable_temporal_union = true;
            comp->config.enable_rgn_cache = true;
        }

        comp->render_thread = std::this_thread::get_id();
        HostCompositorClaimGuard host_claim;
        if (host != nullptr) {
            sao_status_t status = sao_ui_overlay_host_require_owner_thread(host);
            if (status != SAO_STATUS_OK)
                return status;
            void* const hwnd = sao_ui_overlay_host_hwnd(host);
            if (hwnd == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;

            status = host_claim.acquire(host, comp.get());
            if (status != SAO_STATUS_OK)
                return status;

            SaoD3d11DeviceConfig d3d_config{};
            status = sao_ui_d3d11_device_create(&d3d_config, &comp->d3d11_device);
            if (status != SAO_STATUS_OK)
                return status;

            SaoDcompBridgeConfig bridge_config{};
            bridge_config.hwnd = hwnd;
            bridge_config.d3d11_device = sao_ui_d3d11_device_ptr(comp->d3d11_device);
            bridge_config.alpha_mode = 1;
            bridge_config.buffer_count = 2;
            bridge_config.width = 1;
            bridge_config.height = 1;
            status = sao_ui_dcomp_bridge_create(host, &bridge_config, &comp->dcomp_bridge);
            if (status != SAO_STATUS_OK) {
                sao_ui_d3d11_device_destroy(comp->d3d11_device);
                return status;
            }
            status = sao_ui_z_order_manager_create(host, &comp->z_order);
            if (status != SAO_STATUS_OK) {
                sao_ui_dcomp_bridge_destroy(comp->dcomp_bridge);
                sao_ui_d3d11_device_destroy(comp->d3d11_device);
                return status;
            }
            status = sao_ui_overlay_host_set_hit_test(host, &compositor_host_hit_test, comp.get());
            if (status == SAO_STATUS_OK)
                status = sao_ui_overlay_host_set_mouse(host, &compositor_host_mouse, comp.get());
            if (status != SAO_STATUS_OK) {
                (void)sao_ui_overlay_host_set_mouse(host, nullptr, nullptr);
                (void)sao_ui_overlay_host_set_hit_test(host, nullptr, nullptr);
                sao_ui_z_order_manager_destroy(comp->z_order);
                sao_ui_dcomp_bridge_destroy(comp->dcomp_bridge);
                sao_ui_d3d11_device_destroy(comp->d3d11_device);
                return status;
            }
            comp->host_callbacks_bound = true;
        }

        register_compositor(comp.get());
        *out_handle = comp.release();
        input_state.release();
        host_claim.commit();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_destroy_preflight(sao_ui_compositor_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_OK;
    const sao_status_t owner_status = sao_ui_compositor_require_owner_thread(handle);
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    if (handle->present_in_progress.load(std::memory_order_acquire))
        return SAO_STATUS_ERR_CANCELLED;
    try {
        std::lock_guard<std::mutex> lock(handle->mtx);
        const auto callback_active = [](const auto& layer) {
            return layer->input_callbacks_in_flight != 0;
        };
        return std::ranges::any_of(handle->layers, callback_active) ||
                       std::ranges::any_of(handle->pending_layer_destroys, callback_active)
                   ? SAO_STATUS_ERR_CANCELLED
                   : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_try_destroy(
    sao_ui_compositor_handle_t handle) {
    const sao_status_t preflight_status = sao_ui_compositor_destroy_preflight(handle);
    if (preflight_status != SAO_STATUS_OK)
        return preflight_status;
    if (handle == nullptr)
        return SAO_STATUS_OK;
    if (g_fail_next_compositor_destroy_after_preflight.exchange(false,
                                                                std::memory_order_acq_rel)) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    sao_ui_dcomp_bridge_handle_t dcomp_bridge = nullptr;
    sao_ui_d3d11_device_handle_t d3d11_device = nullptr;
    try {
        if (handle->host != nullptr) {
#if defined(_WIN32)
            const auto host_hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(handle->host));
            if (host_hwnd != nullptr && ::GetCapture() == host_hwnd)
                (void)::ReleaseCapture();
#endif
            const sao_status_t status =
                sao::ui::input_router_detail::reset_host_input(handle->host);
            if (status != SAO_STATUS_OK)
                return status;
        }
        if (handle->host_callbacks_bound) {
            sao_status_t status = sao_ui_overlay_host_set_mouse(handle->host, nullptr, nullptr);
            if (status != SAO_STATUS_OK)
                return status;
            status = sao_ui_overlay_host_set_hit_test(handle->host, nullptr, nullptr);
            if (status != SAO_STATUS_OK)
                return status;
            handle->host_callbacks_bound = false;
        }
        std::lock_guard<std::mutex> lock(handle->mtx);
        sao::ui::input_router_detail::reset_layer_input(handle->input_state);
        sao_ui_z_order_manager_destroy(handle->z_order);
        flush_pending_layer_destroys_locked(handle);
#if defined(_WIN32)
        for (const auto& layer : handle->layers) {
            release_shared_texture_objects(layer.get());
            release_layer_gpu_surface(layer.get());
        }
        release_master_gpu_resources(handle);
#endif
        handle->layers.clear();
        handle->pending_layer_destroys.clear();
        handle->released_pending_count = 0;
        dcomp_bridge = std::exchange(handle->dcomp_bridge, nullptr);
        d3d11_device = std::exchange(handle->d3d11_device, nullptr);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    sao_ui_dcomp_bridge_destroy(dcomp_bridge);
    sao_ui_d3d11_device_destroy(d3d11_device);
    sao::ui::input_router_detail::destroy_layer_input_state(handle->input_state);
    handle->input_state = nullptr;
    const sao_ui_overlay_host_handle_t claimed_host = handle->host;
    unregister_compositor(handle);
    release_compositor_host_claim(claimed_host, handle);
    delete handle;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_compositor_destroy(
    sao_ui_compositor_handle_t handle) {
    (void)sao_ui_compositor_try_destroy(handle);
}

extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_test_fail_next_compositor_destroy_after_preflight(void) {
    g_fail_next_compositor_destroy_after_preflight.store(true, std::memory_order_release);
}

extern "C" sao_ui_overlay_host_handle_t SAO_UI_CALL sao_ui_compositor_host(
    sao_ui_compositor_handle_t handle) {
    return handle == nullptr ? nullptr : handle->host;
}

extern "C" void* SAO_UI_CALL sao_ui_compositor_host_hwnd(
    sao_ui_compositor_handle_t handle) {
    return handle == nullptr || handle->host == nullptr
        ? nullptr
        : sao_ui_overlay_host_hwnd(handle->host);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_host_dpi(
    sao_ui_compositor_handle_t compositor,
    uint32_t* out_dpi_x,
    uint32_t* out_dpi_y) {
    // Both axes fall back to the standard 96 DPI when no host is attached or
    // the host has not yet received WM_DPICHANGED. Windows exposes a single
    // scalar DPI per HWND (multi-monitor per-monitor DPI aware v2 still
    // reports one scalar to the top-level window), so x and y agree here.
    uint32_t dpi = 96u;
    if (compositor != nullptr && compositor->host != nullptr) {
        const uint32_t reported = sao_ui_overlay_host_current_dpi(compositor->host);
        if (reported != 0u) {
            dpi = reported;
        }
    }
    if (out_dpi_x != nullptr) *out_dpi_x = dpi;
    if (out_dpi_y != nullptr) *out_dpi_y = dpi;
    return SAO_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Layer lifecycle.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_create(
    sao_ui_compositor_handle_t compositor,
    const SaoLayerConfig* config,
    sao_ui_layer_handle_t* out_layer) {
    if (out_layer == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_layer = nullptr;
    if (compositor == nullptr || config == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    // ABI v1 guard (see SAO_UI_LAYER_CONFIG_V1_SIZE). struct_size == 0 keeps
    // legacy callers working; other values must fit within the platform's
    // compiled struct.
    {
        const uint32_t declared =
            config->struct_size == 0u ? SAO_UI_LAYER_CONFIG_V1_SIZE : config->struct_size;
        if (declared < SAO_UI_LAYER_CONFIG_V1_SIZE ||
            declared > sizeof(SaoLayerConfig)) {
            return SAO_STATUS_ERR_ABI_MISMATCH;
        }
    }
    if (config->name_utf8 == nullptr || config->name_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!valid_composition_extent(config->x, config->y,
                                  config->width, config->height)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        std::lock_guard<std::mutex> lk(compositor->mtx);

        // Header rule "layer name-reuse leak" §7: reject duplicate names
        // rather than silently overwrite. Callers must destroy first.
        const std::string requested_name = config->name_utf8;
        for (const auto& p : compositor->layers) {
            if (p->name == requested_name) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
        }

        auto layer = std::make_unique<sao_ui_layer_s>();
        layer->name = requested_name;
        layer->x = config->x;
        layer->y = config->y;
        layer->width = config->width;
        layer->height = config->height;
        layer->z_order = config->z_order;
        layer->click_through = config->click_through;
        layer->rect_hit = config->rect_hit;
        layer->bgra_swizzle = config->bgra_swizzle;
        layer->high_fps = config->high_fps;
        layer->target_fps = config->target_fps;
        layer->effects.struct_size = sizeof(SaoUiLayerEffects);
        layer->owner = compositor;
        layer->creation_seq = ++compositor->seq;

        sao_ui_layer_handle_t out = layer.get();
        compositor->layers.push_back(std::move(layer));

        // LayerLess includes creation_seq, so non-allocating sort remains
        // deterministic for equal z-order values.
        std::sort(compositor->layers.begin(), compositor->layers.end(),
                  LayerLess{});

        *out_layer = out;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_layer_destroy(sao_ui_layer_handle_t layer) {
    if (layer == nullptr) return;

    try {
        sao_ui_compositor_s* comp = layer->owner;
        if (comp == nullptr) {
            delete layer;
            return;
        }

        InputCallbackInvocation leave{};
        bool invoke_leave = false;
        {
            std::lock_guard<std::mutex> lk(comp->mtx);
            auto it = find_layer_it(comp, layer);
            if (it == comp->layers.end()) return;
            std::array<sao::ui::input_router_detail::LayerInputAction, 1> actions{};
            size_t action_count = 0;
            if (sao::ui::input_router_detail::invalidate_layer_input(
                    comp->input_state, layer, false, actions.data(), actions.size(),
                    &action_count) == SAO_STATUS_OK &&
                action_count != 0) {
                invoke_leave = capture_router_action_locked(comp, actions[0], 0, 0, &leave);
            }
            comp->pending_layer_destroys.push_back(std::move(*it));
            comp->layers.erase(it);
        }
        if (invoke_leave) {
            if (std::this_thread::get_id() != comp->render_thread) {
                std::lock_guard lock(comp->mtx);
                comp->pending_owner_input_callbacks.push_back(std::move(leave));
            } else {
                (void)invoke_input_callback(leave);
            }
        }
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Layer state mutators used by production and tests.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_z_order(
    sao_ui_layer_handle_t layer, int32_t z_order) {
    return with_active_layer_locked(
        layer, [z_order](sao_ui_compositor_s* comp, sao_ui_layer_s* active) {
            active->z_order = z_order;
            std::stable_sort(comp->layers.begin(), comp->layers.end(),
                             LayerLess{});
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_effects(
    sao_ui_layer_handle_t layer, const SaoUiLayerEffects* effects) {
    if (layer == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    SaoUiLayerEffects candidate{};
    candidate.struct_size = sizeof(SaoUiLayerEffects);
    if (effects != nullptr) {
        candidate = *effects;
        if (!sao::ui::effects::validate(candidate))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        candidate.struct_size = sizeof(SaoUiLayerEffects);
    }
    return with_active_layer_locked(
        layer, [&candidate](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->effects = candidate;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_get_effects(
    sao_ui_layer_handle_t layer, SaoUiLayerEffects* out_effects) {
    if (layer == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_effects == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    return with_active_layer_locked(
        layer, [out_effects](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            *out_effects = active->effects;
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_visible(
    sao_ui_layer_handle_t layer, bool visible) {
    return mutate_input_layer(
        layer, [visible](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->visible = visible;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

// ---------------------------------------------------------------------------
// Enumeration -- serves the "get_layer_count" idiom via NULL out_layers[].
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_list_layers(
    sao_ui_compositor_handle_t compositor,
    sao_ui_layer_handle_t* out_layers, size_t capacity,
    size_t* out_count) {
    if (compositor == nullptr || out_count == nullptr) {
        if (out_count != nullptr) *out_count = 0;
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        std::lock_guard<std::mutex> lk(compositor->mtx);
        const size_t n = compositor->layers.size();
        *out_count = n;

        if (out_layers == nullptr) {
            // Header docs: pass NULL to just query count.  Return OK so the
            // caller can distinguish from "invalid arg".
            return SAO_STATUS_OK;
        }
        const size_t emit = (n < capacity) ? n : capacity;
        for (size_t i = 0; i < emit; ++i) {
            out_layers[i] = compositor->layers[i].get();
        }
        if (emit < n) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        *out_count = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Pixel upload and layer-control exports matching the header contract.
// ---------------------------------------------------------------------------

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_update_bgra(
    sao_ui_layer_handle_t layer,
    const uint8_t* bgra_pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride) {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const uint64_t minimum_stride = static_cast<uint64_t>(width) * 4u;
    if (bgra_pixels == nullptr || width == 0 || height == 0 ||
        width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        minimum_stride > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(stride) < minimum_stride) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sao_ui_compositor_s* comp = layer->owner;
    if (comp == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    if (static_cast<size_t>(height) >
        std::numeric_limits<size_t>::max() / static_cast<size_t>(stride)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const size_t copy_size = static_cast<size_t>(stride) * height;
    if (copy_size > kMaxBgraBufferBytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> snapshot;
    try {
        snapshot.resize(copy_size);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    std::memcpy(snapshot.data(), bgra_pixels, copy_size);

    return mutate_input_layer(
        layer, [width, height, stride, &snapshot](sao_ui_compositor_s*,
                                                  sao_ui_layer_s* active) {
            if (!valid_composition_extent(
                    active->x, active->y, width, height)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if ((active->shared_width != 0 && width < active->shared_width) ||
                (active->shared_height != 0 &&
                 height < active->shared_height)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            for (const auto& rect : active->input_rects) {
                if (static_cast<int64_t>(rect.x) + rect.width > width ||
                    static_cast<int64_t>(rect.y) + rect.height > height) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
            }
            active->bgra_pixels = std::move(snapshot);
            active->bgra_width = width;
            active->bgra_height = height;
            active->bgra_stride = stride;
            active->width = static_cast<int32_t>(width);
            active->height = static_cast<int32_t>(height);
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_mmf_source(
    sao_ui_layer_handle_t layer, const char* mmf_name_utf8) {
    return with_active_layer_locked(
        layer, [mmf_name_utf8](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            const std::string requested =
                mmf_name_utf8 == nullptr ? "" : mmf_name_utf8;
            if (!requested.empty() && active->mmf_name == requested) {
                return SAO_STATUS_OK;
            }
            active->mmf_name = requested;
            reset_mmf_generation(active);
            clear_bgra_cache(active);
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_shared_texture(
    sao_ui_layer_handle_t layer, void* shared_handle, uint32_t width, uint32_t height) {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (shared_handle != nullptr && (width == 0 || height == 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (shared_handle != nullptr) {
        size_t shared_bytes = 0;
        if (!checked_bgra_buffer_size(width, height, &shared_bytes)) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        (void)shared_bytes;
    }
    sao_ui_compositor_s* comp = layer->owner;
    return with_active_layer_locked(
        comp, layer,
        [shared_handle, width, height](sao_ui_compositor_s* owner,
                                       sao_ui_layer_s* active) {
            if (std::this_thread::get_id() != owner->render_thread) {
                return SAO_STATUS_ERR_ACCESS_DENIED;
            }
            if (shared_handle != nullptr &&
                 (!valid_composition_extent(
                     active->x, active->y, width, height) ||
                 width > static_cast<uint32_t>(active->width) ||
                 height > static_cast<uint32_t>(active->height))) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
#if defined(_WIN32)
            release_shared_texture_objects(active);
#endif
            clear_bgra_cache(active);
            active->shared_handle = shared_handle;
            active->shared_width = shared_handle == nullptr ? 0 : width;
            active->shared_height = shared_handle == nullptr ? 0 : height;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_render_fn(
    sao_ui_layer_handle_t layer, sao_ui_layer_render_fn_t fn, void* user_data) {
    return with_active_layer_locked(
        layer, [fn, user_data](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->render_fn = fn;
            active->render_user_data = user_data;
            active->redraw_requested = fn != nullptr;
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_d3d11_render_fn(
    sao_ui_layer_handle_t layer, sao_ui_layer_d3d11_render_fn_t fn, void* user_data) {
    if (layer == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_ui_compositor_s* owner = layer->owner;
    if (owner == nullptr || std::this_thread::get_id() != owner->render_thread)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    return with_active_layer_locked(owner, layer,
                                    [fn, user_data](sao_ui_compositor_s*, sao_ui_layer_s* active) {
#if defined(_WIN32)
                                        release_layer_gpu_surface(active);
#endif
                                        active->d3d11_render_fn = fn;
                                        active->d3d11_render_user_data = user_data;
                                        active->redraw_requested = fn != nullptr;
                                        mark_layer_dirty(active);
                                        return SAO_STATUS_OK;
                                    });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_position(
    sao_ui_layer_handle_t layer, int32_t x, int32_t y) {
    return mutate_input_layer(
        layer, [x, y](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            const int64_t source_width = std::max<int64_t>(
                active->width,
                std::max(active->bgra_width, active->shared_width));
            const int64_t source_height = std::max<int64_t>(
                active->height,
                std::max(active->bgra_height, active->shared_height));
                if (!valid_composition_extent(
                    x, y, source_width, source_height)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            active->x = x;
            active->y = y;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_geometry(
    sao_ui_layer_handle_t layer,
    int32_t x, int32_t y, int32_t width, int32_t height) {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_composition_extent(x, y, width, height)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return mutate_input_layer(
        layer, [x, y, width, height](sao_ui_compositor_s*,
                                     sao_ui_layer_s* active) {
            if ((active->bgra_width != 0 &&
                 static_cast<uint32_t>(width) < active->bgra_width) ||
                (active->bgra_height != 0 &&
                  static_cast<uint32_t>(height) < active->bgra_height) ||
                 (active->shared_width != 0 &&
                  static_cast<uint32_t>(width) < active->shared_width) ||
                 (active->shared_height != 0 &&
                  static_cast<uint32_t>(height) < active->shared_height)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            for (const auto& rect : active->input_rects) {
                if (static_cast<int64_t>(rect.x) + rect.width > width ||
                    static_cast<int64_t>(rect.y) + rect.height > height) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
            }
            active->x = x;
            active->y = y;
            active->width = width;
            active->height = height;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_alpha(
    sao_ui_layer_handle_t layer, float alpha) {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return mutate_input_layer(
        layer, [alpha](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->alpha = alpha;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_start_fade(
    sao_ui_layer_handle_t layer, float target_alpha, float duration_sec,
    sao_ui_layer_fade_done_fn_t done_fn, void* user_data) {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(target_alpha) || !std::isfinite(duration_sec) ||
        target_alpha < 0.0f || target_alpha > 1.0f || duration_sec < 0.0f) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    return with_active_layer_locked(
        layer, [target_alpha, duration_sec, done_fn, user_data](
                   sao_ui_compositor_s* comp, sao_ui_layer_s* active) {
            if (active->fade_active && active->fade_done_fn != nullptr) {
                comp->pending_fade_callbacks.push_back(
                    {active->fade_done_fn, active->fade_done_user_data});
            }
            active->fade_from = active->alpha;
            active->fade_target = target_alpha;
            active->fade_duration_sec = duration_sec;
            active->fade_started = std::chrono::steady_clock::now();
            active->fade_done_fn = done_fn;
            active->fade_done_user_data = user_data;
            active->fade_active = true;
            active->redraw_requested = true;
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_input_enabled(
    sao_ui_layer_handle_t layer, bool enabled) {
    return mutate_input_layer(
        layer, [enabled](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->input_enabled = enabled;
            return SAO_STATUS_OK;
        });
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_layer_set_input_policy(
    sao_ui_layer_handle_t layer, bool click_through,
    bool input_enabled) {
    return mutate_input_layer(
        layer, [click_through, input_enabled](
                   sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->click_through = click_through;
            active->input_enabled = input_enabled;
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_input_rects(
    sao_ui_layer_handle_t layer,
    const SaoUiLayerInputRect* rects,
    size_t count) {
    if (layer == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if ((rects == nullptr && count != 0) || count > 4096) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    sao_ui_compositor_s* comp = layer->owner;
    if (comp == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;

    std::vector<SaoUiLayerInputRect> snapshot;
    try {
        if (count != 0) snapshot.assign(rects, rects + count);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    return mutate_input_layer(
        layer, [&snapshot](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            for (const auto& rect : snapshot) {
                const int64_t right =
                    static_cast<int64_t>(rect.x) + rect.width;
                const int64_t bottom =
                    static_cast<int64_t>(rect.y) + rect.height;
                if (rect.x < 0 || rect.y < 0 || rect.width <= 0 ||
                    rect.height <= 0 || right > active->width ||
                    bottom > active->height) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
            }
            active->input_rects = std::move(snapshot);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_request_redraw(
    sao_ui_layer_handle_t layer) {
    return with_active_layer_locked(
        layer, [](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->redraw_requested = true;
            mark_layer_dirty(active);
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_input_callbacks(
    sao_ui_layer_handle_t layer,
    sao_ui_layer_cursor_pos_fn_t cursor_pos_fn,
    sao_ui_layer_cursor_leave_fn_t cursor_leave_fn,
    sao_ui_layer_button_fn_t button_fn,
    sao_ui_layer_scroll_fn_t scroll_fn,
    void* user_data) {
    if (layer == nullptr || layer->owner == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* compositor = layer->owner;
    try {
        for (;;) {
            std::unique_lock lock(compositor->mtx);
            if (find_layer_it(compositor, layer) == compositor->layers.end())
                return SAO_STATUS_ERR_HANDLE_INVALID;
            const bool pending_owner_callback = std::ranges::any_of(
                compositor->pending_owner_input_callbacks,
                [layer](const InputCallbackInvocation& invocation) {
                    return invocation.layer == layer;
                });
            if (pending_owner_callback)
                return SAO_STATUS_ERR_CANCELLED;
            std::vector<uint64_t> generations_to_drain;
            generations_to_drain.reserve(layer->input_callbacks_by_generation.size());
            for (const auto& [generation, count] : layer->input_callbacks_by_generation) {
                const size_t active_here = input_callback_active_count(layer, generation);
                if (active_here != 0 && count > active_here)
                    return SAO_STATUS_ERR_CANCELLED;
                if (active_here == 0 && count != 0)
                    generations_to_drain.push_back(generation);
            }
            if (!generations_to_drain.empty()) {
                compositor->input_callbacks_idle.wait(lock, [&] {
                    return std::ranges::none_of(generations_to_drain, [&](uint64_t generation) {
                        const auto found = layer->input_callbacks_by_generation.find(generation);
                        return found != layer->input_callbacks_by_generation.end() &&
                               found->second != 0;
                    });
                });
            }
            for (auto it = layer->input_callbacks_by_generation.begin();
                 it != layer->input_callbacks_by_generation.end();) {
                if (it->second == 0 && it->first != layer->input_callback_generation)
                    it = layer->input_callbacks_by_generation.erase(it);
                else
                    ++it;
            }
            uint64_t next_generation = layer->input_callback_generation;
            do {
                ++next_generation;
                if (next_generation == 0)
                    next_generation = 1;
            } while (layer->input_callbacks_by_generation.contains(next_generation));
            layer->input_callbacks_by_generation.emplace(next_generation, 0);
            layer->input_callback_generation = next_generation;
            layer->cursor_pos_fn = cursor_pos_fn;
            layer->cursor_leave_fn = cursor_leave_fn;
            layer->button_fn = button_fn;
            layer->scroll_fn = scroll_fn;
            layer->input_user_data = user_data;
            return SAO_STATUS_OK;
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_hit_test(
    sao_ui_compositor_handle_t compositor, int32_t screen_x, int32_t screen_y, bool* out_hit) {
    if (out_hit == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_hit = false;
    if (compositor == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        int32_t host_x = 0;
        int32_t host_y = 0;
        const sao_status_t coordinate_status =
            screen_to_host(compositor, screen_x, screen_y, &host_x, &host_y);
        if (coordinate_status != SAO_STATUS_OK)
            return coordinate_status;
        std::lock_guard lock(compositor->mtx);
        *out_hit = top_input_layer_locked(compositor, host_x, host_y, nullptr, nullptr) != nullptr;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_dispatch_mouse(
    sao_ui_compositor_handle_t compositor, uint32_t message, int32_t screen_x, int32_t screen_y,
    int32_t button, int32_t wheel_delta) {
    if (compositor == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != compositor->render_thread)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (message != kMouseMove && message != kMouseLeave && message != kMouseWheel &&
        message != kCaptureChanged && message != kCancelMode &&
        message != kLeftButtonDown && message != kLeftButtonUp &&
        message != kLeftButtonDoubleClick && message != kRightButtonDown &&
        message != kRightButtonUp && message != kRightButtonDoubleClick &&
        message != kMiddleButtonDown && message != kMiddleButtonUp &&
        message != kMiddleButtonDoubleClick) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    try {
        {
            std::lock_guard lock(compositor->mtx);
            ++compositor->input_dispatch_depth;
        }
        bool callback_failed = drain_owner_input_callbacks(compositor);
        int32_t host_x = 0;
        int32_t host_y = 0;
        sao_status_t dispatch_status = SAO_STATUS_OK;
        if (message != kMouseLeave && message != kCaptureChanged && message != kCancelMode) {
            const sao_status_t coordinate_status =
                screen_to_host(compositor, screen_x, screen_y, &host_x, &host_y);
            if (coordinate_status != SAO_STATUS_OK)
                dispatch_status = coordinate_status;
        }

        std::vector<InputCallbackInvocation> invocations;
        invocations.reserve(3);
        if (dispatch_status == SAO_STATUS_OK) {
            std::lock_guard lock(compositor->mtx);
            float hit_x = 0.0F;
            float hit_y = 0.0F;
            sao_ui_layer_s* hit_target = nullptr;
            if (sao::ui::input_router_detail::layer_event_uses_coordinates(message)) {
                hit_target =
                    top_input_layer_locked(compositor, host_x, host_y, &hit_x, &hit_y);
            }
            std::array<sao::ui::input_router_detail::LayerInputAction, 3> actions{};
            size_t action_count = 0;
            dispatch_status = sao::ui::input_router_detail::route_layer_input(
                compositor->input_state, message, host_x, host_y, button, wheel_delta, hit_target,
                hit_x, hit_y, actions.data(), actions.size(), &action_count);
            if (dispatch_status == SAO_STATUS_OK) {
                for (size_t index = 0; index < action_count; ++index) {
                    InputCallbackInvocation invocation{};
                    if (capture_router_action_locked(compositor, actions[index], host_x, host_y,
                                                     &invocation)) {
                        invocations.push_back(invocation);
                    }
                }
            }
        }
        for (const auto& invocation : invocations) {
            callback_failed = !invoke_input_callback(invocation) || callback_failed;
        }
        callback_failed = drain_owner_input_callbacks(compositor) || callback_failed;
        auto tasks = end_input_dispatch(compositor);
        const sao_status_t result =
            dispatch_status != SAO_STATUS_OK
                ? dispatch_status
                : (callback_failed ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK);
        run_post_input_tasks(std::move(tasks));
        return result;
    } catch (...) {
        auto tasks = end_input_dispatch(compositor);
        run_post_input_tasks(std::move(tasks));
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_post_input(
    sao_ui_compositor_handle_t compositor, sao_ui_compositor_post_input_fn_t fn,
    void* user_data) {
    if (compositor == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (fn == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const sao_status_t owner_status = sao_ui_compositor_require_owner_thread(compositor);
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    try {
        {
            std::lock_guard lock(compositor->mtx);
            if (compositor->input_dispatch_depth != 0) {
                compositor->post_input_tasks.push_back({fn, user_data});
                return SAO_STATUS_OK;
            }
        }
        fn(user_data);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_current_input_position(
    float* out_layer_x, float* out_layer_y) {
    if (out_layer_x == nullptr || out_layer_y == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (g_active_input_invocation == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    *out_layer_x = g_active_input_invocation->x;
    *out_layer_y = g_active_input_invocation->y;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_enable_input_proxy(
    sao_ui_layer_handle_t layer) {
    if (!sao::ui::input_router_detail::legacy_tk_input_enabled())
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    return mutate_input_layer(
        layer, [](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->input_proxy_enabled = true;
            active->input_enabled = true;
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_disable_input_proxy(
    sao_ui_layer_handle_t layer) {
    return mutate_input_layer(
        layer, [](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->input_proxy_enabled = false;
            active->input_enabled = false;
            return SAO_STATUS_OK;
        });
}

sao_status_t compositor_present_impl(
    sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != compositor->render_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (compositor->present_in_progress.exchange(
            true, std::memory_order_acq_rel)) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    const PresentGuard present_guard{&compositor->present_in_progress};
    {
        std::lock_guard<std::mutex> lk(compositor->mtx);
        flush_pending_layer_destroys_locked(compositor);
    }
    const bool callback_failed =
        advance_layer_animations_and_callbacks(compositor);
#if defined(_WIN32)
    // A failed device-loss recovery leaves the bridge null. Retry on later
    // owner-thread presents so a transient adapter/ResizeBuffers failure does
    // not strand the process behind the LinkStart surface indefinitely.
    if (compositor->d3d11_device != nullptr && compositor->dcomp_bridge == nullptr &&
        compositor->host != nullptr) {
        {
            std::lock_guard<std::mutex> lock(compositor->mtx);
            for (const auto& layer : compositor->layers) {
                release_shared_texture_objects(layer.get());
                release_layer_gpu_surface(layer.get());
            }
            release_master_gpu_resources(compositor);
        }
        uint32_t removed_reason = 0;
        sao_status_t recovery_status =
            sao_ui_d3d11_device_recreate(compositor->d3d11_device, &removed_reason);
        if (recovery_status == SAO_STATUS_OK) {
            SaoDcompBridgeConfig bridge_config{};
            bridge_config.hwnd = sao_ui_overlay_host_hwnd(compositor->host);
            bridge_config.d3d11_device = sao_ui_d3d11_device_ptr(compositor->d3d11_device);
            bridge_config.alpha_mode = 1;
            bridge_config.buffer_count = 2;
            bridge_config.width = std::max(1u, compositor->last_present_width);
            bridge_config.height = std::max(1u, compositor->last_present_height);
            recovery_status = sao_ui_dcomp_bridge_create(compositor->host, &bridge_config,
                                                         &compositor->dcomp_bridge);
        }
        if (recovery_status != SAO_STATUS_OK)
            return recovery_status;
    }
#endif
    if (compositor->dcomp_bridge == nullptr ||
        compositor->d3d11_device == nullptr) {
        return callback_failed ? SAO_STATUS_ERR_UNKNOWN
                               : SAO_STATUS_ERR_NOT_INITIALIZED;
    }

#if defined(_WIN32)
    bool use_native_gpu = false;
    {
        std::lock_guard<std::mutex> lock(compositor->mtx);
        use_native_gpu = has_visible_native_layer_locked(compositor);
    }
    if (use_native_gpu) {
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        uint32_t gpu_width = 0;
        uint32_t gpu_height = 0;
        auto compose_and_present = [&]() -> sao_status_t {
            sao_status_t gpu_status = SAO_STATUS_OK;
            {
                std::lock_guard<std::mutex> lock(compositor->mtx);
                gpu_status =
                    compose_native_layers_gpu_locked(compositor, seconds, &gpu_width, &gpu_height);
            }
            if (gpu_status != SAO_STATUS_OK)
                return gpu_status;
            if (gpu_width == 0 || gpu_height == 0)
                return SAO_STATUS_OK;
            gpu_status = sao_ui_dcomp_bridge_copy_texture(
                compositor->dcomp_bridge, compositor->gpu_master_texture, gpu_width, gpu_height);
            if (gpu_status == SAO_STATUS_OK)
                gpu_status = sao_ui_dcomp_bridge_present(compositor->dcomp_bridge);
            return gpu_status;
        };
        sao_status_t gpu_status = compose_and_present();
        if (gpu_status == SAO_STATUS_ERR_DEVICE_LOST) {
            {
                std::lock_guard<std::mutex> lock(compositor->mtx);
                for (const auto& layer : compositor->layers) {
                    release_shared_texture_objects(layer.get());
                    release_layer_gpu_surface(layer.get());
                }
                release_master_gpu_resources(compositor);
            }
            sao_ui_dcomp_bridge_destroy(compositor->dcomp_bridge);
            compositor->dcomp_bridge = nullptr;
            uint32_t removed_reason = 0;
            gpu_status = sao_ui_d3d11_device_recreate(compositor->d3d11_device, &removed_reason);
            if (gpu_status == SAO_STATUS_OK) {
                SaoDcompBridgeConfig bridge_config{};
                bridge_config.hwnd = sao_ui_overlay_host_hwnd(compositor->host);
                bridge_config.d3d11_device = sao_ui_d3d11_device_ptr(compositor->d3d11_device);
                bridge_config.alpha_mode = 1;
                bridge_config.buffer_count = 2;
                bridge_config.width = gpu_width;
                bridge_config.height = gpu_height;
                gpu_status = sao_ui_dcomp_bridge_create(compositor->host, &bridge_config,
                                                        &compositor->dcomp_bridge);
            }
            if (gpu_status == SAO_STATUS_OK)
                gpu_status = compose_and_present();
        }
        if (gpu_status == SAO_STATUS_OK) {
            std::lock_guard<std::mutex> lock(compositor->mtx);
            compositor->presented_visible_content = gpu_width != 0 && gpu_height != 0;
            compositor->last_present_width = gpu_width;
            compositor->last_present_height = gpu_height;
            for (const auto& layer : compositor->layers)
                layer->bgra_dirty = false;
        }
        if (gpu_status != SAO_STATUS_OK)
            return gpu_status;
        return callback_failed ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK;
    }
#endif

    struct PresentedRevision {
        sao_ui_layer_s* layer;
        uint64_t revision;
    };
    std::vector<uint8_t> composed_pixels;
    std::vector<PresentedRevision> presented_revisions;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_geometry_buffer = false;
    bool has_visible_alpha = false;
    void* const d3d11_device_ptr =
        sao_ui_d3d11_device_ptr(compositor->d3d11_device);
    {
        std::lock_guard<std::mutex> lk(compositor->mtx);
        has_geometry_buffer = compose_premultiplied_bgra_locked(
            compositor, d3d11_device_ptr, &composed_pixels, &width, &height, &has_visible_alpha, 0u,
            compositor->layers.size());
        if (!has_visible_alpha) {
            if (!compositor->presented_visible_content) {
                if (!callback_failed) {
                    for (const auto& layer : compositor->layers) {
                        layer->bgra_dirty = false;
                    }
                }
                return callback_failed ? SAO_STATUS_ERR_UNKNOWN
                                       : SAO_STATUS_OK;
            }
            width = compositor->last_present_width;
            height = compositor->last_present_height;
            if (width == 0 || height == 0) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
            size_t clear_size = 0;
            if (!checked_bgra_buffer_size(width, height, &clear_size)) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
            composed_pixels.assign(clear_size, 0u);
        }
        for (const auto& layer : compositor->layers) {
            presented_revisions.push_back(
                {layer.get(), layer->visual_revision});
        }
    }

    sao_status_t status = sao_ui_dcomp_bridge_upload_bgra(
        compositor->dcomp_bridge, composed_pixels.data(), width, height,
        width * 4u);
    if (status == SAO_STATUS_OK) {
        status = sao_ui_dcomp_bridge_present(compositor->dcomp_bridge);
    }
    if (status != SAO_STATUS_ERR_DEVICE_LOST) {
        if (status == SAO_STATUS_OK) {
            std::lock_guard<std::mutex> lk(compositor->mtx);
            compositor->presented_visible_content = has_visible_alpha;
            if (has_visible_alpha && has_geometry_buffer) {
                compositor->last_present_width = width;
                compositor->last_present_height = height;
            }
            for (const auto& presented : presented_revisions) {
                const auto it = find_layer_it(compositor, presented.layer);
                if (it != compositor->layers.end() &&
                    (*it)->visual_revision == presented.revision) {
                    (*it)->bgra_dirty = false;
                }
            }
        }
        if (status != SAO_STATUS_OK) return status;
        return callback_failed ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK;
    }

    // Device loss invalidates every borrowed COM pointer. Drop layer caches
    // and the DComp tree before recreating the shared device and bridge.
    {
        std::lock_guard<std::mutex> lock(compositor->mtx);
#if defined(_WIN32)
        for (const auto& layer : compositor->layers) {
            release_shared_texture_objects(layer.get());
            release_layer_gpu_surface(layer.get());
        }
        release_master_gpu_resources(compositor);
#endif
    }
    sao_ui_dcomp_bridge_destroy(compositor->dcomp_bridge);
    compositor->dcomp_bridge = nullptr;
    uint32_t removed_reason = 0;
    status = sao_ui_d3d11_device_recreate(
        compositor->d3d11_device, &removed_reason);
    if (status != SAO_STATUS_OK) {
        return status;
    }

    SaoDcompBridgeConfig bridge_config{};
    bridge_config.hwnd = sao_ui_overlay_host_hwnd(compositor->host);
    bridge_config.d3d11_device =
        sao_ui_d3d11_device_ptr(compositor->d3d11_device);
    bridge_config.alpha_mode = 1;
    bridge_config.buffer_count = 2;
    bridge_config.width = width;
    bridge_config.height = height;
    status = sao_ui_dcomp_bridge_create(
        compositor->host, &bridge_config, &compositor->dcomp_bridge);
    if (status != SAO_STATUS_OK) {
        return status;
    }
    status = sao_ui_dcomp_bridge_upload_bgra(
        compositor->dcomp_bridge, composed_pixels.data(), width, height,
        width * 4u);
    if (status != SAO_STATUS_OK) {
        return status;
    }
    status = sao_ui_dcomp_bridge_present(compositor->dcomp_bridge);
    if (status == SAO_STATUS_OK) {
        std::lock_guard<std::mutex> lk(compositor->mtx);
        compositor->presented_visible_content = has_visible_alpha;
        if (has_visible_alpha && has_geometry_buffer) {
            compositor->last_present_width = width;
            compositor->last_present_height = height;
        }
        for (const auto& presented : presented_revisions) {
            const auto it = find_layer_it(compositor, presented.layer);
            if (it != compositor->layers.end() &&
                (*it)->visual_revision == presented.revision) {
                (*it)->bgra_dirty = false;
            }
        }
    }
    if (status != SAO_STATUS_OK) return status;
    return callback_failed ? SAO_STATUS_ERR_UNKNOWN : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_present(
    sao_ui_compositor_handle_t compositor) {
    try {
        if (compositor == nullptr)
            return SAO_STATUS_ERR_HANDLE_INVALID;
        if (std::this_thread::get_id() != compositor->render_thread)
            return SAO_STATUS_ERR_ACCESS_DENIED;
        const auto pending = flush_pending_owner_input(compositor);
        if (pending.ran_post_tasks || pending.status != SAO_STATUS_OK)
            return pending.status;
        return compositor_present_impl(compositor);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_tick(
    sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != compositor->render_thread)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    try {
        if (compositor->host != nullptr) {
            const sao_status_t host_status =
                sao_ui_overlay_host_pump_messages(compositor->host);
            if (host_status != SAO_STATUS_OK)
                return host_status;
        }
        const auto pending = flush_pending_owner_input(compositor);
        if (pending.ran_post_tasks || pending.status != SAO_STATUS_OK)
            return pending.status;
        sao_status_t status = compositor_present_impl(compositor);
        const auto merge = [&status](sao_status_t candidate) {
            if (status == SAO_STATUS_OK && candidate != SAO_STATUS_OK)
                status = candidate;
        };
        merge(sao_ui_compositor_sync_host_rgn(compositor));
        merge(sao_ui_compositor_sync_host_input_mode(compositor));
        merge(sao_ui_compositor_enforce_z_order(compositor));
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_compose_snapshot_(
    sao_ui_compositor_handle_t compositor,
    uint8_t* out_bgra_pixels,
    size_t capacity,
    uint32_t* out_width,
    uint32_t* out_height,
    size_t* out_bytes) {
    if (compositor == nullptr || out_width == nullptr || out_height == nullptr ||
        out_bytes == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    *out_width = 0;
    *out_height = 0;
    *out_bytes = 0;

    try {
#if defined(_WIN32)
        bool native_visible = false;
        {
            std::lock_guard<std::mutex> lock(compositor->mtx);
            native_visible = has_visible_native_layer_locked(compositor);
        }
        if (native_visible) {
            if (std::this_thread::get_id() != compositor->render_thread)
                return SAO_STATUS_ERR_ACCESS_DENIED;
            std::vector<uint8_t> gpu_pixels;
            const float seconds =
                std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            sao_status_t gpu_status = SAO_STATUS_OK;
            {
                std::lock_guard<std::mutex> lock(compositor->mtx);
                gpu_status =
                    compose_native_layers_gpu_locked(compositor, seconds, out_width, out_height);
            }
            if (gpu_status != SAO_STATUS_OK)
                return gpu_status;
            size_t required_bytes = 0;
            if (!checked_bgra_buffer_size(*out_width, *out_height, &required_bytes))
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            *out_bytes = required_bytes;
            if (out_bgra_pixels == nullptr || capacity < required_bytes)
                return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
            if (required_bytes != 0) {
                std::lock_guard<std::mutex> lock(compositor->mtx);
                gpu_status = readback_master_locked(compositor, &gpu_pixels);
            }
            if (gpu_status != SAO_STATUS_OK)
                return gpu_status;
            if (!gpu_pixels.empty())
                std::memcpy(out_bgra_pixels, gpu_pixels.data(), gpu_pixels.size());
            return SAO_STATUS_OK;
        }
#endif
        // This is the same composition function used by present. The caller
        // in capture_sync owns the capture lease; this function only composes.
        std::vector<uint8_t> composed_pixels;
        bool has_visible_alpha = false;
        {
            std::lock_guard<std::mutex> lock(compositor->mtx);
            if (!compose_premultiplied_bgra_locked(compositor, nullptr, &composed_pixels, out_width,
                                                   out_height, &has_visible_alpha, 0u,
                                                   compositor->layers.size())) {
                return SAO_STATUS_OK;
            }
        }

        *out_bytes = composed_pixels.size();
        if (out_bgra_pixels == nullptr || capacity < composed_pixels.size()) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(out_bgra_pixels, composed_pixels.data(),
                    composed_pixels.size());
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_enforce_z_order(
    sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (std::this_thread::get_id() != compositor->render_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    if (compositor->z_order == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    void* game = nullptr;
    {
        std::lock_guard<std::mutex> lock(compositor->mtx);
        game = compositor->game_hwnd;
    }
    bool game_present = false;
    bool game_is_topmost = false;
#if defined(_WIN32)
    const HWND game_hwnd = reinterpret_cast<HWND>(game);
    if (game_hwnd != nullptr && ::IsWindow(game_hwnd)) {
        game_present = true;
        game_is_topmost =
            (::GetWindowLongPtrW(game_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    }
#else
    (void)game;
#endif
    return sao_ui_z_order_enforce(compositor->z_order, game, game_is_topmost, game_present);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_set_game_hwnd(
    sao_ui_compositor_handle_t compositor, void* game_hwnd) {
    if (compositor == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(compositor->mtx);
    compositor->game_hwnd = game_hwnd;
    return SAO_STATUS_OK;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_require_owner_thread(sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard lock(g_compositor_registry_mutex);
        if (!compositor_registered_locked(compositor))
            return SAO_STATUS_ERR_HANDLE_INVALID;
        return std::this_thread::get_id() == compositor->render_thread
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_ACCESS_DENIED;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" SAO_UI_API uint64_t SAO_UI_CALL
sao_ui_compositor_test_input_writer_revision(sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr)
        return 0;
    try {
        std::lock_guard lock(compositor->mtx);
        return sao::ui::input_router_detail::layer_input_writer_revision(
            compositor->input_state);
    } catch (...) {
        return 0;
    }
}

namespace {

// RGN pad/settle constants (Python authority: overlay_compositor.py
// _RGN_PAD_STILL/_RGN_PAD_MOVE/_RGN_PAD_ANIM_MIN/_RGN_PAD_ANIM_CAP/
// _RGN_STATIC_SETTLE, tightened revamp values kept verbatim).
constexpr int32_t kRgnPadMove = 24;
constexpr int32_t kRgnPadMoveCap = 256;
constexpr int32_t kRgnPadAnimMin = 6;
constexpr int32_t kRgnPadAnimCap = 96;
constexpr uint32_t kRgnStaticSettleTicks = 3;
constexpr size_t kMaxMergedRegionRects = 4096;

using RgnRect = SaoOverlayHostInputRect;

void translate_rgn_rects(const std::vector<RgnRect>& source, int64_t dx, int64_t dy,
                         std::vector<RgnRect>* out) {
    for (const auto& rect : source) {
        out->push_back(RgnRect{static_cast<int32_t>(static_cast<int64_t>(rect.x) + dx),
                               static_cast<int32_t>(static_cast<int64_t>(rect.y) + dy),
                               rect.width, rect.height});
    }
}

// Lossless 2D rect merge: vertical merge only for identical x-ranges,
// horizontal merge only for identical y-ranges — the GDI union is exactly
// preserved in both cases (Python merges same-row touching spans; identical
// reasoning applied to the taller merged rects this port produces).
void merge_rgn_rects_lossless(std::vector<RgnRect>* rects) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t a = 0; a < rects->size() && !changed; ++a) {
            for (size_t b = a + 1; b < rects->size(); ++b) {
                RgnRect& r0 = (*rects)[a];
                const RgnRect& r1 = (*rects)[b];
                const int64_t r0_right = static_cast<int64_t>(r0.x) + r0.width;
                const int64_t r1_right = static_cast<int64_t>(r1.x) + r1.width;
                const int64_t r0_bottom = static_cast<int64_t>(r0.y) + r0.height;
                const int64_t r1_bottom = static_cast<int64_t>(r1.y) + r1.height;
                if (r0.x == r1.x && r0.width == r1.width && r0.y <= r1_bottom &&
                    r1.y <= r0_bottom) {
                    r0.y = static_cast<int32_t>(std::min<int64_t>(r0.y, r1.y));
                    r0.height = static_cast<int32_t>(std::max(r0_bottom, r1_bottom) - r0.y);
                } else if (r0.y == r1.y && r0.height == r1.height && r0.x <= r1_right &&
                           r1.x <= r0_right) {
                    r0.x = static_cast<int32_t>(std::min<int64_t>(r0.x, r1.x));
                    r0.width = static_cast<int32_t>(std::max(r0_right, r1_right) - r0.x);
                } else {
                    continue;
                }
                rects->erase(rects->begin() + static_cast<ptrdiff_t>(b));
                changed = true;
                break;
            }
        }
    }
}

// Pad every rect by `pad` on all sides, then lossless-merge.  Python
// authority: _pad_and_merge_row_spans — the pad halo is intentional
// predictive cover, the merge keeps the region data bounded.
void pad_merge_rgn_rects(const std::vector<RgnRect>& source, int32_t pad,
                         std::vector<RgnRect>* out) {
    const size_t begin = out->size();
    for (const auto& rect : source) {
        const int64_t left = static_cast<int64_t>(rect.x) - pad;
        const int64_t top = static_cast<int64_t>(rect.y) - pad;
        const int64_t right = static_cast<int64_t>(rect.x) + rect.width + pad;
        const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height + pad;
        constexpr int64_t kMin = std::numeric_limits<int32_t>::min();
        constexpr int64_t kMax = std::numeric_limits<int32_t>::max();
        if (left < kMin || top < kMin || right > kMax || bottom > kMax ||
            right - left > kMax || bottom - top > kMax) {
            out->push_back(rect);
            continue;
        }
        out->push_back(RgnRect{static_cast<int32_t>(left), static_cast<int32_t>(top),
                               static_cast<int32_t>(right - left),
                               static_cast<int32_t>(bottom - top)});
    }
    std::vector<RgnRect> work(out->begin() + static_cast<ptrdiff_t>(begin), out->end());
    merge_rgn_rects_lossless(&work);
    out->resize(begin);
    out->insert(out->end(), work.begin(), work.end());
}

// Max silhouette-motion step between two rect sets (Python authority:
// _span_row_extent_step).  Exact per-y-boundary evaluation: rows where only
// one side has coverage are skipped (Python's dict.get → None → continue),
// plus the vertical row-range delta.
int32_t rgn_row_extent_step(const std::vector<RgnRect>& cur,
                            const std::vector<RgnRect>& prev) {
    if (cur.empty() || prev.empty())
        return 0;
    std::vector<int64_t> bounds;
    bounds.reserve((cur.size() + prev.size()) * 2);
    for (const auto& rect : cur) {
        bounds.push_back(rect.y);
        bounds.push_back(static_cast<int64_t>(rect.y) + rect.height);
    }
    for (const auto& rect : prev) {
        bounds.push_back(rect.y);
        bounds.push_back(static_cast<int64_t>(rect.y) + rect.height);
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    const auto extent_at = [](const std::vector<RgnRect>& rects, int64_t y,
                              int64_t* out_min_x, int64_t* out_max_x) {
        bool found = false;
        int64_t min_x = 0;
        int64_t max_x = 0;
        for (const auto& rect : rects) {
            if (y < rect.y || y >= static_cast<int64_t>(rect.y) + rect.height)
                continue;
            const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
            if (!found) {
                min_x = rect.x;
                max_x = right;
                found = true;
            } else {
                min_x = std::min<int64_t>(min_x, rect.x);
                max_x = std::max<int64_t>(max_x, right);
            }
        }
        if (found) {
            *out_min_x = min_x;
            *out_max_x = max_x;
        }
        return found;
    };
    int64_t step = 0;
    for (size_t index = 0; index + 1 < bounds.size(); ++index) {
        const int64_t y = bounds[index];
        int64_t cur_min = 0;
        int64_t cur_max = 0;
        int64_t prev_min = 0;
        int64_t prev_max = 0;
        if (!extent_at(cur, y, &cur_min, &cur_max) ||
            !extent_at(prev, y, &prev_min, &prev_max)) {
            continue;
        }
        step = std::max(step, cur_min >= prev_min ? cur_min - prev_min
                                                  : prev_min - cur_min);
        step = std::max(step, cur_max >= prev_max ? cur_max - prev_max
                                                  : prev_max - cur_max);
    }
    const auto y_range = [](const std::vector<RgnRect>& rects, int64_t* out_min,
                            int64_t* out_max) {
        int64_t min_y = rects.front().y;
        int64_t max_y = static_cast<int64_t>(rects.front().y) + rects.front().height;
        for (const auto& rect : rects) {
            min_y = std::min<int64_t>(min_y, rect.y);
            max_y = std::max<int64_t>(max_y, static_cast<int64_t>(rect.y) + rect.height);
        }
        *out_min = min_y;
        *out_max = max_y;
    };
    int64_t cur_min_y = 0;
    int64_t cur_max_y = 0;
    int64_t prev_min_y = 0;
    int64_t prev_max_y = 0;
    y_range(cur, &cur_min_y, &cur_max_y);
    y_range(prev, &prev_min_y, &prev_max_y);
    step = std::max(step, cur_min_y >= prev_min_y ? cur_min_y - prev_min_y
                                                  : prev_min_y - cur_min_y);
    step = std::max(step, cur_max_y >= prev_max_y ? cur_max_y - prev_max_y
                                                  : prev_max_y - cur_max_y);
    return static_cast<int32_t>(std::min<int64_t>(step, 0x7FFFFFFF));
}

// Scan a layer's BGRA alpha into LAYER-LOCAL merged spans (exact-match row
// merge; pathological layouts degrade to the full layer rect past the
// bounded budget rather than stalling SetWindowRgn).
void scan_layer_spans_local(const sao_ui_layer_s* layer, std::vector<RgnRect>* out) {
    out->clear();
    const uint32_t scan_width =
        std::min(layer->bgra_width, static_cast<uint32_t>(layer->width));
    const uint32_t scan_height =
        std::min(layer->bgra_height, static_cast<uint32_t>(layer->height));
    struct SpanRun {
        uint32_t start;
        uint32_t end;
        uint32_t y_begin;
        uint32_t y_end;
    };
    std::vector<SpanRun> active;
    active.reserve(64);
    bool budget_exceeded = false;
    const auto flush_run = [&](const SpanRun& run) {
        if (out->size() >= kMaxMergedRegionRects) {
            budget_exceeded = true;
            return;
        }
        out->push_back(RgnRect{static_cast<int32_t>(run.start),
                               static_cast<int32_t>(run.y_begin),
                               static_cast<int32_t>(run.end - run.start),
                               static_cast<int32_t>(run.y_end - run.y_begin)});
    };
    for (uint32_t y = 0; y < scan_height && !budget_exceeded; ++y) {
        const uint8_t* row =
            layer->bgra_pixels.data() + static_cast<size_t>(y) * layer->bgra_stride;
        std::vector<SpanRun> next;
        size_t run_index = 0;
        uint32_t x = 0;
        while (x < scan_width) {
            while (x < scan_width && row[x * 4u + 3u] == 0) ++x;
            const uint32_t start = x;
            while (x < scan_width && row[x * 4u + 3u] != 0) ++x;
            if (start >= x) continue;
            while (run_index < active.size() &&
                   (active[run_index].start < start ||
                    (active[run_index].start == start && active[run_index].end < x))) {
                flush_run(active[run_index++]);
                if (budget_exceeded) break;
            }
            if (budget_exceeded) break;
            if (run_index < active.size() && active[run_index].start == start &&
                active[run_index].end == x) {
                active[run_index].y_end = y + 1;
                next.push_back(active[run_index]);
                ++run_index;
            } else {
                next.push_back(SpanRun{start, x, y, y + 1});
            }
        }
        while (!budget_exceeded && run_index < active.size()) {
            flush_run(active[run_index++]);
        }
        active = std::move(next);
    }
    for (const auto& run : active) {
        if (budget_exceeded) break;
        flush_run(run);
    }
    if (budget_exceeded) {
        out->clear();
        out->push_back(RgnRect{0, 0, layer->width, layer->height});
    }
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_sync_host_rgn(
    sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (compositor->host == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (std::this_thread::get_id() != compositor->render_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::vector<SaoOverlayHostInputRect> rects;
    try {
        std::lock_guard<std::mutex> lock(compositor->mtx);
        for (const auto& layer : compositor->layers) {
            if (!layer->visible || !layer->input_enabled || layer->click_through ||
                layer->input_proxy_enabled || layer->width <= 0 || layer->height <= 0 ||
                layer->alpha <= 0.0f) continue;

            // Motion pad (Python authority: _RGN_PAD_STILL/_RGN_PAD_MOVE with
            // the velocity-scaled cap).  The DWM applies SetWindowRgn up to a
            // frame out of step with the presented pixels, so a moving layer
            // gets a pad sized to its per-tick translation; a settled layer
            // keeps pixel-exact edges.
            const bool has_prev_geometry =
                layer->rgn_prev_x != std::numeric_limits<int32_t>::min();
            const bool layer_moving =
                has_prev_geometry &&
                (layer->rgn_prev_x != layer->x || layer->rgn_prev_y != layer->y);
            int32_t pad = 0;
            if (layer_moving) {
                const int64_t dx =
                    static_cast<int64_t>(layer->x) - layer->rgn_prev_x;
                const int64_t dy =
                    static_cast<int64_t>(layer->y) - layer->rgn_prev_y;
                const int64_t step =
                    std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
                pad = static_cast<int32_t>(std::min<int64_t>(
                    kRgnPadMoveCap, std::max<int64_t>(kRgnPadMove, step * 2)));
            }
            const auto reset_rgn_motion_state = [&] {
                layer->rgn_union_prev.clear();
                layer->rgn_emit_spans.clear();
                layer->rgn_static_ticks = 0;
                layer->rgn_cache_revision = UINT64_MAX;
            };
            const auto finish_geometry = [&] {
                layer->rgn_prev_x = layer->x;
                layer->rgn_prev_y = layer->y;
            };

            if (!layer->input_rects.empty()) {
                // Logical input rects override alpha scanning; they move
                // rigidly with the layer, so the motion pad alone keeps
                // drag-time hit coverage in step with the presented pixels.
                reset_rgn_motion_state();
                for (const auto& rect : layer->input_rects) {
                    if (!append_host_input_rect(
                            &rects,
                            static_cast<int64_t>(layer->x) + rect.x - pad,
                            static_cast<int64_t>(layer->y) + rect.y - pad,
                            rect.width + 2 * pad, rect.height + 2 * pad)) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                }
                finish_geometry();
                continue;
            }
            if (layer->rect_hit || layer->bgra_pixels.empty() ||
                layer->bgra_width == 0 || layer->bgra_height == 0) {
                reset_rgn_motion_state();
                if (!append_host_input_rect(&rects,
                                            static_cast<int64_t>(layer->x) - pad,
                                            static_cast<int64_t>(layer->y) - pad,
                                            layer->width + 2 * pad,
                                            layer->height + 2 * pad)) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                finish_geometry();
                continue;
            }

            // Content-addressed span cache: keyed only on visual_revision so
            // a pure translation never forces a rescan (Python keys spans on
            // content too; the C++ port caches LAYER-LOCAL spans and
            // translates at emit time instead).
            bool content_changed = false;
            if (!compositor->config.enable_rgn_cache ||
                layer->rgn_cache_revision != layer->visual_revision) {
                scan_layer_spans_local(layer.get(), &layer->rgn_cached_spans);
                layer->rgn_cache_revision = layer->visual_revision;
                content_changed = true;
            }
            std::vector<SaoOverlayHostInputRect> current_host;
            current_host.reserve(layer->rgn_cached_spans.size());
            translate_rgn_rects(layer->rgn_cached_spans, layer->x, layer->y,
                                &current_host);

            std::vector<SaoOverlayHostInputRect> emit;
            const bool union_enabled = compositor->config.enable_temporal_union;
            if (content_changed || layer_moving) {
                // Dirty tick (Python authority): the clip region and the
                // presented pixels travel two unsynchronized pipelines, so
                // an exact-fit region built from THIS frame can be paired on
                // screen with the PREVIOUS frame's pixels.  Emit the
                // predictive pad + the temporal union so both pairings stay
                // covered; the pad scales to the MEASURED silhouette motion
                // (x2 headroom) and never sits at a fixed worst-case width.
                if (union_enabled && !layer->rgn_union_prev.empty()) {
                    const int32_t cstep =
                        rgn_row_extent_step(current_host, layer->rgn_union_prev);
                    pad = std::max(pad, std::min(kRgnPadAnimCap,
                                                 std::max(kRgnPadAnimMin, cstep * 2)));
                } else if (union_enabled) {
                    pad = std::max(pad, kRgnPadMove);
                }
                if (pad > 0) {
                    pad_merge_rgn_rects(current_host, pad, &emit);
                    if (union_enabled && !layer->rgn_union_prev.empty()) {
                        pad_merge_rgn_rects(layer->rgn_union_prev, pad, &emit);
                    }
                } else {
                    emit = current_host;
                    if (union_enabled && !layer->rgn_union_prev.empty()) {
                        emit.insert(emit.end(), layer->rgn_union_prev.begin(),
                                    layer->rgn_union_prev.end());
                    }
                }
                if (union_enabled) {
                    layer->rgn_union_prev = current_host;
                    layer->rgn_emit_spans = emit;
                    layer->rgn_static_ticks = 0;
                }
            } else if (union_enabled &&
                       layer->rgn_static_ticks < kRgnStaticSettleTicks &&
                       !layer->rgn_emit_spans.empty()) {
                // Unchanged this tick, but the last change is still within
                // the pairing-skew window — hold the padded union so a
                // late-applying region can't clip the final frame.  Reusing
                // the emitted list verbatim also keeps the SetWindowRgn key
                // stable instead of oscillating union->exact->union.
                ++layer->rgn_static_ticks;
                emit = layer->rgn_emit_spans;
            } else {
                // Quiescent past the skew window: settle back to the
                // exact-fit spans so idle click-through stays per-pixel
                // precise (no permanent pad halo).
                emit = std::move(current_host);
            }
            if (emit.size() > kMaxMergedRegionRects) {
                emit.assign(1, SaoOverlayHostInputRect{layer->x, layer->y,
                                                       layer->width, layer->height});
                if (union_enabled)
                    layer->rgn_emit_spans = emit;
            }
            for (const auto& rect : emit) {
                if (!append_host_input_rect(&rects, rect.x, rect.y, rect.width,
                                            rect.height)) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
            }
            finish_geometry();
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return sao::ui::input_router_detail::apply_host_input_regions(
        compositor->host, rects.empty() ? nullptr : rects.data(), rects.size());
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_lift_input_proxies(
    sao_ui_compositor_handle_t compositor) {
    return sao_ui_compositor_enforce_z_order(compositor);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_sync_host_input_mode(
    sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (compositor->host == nullptr) return SAO_STATUS_ERR_NOT_INITIALIZED;
    if (std::this_thread::get_id() != compositor->render_thread) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    bool interactive = false;
    try {
        std::lock_guard<std::mutex> lock(compositor->mtx);
        for (const auto& layer : compositor->layers) {
            if (layer->visible && layer->input_enabled &&
                !layer->click_through && !layer->input_proxy_enabled && layer->alpha > 0.0f &&
                (!layer->input_rects.empty() || layer->rect_hit ||
                 layer->bgra_pixels.empty() ||
                 has_nonzero_bgra_alpha(layer.get()))) {
                interactive = true;
                break;
            }
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return sao::ui::input_router_detail::apply_host_input_passthrough(compositor->host,
                                                                      !interactive);
}
