// SAO Auto - compositor, Wave 2 first-implementable slice.
//
// This is the *first* implementable slice on top of the Wave 1a deepened
// header (`include/sao/ui/compositor.h`).  It implements ONLY the layer
// bookkeeping in memory -- no bitmap uploads, no SetWindowRgn work, no
// GL/D3D interop.  Concretely:
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
// The other 12 header-declared entry points remain SAO_STATUS_ERR_NOT_
// IMPLEMENTED.  Wave 3 owns rendering / RGN sync / input proxy work.
//
// UTF-8 no BOM.  static_asserts guard the invariants the header banner
// documents.

#include "sao/ui/compositor.h"
#include "sao/ui/d3d11_device.h"
#include "sao/ui/dcomp_bridge.h"
#include "sao/ui/z_order.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#endif

// ---------------------------------------------------------------------------
// Compile-time invariants.
// ---------------------------------------------------------------------------

// SaoLayerConfig layout stability -- reject accidental reorder.
static_assert(offsetof(SaoLayerConfig, name_utf8) == 0,
              "SaoLayerConfig.name_utf8 must be first field");
static_assert(offsetof(SaoLayerConfig, x) == sizeof(void*),
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
    std::string mmf_name;
    uint64_t    mmf_last_generation{0};
    bool        mmf_has_last_generation{false};
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
    bool        input_proxy_enabled{false};

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
    mutable std::mutex           mtx;
    uint64_t                     seq{0};
    sao_ui_d3d11_device_handle_t d3d11_device{nullptr};
    sao_ui_dcomp_bridge_handle_t dcomp_bridge{nullptr};
    sao_ui_z_order_manager_handle_t z_order{nullptr};
    std::thread::id               render_thread{};
    bool                          presented_visible_content{false};
    uint32_t                      last_present_width{0};
    uint32_t                      last_present_height{0};
    std::atomic_bool              present_in_progress{false};
};

namespace {

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
#endif
    std::vector<SaoUiLayerInputRect>{}.swap(layer->input_rects);
    clear_bgra_cache(layer);
    std::string{}.swap(layer->name);
    std::string{}.swap(layer->mmf_name);
    layer->render_fn = nullptr;
    layer->render_user_data = nullptr;
    layer->fade_done_fn = nullptr;
    layer->fade_done_user_data = nullptr;
    layer->cursor_pos_fn = nullptr;
    layer->cursor_leave_fn = nullptr;
    layer->button_fn = nullptr;
    layer->scroll_fn = nullptr;
    layer->input_user_data = nullptr;
}

void flush_pending_layer_destroys_locked(sao_ui_compositor_s* comp) {
    for (size_t index = comp->released_pending_count;
         index < comp->pending_layer_destroys.size(); ++index) {
        release_detached_layer_payload(
            comp->pending_layer_destroys[index].get());
    }
    comp->released_pending_count = comp->pending_layer_destroys.size();
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

void fail_closed_mmf_source(sao_ui_layer_s* layer) {
    reset_mmf_generation(layer);
    if (clear_bgra_cache(layer)) mark_layer_dirty(layer);
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
        fail_closed_mmf_source(layer);
        return;
    }
    const WinHandleGuard mapping_guard{mapping};
    const void* header_view = ::MapViewOfFile(
        mapping, FILE_MAP_READ, 0, 0, SAO_UI_SOPF_MMF_HEADER_BYTES);
    if (header_view == nullptr) {
        fail_closed_mmf_source(layer);
        return;
    }
    MmfHeaderValues initial_header{};
    {
        const MappedViewGuard header_guard{header_view};
        if (!decode_mmf_header(header_view, &initial_header)) {
            fail_closed_mmf_source(layer);
            return;
        }
    }
    size_t frame_bytes = 0;
    size_t mapping_bytes = 0;
    if (!validate_mmf_header_locked(
            layer, initial_header, &frame_bytes, &mapping_bytes)) {
        fail_closed_mmf_source(layer);
        return;
    }
    const void* ring_view = ::MapViewOfFile(
        mapping, FILE_MAP_READ, 0, 0, mapping_bytes);
    if (ring_view == nullptr) {
        fail_closed_mmf_source(layer);
        return;
    }
    const MappedViewGuard ring_guard{ring_view};
    const auto* ring = static_cast<const uint8_t*>(ring_view);
    MmfHeaderValues before{};
    if (!decode_mmf_header(ring, &before)) {
        fail_closed_mmf_source(layer);
        return;
    }
    size_t checked_frame_bytes = 0;
    size_t checked_mapping_bytes = 0;
    if (!validate_mmf_header_locked(
            layer, before, &checked_frame_bytes, &checked_mapping_bytes)) {
        fail_closed_mmf_source(layer);
        return;
    }
    if (!same_mmf_structure(initial_header, before) ||
        frame_bytes != checked_frame_bytes ||
        mapping_bytes != checked_mapping_bytes) {
        fail_closed_mmf_source(layer);
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
        fail_closed_mmf_source(layer);
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

bool compose_premultiplied_bgra_locked(
    const sao_ui_compositor_s* comp,
    std::vector<uint8_t>* out_pixels,
    uint32_t* out_width,
    uint32_t* out_height,
    bool* out_has_visible_alpha) {
    int64_t right = 0;
    int64_t bottom = 0;
    *out_has_visible_alpha = false;
    for (const auto& layer : comp->layers) {
        if (!layer->visible || layer->bgra_pixels.empty() ||
            layer->bgra_width == 0 || layer->bgra_height == 0) {
            continue;
        }
        right = std::max(
            right, static_cast<int64_t>(layer->x) + layer->bgra_width);
        bottom = std::max(
            bottom, static_cast<int64_t>(layer->y) + layer->bgra_height);
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

    for (const auto& layer : comp->layers) {
        if (!layer->visible || layer->bgra_pixels.empty() ||
            layer->bgra_width == 0 || layer->bgra_height == 0 ||
            layer->alpha <= 0.0f) {
            continue;
        }
        const uint8_t layer_alpha = static_cast<uint8_t>(
            std::clamp(layer->alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
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
    // Host can be null in headless/tests; the compositor holds a
    // reference but this slice never dereferences it.  The RGN sync /
    // present paths (Wave 3) will require host non-null.

    auto* comp = new (std::nothrow) sao_ui_compositor_s{};
    if (comp == nullptr) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    comp->host = host;
    if (config != nullptr) {
        comp->config = *config;
    } else {
        // Defaults matching the header banner (target_hz = 0 means
        // "auto-detect", the RGN caches on).
        comp->config.target_hz              = 0;
        comp->config.enable_temporal_union  = true;
        comp->config.enable_rgn_cache       = true;
    }

    comp->render_thread = std::this_thread::get_id();
    if (host != nullptr) {
        void* const hwnd = sao_ui_overlay_host_hwnd(host);
        if (hwnd == nullptr) {
            delete comp;
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }

        SaoD3d11DeviceConfig d3d_config{};
        sao_status_t status = sao_ui_d3d11_device_create(
            &d3d_config, &comp->d3d11_device);
        if (status != SAO_STATUS_OK) {
            delete comp;
            return status;
        }

        SaoDcompBridgeConfig bridge_config{};
        bridge_config.hwnd = hwnd;
        bridge_config.d3d11_device = sao_ui_d3d11_device_ptr(comp->d3d11_device);
        bridge_config.alpha_mode = 1;
        bridge_config.buffer_count = 2;
        bridge_config.width = 1;
        bridge_config.height = 1;
        status = sao_ui_dcomp_bridge_create(
            host, &bridge_config, &comp->dcomp_bridge);
        if (status != SAO_STATUS_OK) {
            sao_ui_d3d11_device_destroy(comp->d3d11_device);
            delete comp;
            return status;
        }
        status = sao_ui_z_order_manager_create(host, &comp->z_order);
        if (status != SAO_STATUS_OK) {
            sao_ui_dcomp_bridge_destroy(comp->dcomp_bridge);
            sao_ui_d3d11_device_destroy(comp->d3d11_device);
            delete comp;
            return status;
        }
    }

    *out_handle = comp;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_compositor_destroy(
    sao_ui_compositor_handle_t handle) {
    if (handle == nullptr) return;
    if (std::this_thread::get_id() != handle->render_thread) return;
    if (handle->present_in_progress.load(std::memory_order_acquire)) return;
    try {
        std::lock_guard<std::mutex> lock(handle->mtx);
        sao_ui_z_order_manager_destroy(handle->z_order);
        flush_pending_layer_destroys_locked(handle);
#if defined(_WIN32)
        for (const auto& layer : handle->layers) {
            release_shared_texture_objects(layer.get());
        }
#endif
        handle->layers.clear();
        handle->pending_layer_destroys.clear();
        handle->released_pending_count = 0;
        sao_ui_dcomp_bridge_destroy(handle->dcomp_bridge);
        sao_ui_d3d11_device_destroy(handle->d3d11_device);
    } catch (...) {
        return;
    }
    delete handle;
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

        std::lock_guard<std::mutex> lk(comp->mtx);
        auto it = find_layer_it(comp, layer);
        if (it == comp->layers.end()) return;
        comp->pending_layer_destroys.push_back(std::move(*it));
        comp->layers.erase(it);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Layer state mutators used by the tests.  The other setters remain
// NOT_IMPLEMENTED so their Wave 3 owners can pick them up.
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

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_visible(
    sao_ui_layer_handle_t layer, bool visible) {
    return with_active_layer_locked(
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
// Everything below is deferred to Wave 3.  Keep the stubs so the DLL
// exports match the header contract.
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

    return with_active_layer_locked(
        comp, layer,
        [width, height, stride, &snapshot](sao_ui_compositor_s*,
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

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_set_position(
    sao_ui_layer_handle_t layer, int32_t x, int32_t y) {
    return with_active_layer_locked(
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
    return with_active_layer_locked(
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
    return with_active_layer_locked(
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
    return with_active_layer_locked(
        layer, [enabled](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->input_enabled = enabled;
            return SAO_STATUS_OK;
        });
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_layer_set_input_policy(
    sao_ui_layer_handle_t layer, bool click_through,
    bool input_enabled) {
    return with_active_layer_locked(
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

    return with_active_layer_locked(
        comp, layer,
        [&snapshot](sao_ui_compositor_s*, sao_ui_layer_s* active) {
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
    return with_active_layer_locked(
        layer, [cursor_pos_fn, cursor_leave_fn, button_fn, scroll_fn,
                user_data](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->cursor_pos_fn = cursor_pos_fn;
            active->cursor_leave_fn = cursor_leave_fn;
            active->button_fn = button_fn;
            active->scroll_fn = scroll_fn;
            active->input_user_data = user_data;
            return SAO_STATUS_OK;
        });
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_enable_input_proxy(
    sao_ui_layer_handle_t layer) {
    return with_active_layer_locked(
        layer, [](sao_ui_compositor_s*, sao_ui_layer_s* active) {
            active->input_proxy_enabled = true;
            active->input_enabled = true;
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
    if (compositor->dcomp_bridge == nullptr ||
        compositor->d3d11_device == nullptr) {
        return callback_failed ? SAO_STATUS_ERR_UNKNOWN
                               : SAO_STATUS_ERR_NOT_INITIALIZED;
    }

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
    {
        std::lock_guard<std::mutex> lk(compositor->mtx);
        has_geometry_buffer = compose_premultiplied_bgra_locked(
            compositor, &composed_pixels, &width, &height,
            &has_visible_alpha);
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
        }
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
        return compositor_present_impl(compositor);
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_compositor_snapshot_bgra(
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
        std::vector<uint8_t> composed_pixels;
        bool has_visible_alpha = false;
        {
            std::lock_guard<std::mutex> lock(compositor->mtx);
            if (!compose_premultiplied_bgra_locked(
                    compositor, &composed_pixels, out_width, out_height,
                    &has_visible_alpha)) {
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
    return sao_ui_z_order_enforce(compositor->z_order, nullptr, false, false);
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_compositor_require_owner_thread(sao_ui_compositor_handle_t compositor) {
    if (compositor == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    return std::this_thread::get_id() == compositor->render_thread
               ? SAO_STATUS_OK
               : SAO_STATUS_ERR_ACCESS_DENIED;
}

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
                layer->width <= 0 || layer->height <= 0 ||
                layer->alpha <= 0.0f) continue;
            if (!layer->input_rects.empty()) {
                for (const auto& rect : layer->input_rects) {
                    if (!append_host_input_rect(
                            &rects, static_cast<int64_t>(layer->x) + rect.x,
                            static_cast<int64_t>(layer->y) + rect.y,
                            rect.width, rect.height)) {
                        return SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                }
                continue;
            }
            if (layer->rect_hit || layer->bgra_pixels.empty() ||
                layer->bgra_width == 0 || layer->bgra_height == 0) {
                if (!append_host_input_rect(&rects, layer->x, layer->y,
                                            layer->width, layer->height)) {
                    return SAO_STATUS_ERR_INVALID_ARGUMENT;
                }
                continue;
            }
            const uint32_t scan_width = std::min(
                layer->bgra_width, static_cast<uint32_t>(layer->width));
            const uint32_t scan_height = std::min(
                layer->bgra_height, static_cast<uint32_t>(layer->height));
            for (uint32_t y = 0; y < scan_height; ++y) {
                const uint8_t* row = layer->bgra_pixels.data() +
                    static_cast<size_t>(y) * layer->bgra_stride;
                uint32_t x = 0;
                while (x < scan_width) {
                    while (x < scan_width && row[x * 4u + 3u] == 0) ++x;
                    const uint32_t start = x;
                    while (x < scan_width && row[x * 4u + 3u] != 0) ++x;
                    if (start < x) {
                        if (!append_host_input_rect(
                                &rects,
                                static_cast<int64_t>(layer->x) + start,
                                static_cast<int64_t>(layer->y) + y,
                                x - start, 1)) {
                            return SAO_STATUS_ERR_INVALID_ARGUMENT;
                        }
                    }
                }
            }
        }
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    return sao_ui_overlay_host_set_input_region(
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
                !layer->click_through && layer->alpha > 0.0f &&
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
    return sao_ui_overlay_host_set_input_passthrough(compositor->host, !interactive);
}
