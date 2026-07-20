// Render-spine tests: pure layer ownership plus real D3D/DComp
// resources. All cases link the production sao_platform_ui DLL.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/compositor.h"
#include "sao/ui/d3d11_device.h"
#include "sao/ui/dcomp_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
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
#  include <wrl/client.h>
#endif

#ifndef SAO_UI_TEST_FIXTURE_DIR
#  define SAO_UI_TEST_FIXTURE_DIR ""
#endif

namespace {

struct RenderLog {
    uint32_t render_calls = 0;
    uint32_t fade_calls = 0;
    bool throw_render = false;
    bool throw_fade = false;
};

struct DestroyLog {
    sao_ui_compositor_handle_t compositor = nullptr;
    uint32_t calls = 0;
};

void SAO_UI_CALL render_callback(void*, float, void* user_data) {
    auto* log = static_cast<RenderLog*>(user_data);
    ++log->render_calls;
    if (log->throw_render) throw std::runtime_error("render callback");
}

void SAO_UI_CALL fade_callback(void* user_data) {
    auto* log = static_cast<RenderLog*>(user_data);
    ++log->fade_calls;
    if (log->throw_fade) throw std::runtime_error("fade callback");
}

void SAO_UI_CALL destroy_compositor_callback(void*, float, void* user_data) {
    auto* log = static_cast<DestroyLog*>(user_data);
    ++log->calls;
    sao_ui_compositor_destroy(log->compositor);
}

SaoCompositorConfig compositor_config() {
    SaoCompositorConfig config{};
    config.target_hz = 60;
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    return config;
}

SaoLayerConfig layer_config(const char* name) {
    SaoLayerConfig config{};
    config.name_utf8 = name;
    config.width = 2;
    config.height = 2;
    config.bgra_swizzle = true;
    config.click_through = true;
    return config;
}

std::vector<uint8_t> compositor_snapshot(sao_ui_compositor_handle_t compositor,
                                         uint32_t* width,
                                         uint32_t* height) {
    size_t required_bytes = 0;
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, nullptr, 0, width, height, &required_bytes) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(required_bytes);
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, pixels.data(), pixels.size(), width, height,
                &required_bytes) == SAO_STATUS_OK);
    REQUIRE(pixels.size() == required_bytes);
    return pixels;
}

std::vector<uint8_t> read_fixture(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(SAO_UI_TEST_FIXTURE_DIR) / name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

#if defined(_WIN32)

struct HiddenWindow {
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;
    ATOM atom = 0;
    const wchar_t* class_name = L"SaoRenderSpineTestWindow";

    HiddenWindow() {
        instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = class_name;
        atom = ::RegisterClassExW(&wc);
        hwnd = ::CreateWindowExW(
            0, class_name, L"sao render spine", WS_OVERLAPPEDWINDOW,
            0, 0, 32, 32, nullptr, nullptr, instance, nullptr);
    }

    ~HiddenWindow() {
        if (hwnd != nullptr) ::DestroyWindow(hwnd);
        if (atom != 0) ::UnregisterClassW(class_name, instance);
    }
};

class ScopedFileMapping {
public:
    explicit ScopedFileMapping(size_t byte_count) {
        static std::atomic<uint32_t> next_id{0};
        name = "Local\\SaoUiSopfW19_" +
            std::to_string(::GetCurrentProcessId()) + "_" +
            std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
        const uint64_t size = static_cast<uint64_t>(byte_count);
        mapping = ::CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            static_cast<DWORD>(size >> 32u), static_cast<DWORD>(size),
            name.c_str());
        if (mapping != nullptr) {
            view = static_cast<uint8_t*>(::MapViewOfFile(
                mapping, FILE_MAP_ALL_ACCESS, 0, 0, byte_count));
        }
        if (view != nullptr) std::memset(view, 0, byte_count);
    }

    ~ScopedFileMapping() {
        if (view != nullptr) ::UnmapViewOfFile(view);
        if (mapping != nullptr) ::CloseHandle(mapping);
    }

    ScopedFileMapping(const ScopedFileMapping&) = delete;
    ScopedFileMapping& operator=(const ScopedFileMapping&) = delete;

    std::string name;
    HANDLE mapping = nullptr;
    uint8_t* view = nullptr;
};

struct UiInteropFixtureGuard {
    sao_ui_overlay_host_handle_t host = nullptr;
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_layer_handle_t layer = nullptr;

    ~UiInteropFixtureGuard() {
        if (layer != nullptr) sao_ui_layer_destroy(layer);
        if (compositor != nullptr) sao_ui_compositor_destroy(compositor);
        if (host != nullptr) (void)sao_ui_overlay_host_destroy(host);
    }
};

struct KeyedMutexReleaseGuard {
    IDXGIKeyedMutex* mutex = nullptr;
    bool acquired = false;

    ~KeyedMutexReleaseGuard() {
        if (acquired) {
            (void)mutex->ReleaseSync(SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY);
        }
    }
};

template <typename Header>
void write_mmf_header(ScopedFileMapping& mapping, const Header& header) {
    REQUIRE(mapping.view != nullptr);
    std::memcpy(mapping.view, &header, sizeof(header));
}

void write_mmf_slot(ScopedFileMapping& mapping,
                    uint32_t slot_stride,
                    uint32_t slot,
                    const std::array<uint8_t, 8>& pixels) {
    const size_t offset = SAO_UI_SOPF_MMF_HEADER_BYTES +
        static_cast<size_t>(slot) * slot_stride;
    std::memcpy(mapping.view + offset, pixels.data(), pixels.size());
}

void write_mmf_v2_footer(ScopedFileMapping& mapping,
                         uint32_t slot_stride,
                         uint32_t slot,
                         uint64_t generation) {
    const size_t offset = SAO_UI_SOPF_MMF_HEADER_BYTES +
        static_cast<size_t>(slot + 1u) * slot_stride -
        SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES;
    const SaoUiSopfMmfSlotFooterV2 footer{generation};
    std::memcpy(mapping.view + offset, &footer, sizeof(footer));
}

SaoUiSopfMmfHeaderV1 sopf_v1_header(uint32_t slot_stride,
                                    uint64_t generation,
                                    uint32_t published_slot) {
    SaoUiSopfMmfHeaderV1 header{};
    header.magic = SAO_UI_SOPF_MMF_MAGIC;
    header.version = SAO_UI_SOPF_MMF_VERSION_V1;
    header.frame_width = 2;
    header.frame_height = 1;
    header.slot_count = 3;
    header.slot_stride = slot_stride;
    header.published_generation = generation;
    header.published_slot = published_slot;
    return header;
}

SaoUiSopfMmfHeaderV2 sopf_v2_header(uint32_t slot_stride,
                                    uint64_t generation,
                                    uint32_t latest_completed_slot) {
    SaoUiSopfMmfHeaderV2 header{};
    header.magic = SAO_UI_SOPF_MMF_MAGIC;
    header.version = SAO_UI_SOPF_MMF_VERSION_V2;
    header.frame_width = 2;
    header.frame_height = 1;
    header.slot_count = 3;
    header.slot_stride = slot_stride;
    header.published_generation = generation;
    header.latest_completed_slot = latest_completed_slot;
    header.header_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES;
    return header;
}

void create_headless_mmf_layer(const char* layer_name,
                               UiInteropFixtureGuard* fixture) {
    SaoCompositorConfig config = compositor_config();
    REQUIRE(sao_ui_compositor_create(
                nullptr, &config, &fixture->compositor) == SAO_STATUS_OK);
    SaoLayerConfig definition = layer_config(layer_name);
    definition.width = 2;
    definition.height = 1;
    REQUIRE(sao_ui_layer_create(
                fixture->compositor, &definition, &fixture->layer) ==
            SAO_STATUS_OK);
}

void require_empty_snapshot(sao_ui_compositor_handle_t compositor) {
    uint32_t width = 1;
    uint32_t height = 1;
    size_t bytes = 1;
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, nullptr, 0, &width, &height, &bytes) ==
            SAO_STATUS_OK);
    REQUIRE(width == 0);
    REQUIRE(height == 0);
    REQUIRE(bytes == 0);
}

HRESULT create_test_d3d11_device(
    Microsoft::WRL::ComPtr<ID3D11Device>* device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>* context) {
    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
        device->ReleaseAndGetAddressOf(), &selected,
        context->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels.data(),
            static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
            device->ReleaseAndGetAddressOf(), &selected,
            context->ReleaseAndGetAddressOf());
    }
    return hr;
}

#endif

}  // namespace

TEST_CASE("render_spine_layer_bgra_snapshot_is_headless",
          "[ui][render_spine][ui_parity][pure]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig layer_definition = layer_config("render_spine_headless");
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_definition, &layer) == SAO_STATUS_OK);

    const std::array<uint8_t, 16> premultiplied_bgra = {
        0x20, 0x10, 0x08, 0x40,
        0x00, 0x00, 0x00, 0x00,
        0x20, 0x40, 0x10, 0x80,
        0x10, 0x08, 0x04, 0x20,
    };
    REQUIRE(sao_ui_layer_update_bgra(
        layer, premultiplied_bgra.data(), 2, 2, 8) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_position(layer, 3, 4) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_alpha(layer, 0.5f) == SAO_STATUS_OK);

    // A null-host compositor has no presentation resource by design. The
    // snapshot path is pure and succeeds; the production present boundary
    // reports the absent DComp bridge instead of pretending it rendered.
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);

    std::thread destroyer([layer] { sao_ui_layer_destroy(layer); });
    destroyer.join();

    const SaoUiLayerInputRect input_rect{0, 0, 1, 1};
    CHECK(sao_ui_layer_update_bgra(
              layer, premultiplied_bgra.data(), 2, 2, 8) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_mmf_source(layer, nullptr) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_shared_texture(layer, nullptr, 0, 0) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_render_fn(layer, nullptr, nullptr) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_position(layer, 0, 0) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_geometry(layer, 0, 0, 2, 2) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_z_order(layer, 1) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_visible(layer, true) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_alpha(layer, 1.0f) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_start_fade(layer, 1.0f, 0.0f, nullptr, nullptr) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_input_enabled(layer, true) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_input_rects(layer, &input_rect, 1) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_request_redraw(layer) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_input_callbacks(
              layer, nullptr, nullptr, nullptr, nullptr, nullptr) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_enable_input_proxy(layer) ==
          SAO_STATUS_ERR_HANDLE_INVALID);

    sao_ui_layer_handle_t replacement = nullptr;
    REQUIRE(sao_ui_layer_create(
                compositor, &layer_definition, &replacement) == SAO_STATUS_OK);
    size_t layer_count = 0;
    REQUIRE(sao_ui_compositor_list_layers(
                compositor, nullptr, 0, &layer_count) == SAO_STATUS_OK);
    CHECK(layer_count == 1);

    // The render-thread present flushes the detached layer before doing frame
    // work. The replacement remains active after that flush.
    CHECK(sao_ui_compositor_present(compositor) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);
        CHECK(sao_ui_layer_set_visible(layer, false) ==
            SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_layer_set_visible(replacement, false) == SAO_STATUS_OK);
    sao_ui_layer_destroy(replacement);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("render_spine_compositor_destroy_is_render_thread_affine",
          "[ui][render_spine][ui_parity][lifecycle]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

    SaoLayerConfig definition = layer_config("render_spine_owner_destroy");
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);

    std::thread non_owner(
        [compositor] { sao_ui_compositor_destroy(compositor); });
    non_owner.join();

    CHECK(sao_ui_layer_set_visible(layer, false) == SAO_STATUS_OK);
    size_t layer_count = 0;
    REQUIRE(sao_ui_compositor_list_layers(
                compositor, nullptr, 0, &layer_count) == SAO_STATUS_OK);
    CHECK(layer_count == 1);

    sao_ui_compositor_destroy(compositor);
}

    TEST_CASE("render_spine_callback_destroy_is_ignored_during_present",
          "[ui][render_spine][ui_parity][lifecycle]") {
        SaoCompositorConfig config = compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

        SaoLayerConfig definition = layer_config("render_spine_callback_destroy");
        sao_ui_layer_handle_t layer = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);
        DestroyLog log{compositor, 0};
        REQUIRE(sao_ui_layer_set_render_fn(
            layer, &destroy_compositor_callback, &log) == SAO_STATUS_OK);

        CHECK(sao_ui_compositor_present(compositor) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);
        CHECK(log.calls == 1);
        size_t layer_count = 0;
        REQUIRE(sao_ui_compositor_list_layers(
            compositor, nullptr, 0, &layer_count) == SAO_STATUS_OK);
        CHECK(layer_count == 1);

        sao_ui_compositor_destroy(compositor);
    }

TEST_CASE("render_spine_layer_production_controls_are_headless_safe",
      "[ui][render_spine][ui_parity][pure]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig layer_definition = layer_config("render_spine_controls");
    layer_definition.click_through = false;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_definition, &layer) == SAO_STATUS_OK);

    RenderLog log{};
    REQUIRE(sao_ui_layer_set_mmf_source(layer, "render_spine_optional_mmf") == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_shared_texture(layer, nullptr, 0, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_render_fn(layer, &render_callback, &log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_enabled(layer, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, nullptr, nullptr, nullptr, nullptr, &log) ==
        SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_enable_input_proxy(layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_request_redraw(layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_start_fade(layer, 0.25f, 0.0f, &fade_callback, &log) == SAO_STATUS_OK);

    // Presentation correctly reports that a headless compositor has no DComp
    // target, but it still performs the render-thread callback and fade work.
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(log.render_calls == 1);
    CHECK(log.fade_calls == 1);

    log.throw_render = true;
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(log.render_calls == 2);
    log.throw_render = false;

    log.throw_fade = true;
    REQUIRE(sao_ui_layer_start_fade(
                layer, 0.5f, 0.0f, &fade_callback, &log) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(log.render_calls == 3);
    CHECK(log.fade_calls == 2);
    CHECK(sao_ui_compositor_enforce_z_order(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(sao_ui_compositor_sync_host_rgn(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(sao_ui_compositor_sync_host_input_mode(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);

    sao_status_t cross_thread_z_status = SAO_STATUS_OK;
    sao_status_t cross_thread_shared_status = SAO_STATUS_OK;
    std::thread cross_thread_z([&] {
        cross_thread_z_status = sao_ui_compositor_enforce_z_order(compositor);
        cross_thread_shared_status =
            sao_ui_layer_set_shared_texture(layer, nullptr, 0, 0);
    });
    cross_thread_z.join();
    CHECK(cross_thread_z_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(cross_thread_shared_status == SAO_STATUS_ERR_ACCESS_DENIED);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("render_spine_callbacks_isolate_exceptions_and_continue",
          "[ui][render_spine][ui_parity][callback]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

    std::array<sao_ui_layer_handle_t, 4> layers{};
    std::array<RenderLog, 4> logs{};
    logs[0].throw_render = true;
    logs[2].throw_fade = true;
    for (size_t index = 0; index < layers.size(); ++index) {
        const std::string name =
            "render_spine_callback_" + std::to_string(index);
        SaoLayerConfig definition = layer_config(name.c_str());
        REQUIRE(sao_ui_layer_create(
                    compositor, &definition, &layers[index]) ==
                SAO_STATUS_OK);
    }
    REQUIRE(sao_ui_layer_set_render_fn(
                layers[0], &render_callback, &logs[0]) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_render_fn(
                layers[1], &render_callback, &logs[1]) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_start_fade(
                layers[2], 0.5f, 0.0f, &fade_callback, &logs[2]) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_start_fade(
                layers[3], 0.5f, 0.0f, &fade_callback, &logs[3]) ==
            SAO_STATUS_OK);

    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_UNKNOWN);
    CHECK(logs[0].render_calls == 1);
    CHECK(logs[1].render_calls == 1);
    CHECK(logs[2].fade_calls == 1);
    CHECK(logs[3].fade_calls == 1);

    for (const auto layer : layers) sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

    TEST_CASE("render_spine_second_fade_reports_interrupted_callback",
          "[ui][render_spine][ui_parity][callback]") {
        SaoCompositorConfig config = compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

        SaoLayerConfig definition = layer_config("render_spine_fade_interrupt");
        sao_ui_layer_handle_t layer = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);
        RenderLog first{};
        RenderLog second{};
        REQUIRE(sao_ui_layer_start_fade(
            layer, 0.25f, 60.0f, &fade_callback, &first) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_layer_start_fade(
            layer, 0.75f, 60.0f, &fade_callback, &second) ==
            SAO_STATUS_OK);

        CHECK(sao_ui_compositor_present(compositor) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);
        CHECK(first.fade_calls == 1);
        CHECK(second.fade_calls == 0);

        sao_ui_layer_destroy(layer);
        sao_ui_compositor_destroy(compositor);
    }

TEST_CASE("render_spine_geometry_updates_are_atomic_and_bounded",
          "[ui][render_spine][ui_parity][geometry]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

    SaoLayerConfig definition = layer_config("render_spine_geometry_guard");
    definition.width = 4;
    definition.height = 4;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);

    std::vector<uint8_t> original(4u * 4u * 4u, 0u);
    original[0] = 0x20;
    original[1] = 0x30;
    original[2] = 0x40;
    original[3] = 0x80;
    REQUIRE(sao_ui_layer_update_bgra(
                layer, original.data(), 4, 4, 16) == SAO_STATUS_OK);
    const SaoUiLayerInputRect input_rect{2, 2, 2, 2};
    REQUIRE(sao_ui_layer_set_input_rects(layer, &input_rect, 1) ==
            SAO_STATUS_OK);

    const std::vector<uint8_t> smaller(3u * 3u * 4u, 0x7f);
    CHECK(sao_ui_layer_update_bgra(
              layer, smaller.data(), 3, 3, 12) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_layer_set_geometry(layer, 0, 0, 3, 4) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_layer_set_position(layer, INT32_MAX - 2, 0) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
        CHECK(sao_ui_layer_set_position(layer, 1073741822, 0) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);
        const std::array<uint8_t, 4> tiny_source{};
        CHECK(sao_ui_layer_update_bgra(
                  layer, tiny_source.data(), 16384, 8193, 65536) ==
              SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_layer_set_alpha(
              layer, std::numeric_limits<float>::quiet_NaN()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_layer_set_alpha(
              layer, std::numeric_limits<float>::infinity()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_layer_start_fade(
              layer, 0.5f, std::numeric_limits<float>::quiet_NaN(), nullptr,
              nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(sao_ui_layer_start_fade(
              layer, -std::numeric_limits<float>::infinity(), 0.1f, nullptr,
              nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto pixels = compositor_snapshot(compositor, &width, &height);
    CHECK(width == 4);
    CHECK(height == 4);
    CHECK(pixels == original);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

    TEST_CASE("render_spine_hidden_render_callback_retries_after_exception",
          "[ui][render_spine][ui_parity][callback]") {
        SaoCompositorConfig config = compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

        SaoLayerConfig definition = layer_config("render_spine_hidden_retry");
        sao_ui_layer_handle_t layer = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);
        RenderLog log{};
        log.throw_render = true;
        REQUIRE(sao_ui_layer_set_render_fn(layer, &render_callback, &log) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_layer_set_visible(layer, false) == SAO_STATUS_OK);

        CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_UNKNOWN);
        CHECK(log.render_calls == 1);
        log.throw_render = false;
        CHECK(sao_ui_compositor_present(compositor) ==
          SAO_STATUS_ERR_NOT_INITIALIZED);
        CHECK(log.render_calls == 2);

        sao_ui_layer_destroy(layer);
        sao_ui_compositor_destroy(compositor);
    }

TEST_CASE("render_spine_transparent_snapshot_keeps_geometry",
          "[ui][render_spine][ui_parity][transparent]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

    SaoLayerConfig definition = layer_config("render_spine_transparent_geometry");
    definition.width = 3;
    definition.height = 2;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);
    const std::vector<uint8_t> transparent(3u * 2u * 4u, 0u);
    REQUIRE(sao_ui_layer_update_bgra(
                layer, transparent.data(), 3, 2, 12) == SAO_STATUS_OK);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto pixels = compositor_snapshot(compositor, &width, &height);
    CHECK(width == 3);
    CHECK(height == 2);
    CHECK(pixels == transparent);

    REQUIRE(sao_ui_layer_set_visible(layer, false) == SAO_STATUS_OK);
    size_t bytes = 1;
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, nullptr, 0, &width, &height, &bytes) ==
            SAO_STATUS_OK);
    CHECK(width == 0);
    CHECK(height == 0);
    CHECK(bytes == 0);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

    TEST_CASE("render_spine_shared_source_clear_drops_cached_pixels",
          "[ui][render_spine][ui_parity][source]") {
        SaoCompositorConfig config = compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

        SaoLayerConfig definition = layer_config("render_spine_shared_clear");
        sao_ui_layer_handle_t layer = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);
        const std::array<uint8_t, 4> pixel{0x20, 0x10, 0x08, 0x40};
        REQUIRE(sao_ui_layer_update_bgra(
            layer, pixel.data(), 1, 1, 4) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layer_set_shared_texture(layer, nullptr, 0, 0) ==
            SAO_STATUS_OK);

        uint32_t width = 1;
        uint32_t height = 1;
        size_t bytes = 1;
        REQUIRE(sao_ui_compositor_snapshot_bgra(
            compositor, nullptr, 0, &width, &height, &bytes) ==
            SAO_STATUS_OK);
        CHECK(width == 0);
        CHECK(height == 0);
        CHECK(bytes == 0);

        sao_ui_layer_destroy(layer);
        sao_ui_compositor_destroy(compositor);
    }

TEST_CASE("render_spine_snapshot_uses_production_composition_order",
          "[ui][render_spine][ui_parity][pixel]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig base_definition = layer_config("render_spine_snapshot_base");
    base_definition.z_order = 10;
    sao_ui_layer_handle_t base = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &base_definition, &base) == SAO_STATUS_OK);

    SaoLayerConfig overlay_definition = layer_config("render_spine_snapshot_overlay");
    overlay_definition.z_order = 20;
    sao_ui_layer_handle_t overlay = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &overlay_definition, &overlay) == SAO_STATUS_OK);

    const std::array<uint8_t, 4> base_pixel = {0x20, 0x40, 0x60, 0x80};
    const std::array<uint8_t, 4> overlay_pixel = {0x40, 0x20, 0x10, 0x80};
    REQUIRE(sao_ui_layer_update_bgra(base, base_pixel.data(), 1, 1, 4) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_update_bgra(overlay, overlay_pixel.data(), 1, 1, 4) ==
            SAO_STATUS_OK);

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels = compositor_snapshot(compositor, &width, &height);
    REQUIRE(width == 1);
    REQUIRE(height == 1);
    REQUIRE(pixels == std::vector<uint8_t>{0x50, 0x40, 0x40, 0xC0});

    REQUIRE(sao_ui_layer_set_alpha(overlay, 0.5f) == SAO_STATUS_OK);
    pixels = compositor_snapshot(compositor, &width, &height);
    REQUIRE(pixels == std::vector<uint8_t>{0x38, 0x40, 0x50, 0xA0});

    REQUIRE(sao_ui_layer_set_z_order(base, 30) == SAO_STATUS_OK);
    std::array<sao_ui_layer_handle_t, 2> layers{};
    size_t layer_count = 0;
    REQUIRE(sao_ui_compositor_list_layers(
                compositor, layers.data(), layers.size(), &layer_count) == SAO_STATUS_OK);
    REQUIRE(layer_count == 2);
    REQUIRE(layers[0] == overlay);
    REQUIRE(layers[1] == base);

    pixels = compositor_snapshot(compositor, &width, &height);
    REQUIRE(pixels == std::vector<uint8_t>{0x30, 0x48, 0x64, 0xA0});

    sao_ui_layer_destroy(overlay);
    sao_ui_layer_destroy(base);
    sao_ui_compositor_destroy(compositor);
}

    TEST_CASE("render_spine_mmf_source_clear_drops_cached_pixels",
          "[ui][render_spine][ui_parity][source]") {
        SaoCompositorConfig config = compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) ==
            SAO_STATUS_OK);

        SaoLayerConfig definition = layer_config("render_spine_mmf_clear");
        sao_ui_layer_handle_t layer = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) ==
            SAO_STATUS_OK);
        const std::array<uint8_t, 4> pixel{0x20, 0x10, 0x08, 0x40};
        REQUIRE(sao_ui_layer_update_bgra(
            layer, pixel.data(), 1, 1, 4) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layer_set_mmf_source(layer, nullptr) == SAO_STATUS_OK);

        uint32_t width = 1;
        uint32_t height = 1;
        size_t bytes = 1;
        REQUIRE(sao_ui_compositor_snapshot_bgra(
            compositor, nullptr, 0, &width, &height, &bytes) ==
            SAO_STATUS_OK);
        CHECK(width == 0);
        CHECK(height == 0);
        CHECK(bytes == 0);

        sao_ui_layer_destroy(layer);
        sao_ui_compositor_destroy(compositor);
    }

TEST_CASE("render_spine_python_bgra_transport_matrix_is_lossless",
          "[ui][render_spine][ui_parity][bridge][pixel]") {
    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    // These records intentionally only identify generic transport fixtures:
    // visual ownership remains with the Python producers.  Each frame has
    // distinct premultiplied BGRA bytes so a channel, alpha, row-order, or
    // stale-frame regression cannot pass by comparing an all-zero buffer.
    constexpr std::array<std::array<uint8_t, 8>, 10> fixture_frames = {{
        {{0x08, 0x10, 0x18, 0x20, 0x20, 0x18, 0x10, 0x20}},
        {{0x10, 0x20, 0x30, 0x40, 0x40, 0x30, 0x20, 0x40}},
        {{0x18, 0x30, 0x48, 0x60, 0x60, 0x48, 0x30, 0x60}},
        {{0x20, 0x40, 0x60, 0x80, 0x80, 0x60, 0x40, 0x80}},
        {{0x28, 0x50, 0x78, 0xA0, 0xA0, 0x78, 0x50, 0xA0}},
        {{0x30, 0x60, 0x90, 0xC0, 0xC0, 0x90, 0x60, 0xC0}},
        {{0x38, 0x70, 0xA8, 0xE0, 0xE0, 0xA8, 0x70, 0xE0}},
        {{0x10, 0x08, 0x20, 0x40, 0x40, 0x20, 0x08, 0x40}},
        {{0x18, 0x0C, 0x30, 0x60, 0x60, 0x30, 0x0C, 0x60}},
        {{0x20, 0x10, 0x40, 0x80, 0x80, 0x40, 0x10, 0x80}},
    }};

    for (size_t index = 0; index < fixture_frames.size(); ++index) {
        const std::string name = "render_spine_python_fixture_" + std::to_string(index);
        SaoLayerConfig definition = layer_config(name.c_str());
        definition.width = 2;
        definition.height = 1;
        definition.z_order = static_cast<int32_t>(index);
        sao_ui_layer_handle_t layer = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) == SAO_STATUS_OK);
        REQUIRE(sao_ui_layer_update_bgra(
                    layer, fixture_frames[index].data(), 2, 1, 8) == SAO_STATUS_OK);

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> pixels = compositor_snapshot(compositor, &width, &height);
        REQUIRE(width == 2);
        REQUIRE(height == 1);
        REQUIRE(pixels == std::vector<uint8_t>(
                              fixture_frames[index].begin(), fixture_frames[index].end()));

        REQUIRE(sao_ui_layer_set_visible(layer, false) == SAO_STATUS_OK);
        size_t empty_bytes = 123;
        REQUIRE(sao_ui_compositor_snapshot_bgra(
                    compositor, nullptr, 0, &width, &height, &empty_bytes) == SAO_STATUS_OK);
        REQUIRE(width == 0);
        REQUIRE(height == 0);
        REQUIRE(empty_bytes == 0);
        sao_ui_layer_destroy(layer);
    }

    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("render_spine_authoritative_bgra_fixture_crosses_production_bridge",
          "[ui][render_spine][ui_parity][bridge][fixture][pixel]") {
    const std::vector<uint8_t> source_pixels =
        read_fixture("nervegear_hover.bgra");
    REQUIRE(source_pixels.size() == 72u * 72u * 4u);

    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig definition = layer_config("render_spine_authoritative_frame");
    definition.width = 72;
    definition.height = 72;
    definition.click_through = false;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_update_bgra(
                layer, source_pixels.data(), 72, 72, 72 * 4) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_position(layer, 11, 7) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_enable_input_proxy(layer) == SAO_STATUS_OK);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> pixels = compositor_snapshot(compositor, &width, &height);
    REQUIRE(width == 83);
    REQUIRE(height == 79);
    for (uint32_t row = 0; row < 72; ++row) {
        const size_t source_offset = static_cast<size_t>(row) * 72u * 4u;
        const size_t composed_offset =
            (static_cast<size_t>(row + 7u) * width + 11u) * 4u;
        REQUIRE(std::equal(source_pixels.begin() + source_offset,
                           source_pixels.begin() + source_offset + 72u * 4u,
                           pixels.begin() + composed_offset));
    }

    REQUIRE(sao_ui_layer_set_visible(layer, false) == SAO_STATUS_OK);
    size_t bytes = 1;
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, nullptr, 0, &width, &height, &bytes) == SAO_STATUS_OK);
    REQUIRE(width == 0);
    REQUIRE(height == 0);
    REQUIRE(bytes == 0);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("render_spine_second_authoritative_fixture_crosses_production_bridge",
          "[ui][render_spine][ui_parity][bridge][fixture][pixel]") {
    const std::vector<uint8_t> source_pixels = read_fixture("popup.bgra");
    REQUIRE(source_pixels.size() == 147u * 91u * 4u);

    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig definition = layer_config("render_spine_second_authoritative_frame");
    definition.width = 147;
    definition.height = 91;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_update_bgra(
                layer, source_pixels.data(), 147, 91, 147 * 4) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_position(layer, 4, 9) == SAO_STATUS_OK);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> pixels = compositor_snapshot(compositor, &width, &height);
    REQUIRE(width == 151);
    REQUIRE(height == 100);
    for (uint32_t row = 0; row < 91; ++row) {
        const size_t source_offset = static_cast<size_t>(row) * 147u * 4u;
        const size_t composed_offset =
            (static_cast<size_t>(row + 9u) * width + 4u) * 4u;
        REQUIRE(std::equal(source_pixels.begin() + source_offset,
                           source_pixels.begin() + source_offset + 147u * 4u,
                           pixels.begin() + composed_offset));
    }

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

#if defined(_WIN32)

TEST_CASE("render_spine_sopf_v2_reads_latest_slot_and_skips_unchanged_generation",
          "[ui][render_spine][interop][mmf][v2]") {
    constexpr uint32_t slot_stride = 16;
    constexpr size_t mapping_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES +
        3u * slot_stride;
    ScopedFileMapping mapping(mapping_bytes);
    REQUIRE(mapping.mapping != nullptr);
    REQUIRE(mapping.view != nullptr);

    const std::array<uint8_t, 8> first = {
        0x08, 0x10, 0x18, 0x20, 0x20, 0x18, 0x10, 0x20};
    const std::array<uint8_t, 8> second = {
        0x10, 0x20, 0x30, 0x40, 0x40, 0x30, 0x20, 0x40};
    const std::array<uint8_t, 8> latest = {
        0x18, 0x30, 0x48, 0x60, 0x60, 0x48, 0x30, 0x60};
    auto header = sopf_v2_header(slot_stride, 2, 2);
    write_mmf_header(mapping, header);
    write_mmf_slot(mapping, slot_stride, 0, first);
    write_mmf_slot(mapping, slot_stride, 1, second);
    write_mmf_slot(mapping, slot_stride, 2, latest);
    write_mmf_v2_footer(mapping, slot_stride, 2, 2);

    UiInteropFixtureGuard fixture;
    create_headless_mmf_layer("w19_sopf_v2_latest", &fixture);
    REQUIRE(sao_ui_layer_set_mmf_source(
                fixture.layer, mapping.name.c_str()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);

    uint32_t width = 0;
    uint32_t height = 0;
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(latest.begin(), latest.end()));

    const std::array<uint8_t, 8> unpublished = {
        0x20, 0x40, 0x60, 0x80, 0x80, 0x60, 0x40, 0x80};
    write_mmf_slot(mapping, slot_stride, 2, unpublished);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(latest.begin(), latest.end()));

    header.published_generation = 4;
    header.latest_completed_slot = 1;
    write_mmf_slot(mapping, slot_stride, 1, second);
    write_mmf_v2_footer(mapping, slot_stride, 1, 4);
    write_mmf_header(mapping, header);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(second.begin(), second.end()));

    ScopedFileMapping replacement_mapping(mapping_bytes);
    REQUIRE(replacement_mapping.mapping != nullptr);
    REQUIRE(replacement_mapping.view != nullptr);
    const std::array<uint8_t, 8> replacement = {
        0x28, 0x50, 0x78, 0xA0, 0xA0, 0x78, 0x50, 0xA0};
    const auto replacement_header = sopf_v2_header(slot_stride, 4, 0);
    write_mmf_header(replacement_mapping, replacement_header);
    write_mmf_slot(replacement_mapping, slot_stride, 0, replacement);
    write_mmf_v2_footer(replacement_mapping, slot_stride, 0, 4);
    REQUIRE(sao_ui_layer_set_mmf_source(
                fixture.layer, replacement_mapping.name.c_str()) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(replacement.begin(), replacement.end()));

    REQUIRE(sao_ui_layer_set_mmf_source(fixture.layer, nullptr) ==
            SAO_STATUS_OK);
    require_empty_snapshot(fixture.compositor);
}

TEST_CASE("render_spine_sopf_v2_transient_markers_keep_last_good_frame",
          "[ui][render_spine][interop][mmf][v2]") {
    constexpr uint32_t slot_stride = 16;
    constexpr size_t mapping_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES +
        3u * slot_stride;
    ScopedFileMapping mapping(mapping_bytes);
    REQUIRE(mapping.mapping != nullptr);
    REQUIRE(mapping.view != nullptr);

    const std::array<uint8_t, 8> good = {
        0x08, 0x10, 0x18, 0x20, 0x20, 0x18, 0x10, 0x20};
    const std::array<uint8_t, 8> partial = {
        0x20, 0x40, 0x60, 0x80, 0x80, 0x60, 0x40, 0x80};
    auto header = sopf_v2_header(slot_stride, 2, 0);
    write_mmf_header(mapping, header);
    write_mmf_slot(mapping, slot_stride, 0, good);
    write_mmf_v2_footer(mapping, slot_stride, 0, 2);

    UiInteropFixtureGuard fixture;
    create_headless_mmf_layer("w19_sopf_v2_partial", &fixture);
    REQUIRE(sao_ui_layer_set_mmf_source(
                fixture.layer, mapping.name.c_str()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);

    uint32_t width = 0;
    uint32_t height = 0;
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(good.begin(), good.end()));

    header.published_generation = 3;
    header.latest_completed_slot = 1;
    write_mmf_slot(mapping, slot_stride, 1, partial);
    write_mmf_v2_footer(mapping, slot_stride, 1, 3);
    write_mmf_header(mapping, header);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(good.begin(), good.end()));

    header.published_generation = 4;
    write_mmf_header(mapping, header);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(good.begin(), good.end()));

    header.published_generation = 0;
    write_mmf_v2_footer(mapping, slot_stride, 1, 0);
    write_mmf_header(mapping, header);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(good.begin(), good.end()));
}

TEST_CASE("render_spine_sopf_structural_errors_fail_closed",
          "[ui][render_spine][interop][mmf][validation]") {
    constexpr uint32_t slot_stride = 16;
    constexpr size_t mapping_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES +
        3u * slot_stride;
    ScopedFileMapping mapping(mapping_bytes);
    REQUIRE(mapping.mapping != nullptr);
    REQUIRE(mapping.view != nullptr);

    const std::array<uint8_t, 8> good = {
        0x08, 0x10, 0x18, 0x20, 0x20, 0x18, 0x10, 0x20};
    auto header = sopf_v2_header(slot_stride, 2, 0);
    write_mmf_header(mapping, header);
    write_mmf_slot(mapping, slot_stride, 0, good);
    write_mmf_v2_footer(mapping, slot_stride, 0, 2);

    UiInteropFixtureGuard fixture;
    create_headless_mmf_layer("w19_sopf_bad_structure", &fixture);
    REQUIRE(sao_ui_layer_set_mmf_source(
                fixture.layer, mapping.name.c_str()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);

    SECTION("bad header bytes") {
        header.header_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES - 1u;
    }
    SECTION("bad magic") {
        header.magic ^= 1u;
    }
    SECTION("bad version") {
        header.version = SAO_UI_SOPF_MMF_VERSION_V2 + 1u;
    }
    SECTION("bad slot stride") {
        header.slot_stride = 15;
    }
    SECTION("bad latest slot") {
        header.latest_completed_slot = header.slot_count;
    }
    SECTION("mapping exceeds budget") {
        header.slot_stride = 256u * 1024u * 1024u;
    }
    write_mmf_header(mapping, header);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);
    require_empty_snapshot(fixture.compositor);
}

TEST_CASE("render_spine_sopf_v1_keeps_python_legacy_read_slot",
          "[ui][render_spine][interop][mmf][v1]") {
    constexpr uint32_t slot_stride = 8;
    constexpr size_t mapping_bytes = SAO_UI_SOPF_MMF_HEADER_BYTES +
        3u * slot_stride;
    ScopedFileMapping mapping(mapping_bytes);
    REQUIRE(mapping.mapping != nullptr);
    REQUIRE(mapping.view != nullptr);

    const std::array<uint8_t, 8> slot_zero = {
        0x08, 0x10, 0x18, 0x20, 0x20, 0x18, 0x10, 0x20};
    const std::array<uint8_t, 8> slot_one = {
        0x10, 0x20, 0x30, 0x40, 0x40, 0x30, 0x20, 0x40};
    const std::array<uint8_t, 8> legacy_read_slot = {
        0x18, 0x30, 0x48, 0x60, 0x60, 0x48, 0x30, 0x60};
    const auto header = sopf_v1_header(slot_stride, 1, 0);
    write_mmf_header(mapping, header);
    write_mmf_slot(mapping, slot_stride, 0, slot_zero);
    write_mmf_slot(mapping, slot_stride, 1, slot_one);
    write_mmf_slot(mapping, slot_stride, 2, legacy_read_slot);

    UiInteropFixtureGuard fixture;
    create_headless_mmf_layer("w19_sopf_v1_legacy", &fixture);
    REQUIRE(sao_ui_layer_set_mmf_source(
                fixture.layer, mapping.name.c_str()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) ==
            SAO_STATUS_ERR_NOT_INITIALIZED);

    uint32_t width = 0;
    uint32_t height = 0;
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            std::vector<uint8_t>(
                legacy_read_slot.begin(), legacy_read_slot.end()));
}

TEST_CASE("render_spine_keyed_shared_texture_readback_and_contention_or_skip",
          "[ui][render_spine][interop][keyed_mutex][resource]") {
    Microsoft::WRL::ComPtr<ID3D11Device> producer_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> producer_context;
    if (FAILED(create_test_d3d11_device(
            &producer_device, &producer_context))) {
        SKIP("D3D11 shared resources are unavailable in this environment");
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = 2;
    texture_desc.Height = 1;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(producer_device->CreateTexture2D(
            &texture_desc, nullptr, texture.GetAddressOf()))) {
        SKIP("a real keyed shared texture is unavailable");
    }

    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> producer_mutex;
    Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
    HANDLE shared_handle = nullptr;
    if (FAILED(texture.As(&producer_mutex)) ||
        FAILED(texture.As(&dxgi_resource)) ||
        FAILED(dxgi_resource->GetSharedHandle(&shared_handle)) ||
        shared_handle == nullptr) {
        SKIP("the D3D11 device does not expose keyed legacy sharing");
    }

    UiInteropFixtureGuard fixture;
    SaoOverlayHostConfig host_config{};
    host_config.width = 2;
    host_config.height = 1;
    if (sao_ui_overlay_host_create(&host_config, &fixture.host) !=
        SAO_STATUS_OK) {
        SKIP("an overlay window station is unavailable");
    }
    SaoCompositorConfig config = compositor_config();
    if (sao_ui_compositor_create(
            fixture.host, &config, &fixture.compositor) != SAO_STATUS_OK) {
        SKIP("the D3D11 DirectComposition consumer is unavailable");
    }
    SaoLayerConfig definition = layer_config("w19_keyed_shared");
    definition.width = 2;
    definition.height = 1;
    REQUIRE(sao_ui_layer_create(
                fixture.compositor, &definition, &fixture.layer) ==
            SAO_STATUS_OK);

    const std::array<uint8_t, 8> first_rgba = {
        0x40, 0x20, 0x10, 0x80, 0x20, 0x40, 0x60, 0x80};
    REQUIRE(producer_mutex->AcquireSync(
                SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY, 0) == S_OK);
    producer_context->UpdateSubresource(
        texture.Get(), 0, nullptr, first_rgba.data(), 8, 0);
    REQUIRE(producer_mutex->ReleaseSync(
                SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY) == S_OK);
    producer_context->Flush();

    REQUIRE(sao_ui_layer_set_shared_texture(
                fixture.layer, shared_handle, 2, 1) == SAO_STATUS_OK);
    const sao_status_t first_present =
        sao_ui_compositor_present(fixture.compositor);
    if (first_present != SAO_STATUS_OK) {
        SKIP("the real cross-device shared texture path is unavailable");
    }
    uint32_t width = 0;
    uint32_t height = 0;
    size_t bytes = 0;
    const sao_status_t query_status = sao_ui_compositor_snapshot_bgra(
        fixture.compositor, nullptr, 0, &width, &height, &bytes);
    if (query_status != SAO_STATUS_ERR_BUFFER_TOO_SMALL || bytes == 0) {
        SKIP("the real shared texture could not be opened by the consumer");
    }
    const std::vector<uint8_t> first_expected = {
        0x08, 0x10, 0x20, 0x80, 0x30, 0x20, 0x10, 0x80};
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            first_expected);

    REQUIRE(producer_mutex->AcquireSync(
                SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY, 0) == S_OK);
    KeyedMutexReleaseGuard held{producer_mutex.Get(), true};
    const std::array<uint8_t, 8> second_rgba = {
        0x80, 0x20, 0x10, 0x80, 0x10, 0x20, 0x40, 0x80};
    producer_context->UpdateSubresource(
        texture.Get(), 0, nullptr, second_rgba.data(), 8, 0);
    producer_context->Flush();

    const auto started = std::chrono::steady_clock::now();
    REQUIRE(sao_ui_compositor_present(fixture.compositor) == SAO_STATUS_OK);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(elapsed < std::chrono::milliseconds(250));
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            first_expected);

    const HRESULT release_status = producer_mutex->ReleaseSync(
        SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY);
    held.acquired = false;
    REQUIRE(release_status == S_OK);
    producer_context->Flush();
    REQUIRE(sao_ui_compositor_present(fixture.compositor) == SAO_STATUS_OK);
    const std::vector<uint8_t> second_expected = {
        0x08, 0x10, 0x40, 0x80, 0x20, 0x10, 0x08, 0x80};
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            second_expected);

    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> plain_texture;
    REQUIRE(SUCCEEDED(producer_device->CreateTexture2D(
        &texture_desc, nullptr, plain_texture.GetAddressOf())));
    Microsoft::WRL::ComPtr<IDXGIResource> plain_resource;
    HANDLE plain_handle = nullptr;
    REQUIRE(SUCCEEDED(plain_texture.As(&plain_resource)));
    REQUIRE(SUCCEEDED(plain_resource->GetSharedHandle(&plain_handle)));
    REQUIRE(plain_handle != nullptr);
    const std::array<uint8_t, 8> plain_rgba = {
        0x20, 0x60, 0x40, 0x80, 0x60, 0x40, 0x20, 0x80};
    producer_context->UpdateSubresource(
        plain_texture.Get(), 0, nullptr, plain_rgba.data(), 8, 0);
    producer_context->Flush();
    REQUIRE(sao_ui_layer_set_shared_texture(
                fixture.layer, plain_handle, 2, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_present(fixture.compositor) == SAO_STATUS_OK);
    const std::vector<uint8_t> plain_expected = {
        0x20, 0x30, 0x10, 0x80, 0x10, 0x20, 0x30, 0x80};
    REQUIRE(compositor_snapshot(fixture.compositor, &width, &height) ==
            plain_expected);
}

TEST_CASE("render_spine_shared_device_resources_or_skip",
          "[ui][render_spine][ui_parity][resource]") {
    SaoD3d11DeviceConfig config{};
    config.prefer_warp = true;
    sao_ui_d3d11_device_handle_t device = nullptr;
    const sao_status_t create_status = sao_ui_d3d11_device_create(&config, &device);
    if (create_status != SAO_STATUS_OK) {
        SKIP("D3D11 WARP device is unavailable in this environment");
    }

    REQUIRE(device != nullptr);
    CHECK(sao_ui_d3d11_device_ptr(device) != nullptr);
    CHECK(sao_ui_d3d11_device_context_ptr(device) != nullptr);
    CHECK(sao_ui_d3d11_device_dxgi_factory(device) != nullptr);
    CHECK(sao_ui_d3d11_device_dxgi_adapter(device) != nullptr);
    CHECK(sao_ui_d3d11_device_feature_level(device) != 0u);
    CHECK(sao_ui_d3d11_device_check_alive(device) == SAO_STATUS_OK);
    uint32_t removed_reason = 0xFFFFFFFFu;
    REQUIRE(sao_ui_d3d11_device_recreate(device, &removed_reason) == SAO_STATUS_OK);
    CHECK(removed_reason == 0u);
    CHECK(sao_ui_d3d11_device_ptr(device) != nullptr);
    CHECK(sao_ui_d3d11_device_context_ptr(device) != nullptr);
    sao_ui_d3d11_device_destroy(device);
}

TEST_CASE("render_spine_dcomp_upload_resize_present_or_skip",
          "[ui][render_spine][ui_parity][resource]") {
    HiddenWindow window;
    if (window.hwnd == nullptr) {
        SKIP("a window station is unavailable in this environment");
    }

    SaoD3d11DeviceConfig device_config{};
    device_config.prefer_warp = true;
    sao_ui_d3d11_device_handle_t device = nullptr;
    if (sao_ui_d3d11_device_create(&device_config, &device) != SAO_STATUS_OK) {
        SKIP("D3D11 WARP device is unavailable in this environment");
    }

    SaoDcompBridgeConfig bridge_config{};
    bridge_config.hwnd = window.hwnd;
    bridge_config.d3d11_device = sao_ui_d3d11_device_ptr(device);
    bridge_config.alpha_mode = 1;
    bridge_config.buffer_count = 2;
    bridge_config.width = 2;
    bridge_config.height = 2;
    sao_ui_dcomp_bridge_handle_t bridge = nullptr;
    const sao_status_t bridge_status = sao_ui_dcomp_bridge_create(
        nullptr, &bridge_config, &bridge);
    if (bridge_status != SAO_STATUS_OK) {
        sao_ui_d3d11_device_destroy(device);
        SKIP("DirectComposition is unavailable in this environment");
    }

    const std::array<uint8_t, 16> first_frame = {
        0x40, 0x20, 0x10, 0x80,
        0x00, 0x00, 0x00, 0x00,
        0x10, 0x20, 0x40, 0x80,
        0x08, 0x04, 0x02, 0x10,
    };
    REQUIRE(sao_ui_dcomp_bridge_swap_chain(bridge) != nullptr);
    REQUIRE(sao_ui_dcomp_bridge_upload_bgra(
        bridge, first_frame.data(), 2, 2, 8) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dcomp_bridge_present(bridge) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dcomp_bridge_resize(bridge, 3, 1) == SAO_STATUS_OK);

    const std::array<uint8_t, 12> resized_frame = {
        0x10, 0x10, 0x10, 0x20,
        0x20, 0x20, 0x20, 0x40,
        0x40, 0x40, 0x40, 0x80,
    };
    REQUIRE(sao_ui_dcomp_bridge_upload_bgra(
        bridge, resized_frame.data(), 3, 1, 12) == SAO_STATUS_OK);
    CHECK(sao_ui_dcomp_bridge_present(bridge) == SAO_STATUS_OK);

    sao_ui_dcomp_bridge_destroy(bridge);
    sao_ui_d3d11_device_destroy(device);
}

#else

TEST_CASE("render_spine_windows_resources_skipped", "[ui][render_spine][ui_parity][resource]") {
    SKIP("D3D11, DXGI, and DirectComposition require Windows");
}

#endif
