#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")

#include "hotkey_config_panel.h"
#include "menu_panel_toggle.h"
#include "hotkey_manager.h"
#include "memory_viewer_panel_internal.h"
#include "plugin_manager_panel_internal.h"
#include "process_selector_panel_internal.h"
#include "settings_config_panel.h"
#include "settings_owner_internal.h"
#include "settings_profiles.h"
#include "workshop_panel_internal.h"
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
#include "license_panel_internal.h"
#endif
#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_main_panel.h"
#include "sao/ai_editor/ai_editor_settings_panel.h"
#include "sao/launcher/user_menu.h"
#include "sao/launcher/user_guide_webview.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_ui.h"
#include "sao/sdk/sao_sdk_provider.h"
#include "sao/server/freetier/workshop_client/workshop_client.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/compositor.h"
#include "sao/ui/file_picker.h"
#include "sao/ui/fisheye_backdrop.h"
#include "sao/ui/input_router.h"
#include "sao/ui/linkstart_intro.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/plugin_tabs.h"
#include "sao/ui/panel.h"
#include "sao/ui/sao_ui_scriptable_canvas.h"
#include "sao/ui/sound.h"
#include "sao/ui/theme.h"
#include "sao/ui/widget_text.h"
#include "sao/ui/widget_kit.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::launcher::settings {
// Production entry point with a status result; the public wrapper is intentionally void.
sao_status_t open_config_panel_status() noexcept;
} // namespace sao::launcher::settings

namespace {
constexpr UINT_PTR kUiService = 1;
constexpr int32_t kOpenLauncherSettings = 1000;
constexpr int32_t kOpenHotkeys = 1001;
constexpr int32_t kOpenAiMain = 1002;
constexpr int32_t kOpenAiSettings = 1003;
constexpr int32_t kOpenPluginManager = 1004;
constexpr int32_t kOpenWorkshop = 1005;
constexpr int32_t kOpenProcessSelector = 1006;
constexpr int32_t kOpenLicense = 1007;
constexpr int32_t kOpenUserMenu = 1008;
constexpr int32_t kOpenAboutGuide = 1009;
constexpr int32_t kOpenMemoryViewer = 1010;
constexpr size_t kPreviewPluginCount = 16;

void require(sao_status_t status,
             const std::source_location source = std::source_location::current()) {
    if (status != SAO_STATUS_OK)
        throw std::runtime_error("Production UI API status " + std::to_string(status) + " at " +
                                 source.function_name() + ":" + std::to_string(source.line()));
}
void require_sdk(sao_sdk_status_t status) {
    if (status != SAO_SDK_OK)
        throw std::runtime_error("Production SDK API status " + std::to_string(status));
}
void require_active(sao_status_t status,
                    const std::source_location source = std::source_location::current()) {
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_INITIALIZED)
        require(status, source);
}

enum class InitialSurface {
    root,
    ai_main,
    ai_settings,
    settings,
    hotkeys,
    plugins,
    workshop,
    process,
    license,
    user,
    files,
    about
};
struct Options {
    InitialSurface initial_surface{InitialSurface::root};
    bool offline{};
    bool offline_explicit{};
    bool backend_explicit{};
    bool local_backend_fixture{};
    bool intro{};
    bool intro_audition{};
    bool intro_handoff{};
    bool outro{};
    bool menu_motion{};
    bool plugin_tabs{};
    bool shared_texture_probe{};
    bool readability_probe{};
    std::filesystem::path workspace{std::filesystem::current_path()};
    std::filesystem::path backend;
    std::filesystem::path settings;
    std::filesystem::path frame_out;
    int32_t frame_ms{1000};
    int32_t frame_count{1};
    bool frame_time_explicit{};
};

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= static_cast<DWORD>(path.size()))
        throw std::runtime_error("Preview executable path unavailable");
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

std::filesystem::path sibling_executable(std::wstring_view name) {
    return executable_directory() / name;
}

// The Workshop owner remains the production page.  This provider only reads
// the real client configuration and deliberately exposes a detached backend
// state, so opening the page never schedules a network request in the shell.
sao_status_t detached_workshop_status() noexcept {
    char endpoint[512]{};
    size_t written = 0;
    (void)sao_workshop_client_get_base_url(endpoint, std::size(endpoint), &written);
    return SAO_STATUS_ERR_NOT_INITIALIZED;
}

sao::launcher::workshop_panel::Operations detached_workshop_operations() {
    using namespace sao::launcher::workshop_panel;
    Operations operations;
    operations.list = [](std::stop_token stop, std::uint32_t, std::uint32_t, PluginPage& output) {
        output = {};
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.detail = [](std::stop_token stop, std::string_view, PluginDetail& output) {
        output = {};
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.download = [](std::stop_token stop, std::string_view, const std::filesystem::path&,
                             std::filesystem::path& output) {
        output.clear();
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.verify = [](std::stop_token stop, const std::filesystem::path&, std::string_view) {
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.install = [](std::stop_token stop, const std::filesystem::path&,
                            const std::filesystem::path&) {
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.uninstall = [](std::stop_token stop, std::string_view, const std::filesystem::path&,
                              bool) {
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    return operations;
}

Options options() {
    Options value;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr)
        throw std::runtime_error("Arguments unavailable");
    struct ArgsRelease {
        LPWSTR* value;
        ~ArgsRelease() {
            LocalFree(value);
        }
    } release{args};
    for (int index = 1; index < count; ++index) {
        const std::wstring arg = args[index];
        if (arg == L"--offline") {
            value.offline = true;
            value.offline_explicit = true;
        } else if (arg == L"--ai-main" || arg == L"--main")
            value.initial_surface = InitialSurface::ai_main;
        else if (arg == L"--ai-settings")
            value.initial_surface = InitialSurface::ai_settings;
        else if (arg == L"--page" && index + 1 < count) {
            const std::wstring page = args[++index];
            if (page == L"root")
                value.initial_surface = InitialSurface::root;
            else if (page == L"settings")
                value.initial_surface = InitialSurface::settings;
            else if (page == L"hotkeys")
                value.initial_surface = InitialSurface::hotkeys;
            else if (page == L"plugins")
                value.initial_surface = InitialSurface::plugins;
            else if (page == L"workshop")
                value.initial_surface = InitialSurface::workshop;
            else if (page == L"process")
                value.initial_surface = InitialSurface::process;
            else if (page == L"license")
                value.initial_surface = InitialSurface::license;
            else if (page == L"user")
                value.initial_surface = InitialSurface::user;
            else if (page == L"files")
                value.initial_surface = InitialSurface::files;
            else if (page == L"about" || page == L"guide")
                value.initial_surface = InitialSurface::about;
            else if (page == L"ai-main")
                value.initial_surface = InitialSurface::ai_main;
            else if (page == L"ai-settings")
                value.initial_surface = InitialSurface::ai_settings;
            else if (page == L"link-start") {
                value.initial_surface = InitialSurface::root;
                value.intro = true;
            } else
                throw std::runtime_error("Unknown production UI page");
        } else if (arg == L"--intro")
            value.intro = true;
        else if (arg == L"--intro-handoff") {
            value.intro = true;
            value.intro_handoff = true;
        }
        else if (arg == L"--outro")
            value.outro = true;
        else if (arg == L"--menu-motion")
            value.menu_motion = true;
        else if (arg == L"--plugin-tabs")
            value.plugin_tabs = true;
        else if (arg == L"--shared-texture-probe")
            value.shared_texture_probe = true;
        else if (arg == L"--readability-probe")
            value.readability_probe = true;
        else if (arg == L"--intro-audition") {
            value.intro = true;
            value.intro_audition = true;
            value.offline = true;
            value.offline_explicit = true;
        }
        else if (arg == L"--workspace" && index + 1 < count)
            value.workspace = std::filesystem::absolute(args[++index]);
        else if (arg == L"--settings" && index + 1 < count)
            value.settings = std::filesystem::absolute(args[++index]);
        else if (arg == L"--frame-out" && index + 1 < count) {
            const std::filesystem::path output = args[++index];
            value.frame_out = output == L"-" ? output : std::filesystem::absolute(output);
        }
        else if (arg == L"--frame-ms" && index + 1 < count) {
            const std::wstring input = args[++index];
            size_t consumed = 0;
            const long long milliseconds = std::stoll(input, &consumed);
            if (consumed != input.size() || milliseconds < 0 || milliseconds > 60000)
                throw std::runtime_error("Frame time must be 0..60000 milliseconds");
            value.frame_ms = static_cast<int32_t>(milliseconds);
            value.frame_time_explicit = true;
        }
        else if (arg == L"--frame-count" && index + 1 < count) {
            const std::wstring input = args[++index];
            size_t consumed = 0;
            const long long frames = std::stoll(input, &consumed);
            if (consumed != input.size() || frames < 1 || frames > 1800)
                throw std::runtime_error("Frame count must be 1..1800");
            value.frame_count = static_cast<int32_t>(frames);
        }
        else if (arg == L"--backend" && index + 1 < count) {
            if (value.local_backend_fixture)
                throw std::runtime_error("Preview backend selection is ambiguous");
            value.backend = std::filesystem::absolute(args[++index]);
            value.backend_explicit = true;
        } else if (arg == L"--local-backend") {
            if (value.backend_explicit)
                throw std::runtime_error("Preview backend selection is ambiguous");
            value.backend_explicit = true;
            value.local_backend_fixture = true;
        } else
            throw std::runtime_error(
                "Usage: sao_ui_preview [--main|--ai-main|--ai-settings] [--page "
                "root|settings|hotkeys|plugins|workshop|process|license|user|about|link-start] "
                "[--offline] [--backend EXE|--local-backend] [--intro|--intro-audition|--intro-handoff|--outro] [--workspace PATH] "
                "[--settings PATH] [--frame-out BMP_PATH|- --frame-ms MILLISECONDS --offline] "
                "[--frame-count COUNT] [--menu-motion|--plugin-tabs|--readability-probe] [--offline --shared-texture-probe]");
    }
    if (value.shared_texture_probe &&
        (!value.offline_explicit || !value.offline || value.backend_explicit || value.intro ||
         value.outro || value.menu_motion || value.plugin_tabs || value.readability_probe || !value.frame_out.empty() ||
         value.frame_time_explicit || value.frame_count != 1 ||
         value.initial_surface != InitialSurface::root))
        throw std::runtime_error("Shared texture probe requires explicit --offline without backend, intro, outro, page, or other probe/export modes");
    if (value.readability_probe && (!value.offline_explicit || !value.offline ||
        value.backend_explicit || value.intro || value.outro || value.menu_motion ||
        value.plugin_tabs || value.shared_texture_probe || value.frame_count != 1 ||
        value.frame_out.empty() || value.frame_out == L"-" ||
        value.initial_surface != InitialSurface::root))
        throw std::runtime_error("Readability probe requires offline root single-frame export without other modes");
    if (value.outro && value.intro)
        throw std::runtime_error("--outro cannot be combined with --intro, --intro-audition, or --page link-start");
    if (value.outro) {
        value.offline = true;
        value.offline_explicit = true;
    }
    if (value.backend_explicit && !value.offline_explicit)
        value.offline = false;
    if ((!value.frame_out.empty() && !value.offline) ||
        (value.frame_time_explicit && value.frame_out.empty()))
        throw std::runtime_error("Frame export requires --offline and --frame-out");
    if (value.intro_audition && !value.frame_out.empty())
        throw std::runtime_error("Audition and muted frame export are separate modes");
    if (value.intro_handoff && (!value.offline_explicit || !value.offline ||
        value.backend_explicit || value.frame_out.empty() || value.intro_audition ||
        value.initial_surface != InitialSurface::root))
        throw std::runtime_error("Intro handoff requires explicit offline root frame export without a backend");
    if (value.frame_count > 1 && value.frame_out != L"-")
        throw std::runtime_error("Continuous frames require --frame-out -");
    if (value.frame_ms + (value.frame_count - 1) * 1000 / 60 > 60000)
        throw std::runtime_error("Frame sequence must finish within 60000ms");
    if (value.menu_motion && (!value.offline || value.backend_explicit || value.frame_out.empty() || value.intro ||
                             value.outro || value.initial_surface != InitialSurface::root))
        throw std::runtime_error("Menu motion requires offline root frame export without backend/intro/outro");
    if (value.plugin_tabs && (!value.offline_explicit || !value.offline || value.backend_explicit ||
                             value.intro || value.outro || value.menu_motion ||
                             value.initial_surface != InitialSurface::root))
        throw std::runtime_error("Plugin tabs require explicit --offline root preview without backend/intro/outro/menu-motion");
    return value;
}

struct SharedTextureProbe {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    static constexpr uint32_t extent = 8;
    sao_ui_overlay_host_handle_t overlay{};
    sao_ui_compositor_handle_t compositor{};
    sao_ui_layer_handle_t layer{};
    sao_ui_layer_handle_t device_layer{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device> producer;
    ComPtr<ID3D11DeviceContext> producer_context;
    size_t checks{};
    const char* variant{"setup"};
    unsigned cycle{};
    bool teardown_failed{};

    struct Fixture {
        ComPtr<ID3D11Texture2D> original;
        ComPtr<ID3D11Texture2D> writable;
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<IDXGIKeyedMutex> mutex;
        HANDLE handle{};
        bool nt{};
        bool locked{};
        ~Fixture() {
            if (locked) (void)mutex->ReleaseSync(0);
            if (nt && handle != nullptr) CloseHandle(handle);
        }
    };

    void check(bool passed, const char* name) {
        ++checks;
        std::fprintf(stderr, "SHARED_TEXTURE_VERIFY variant=%s cycle=%u check=%s pass=%d sequence=%zu\n",
                     variant, cycle, name, passed ? 1 : 0, checks);
        std::fflush(stderr);
        if (!passed) throw std::runtime_error(name);
    }

    void hr(HRESULT result, const char* name) {
        if (result != S_OK)
            std::fprintf(stderr, "SHARED_TEXTURE_HRESULT operation=%s value=0x%08lX\n",
                         name, static_cast<unsigned long>(result));
        check(result == S_OK, name);
    }

    static sao_status_t SAO_UI_CALL capture_device(const SaoUiD3d11LayerRenderContext* frame,
                                                  void* data) {
        if (frame == nullptr || frame->struct_size < sizeof(*frame) || frame->d3d11_device == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        static_cast<SharedTextureProbe*>(data)->device = static_cast<ID3D11Device*>(frame->d3d11_device);
        return SAO_STATUS_OK;
    }

    SaoUiSharedTextureState state(const char* name) {
        SaoUiSharedTextureState value{};
        value.struct_size = sizeof(value);
        require(sao_ui_layer_get_shared_texture_state(layer, &value));
        std::fprintf(stderr,
                     "SHARED_TEXTURE_STATE check=%s configured=%u imported=%u active=%u status=%d hresult=0x%08X width=%u height=%u generation=%llu acquired=%llu\n",
                     name, value.configured, value.imported, value.active, value.last_status,
                     static_cast<unsigned>(value.last_hresult), value.width, value.height,
                     static_cast<unsigned long long>(value.generation),
                     static_cast<unsigned long long>(value.acquired_frames));
        return value;
    }

    void pixels(const std::array<uint8_t, 4>& rgba, bool visible, const char* name) {
        uint32_t width = 0, height = 0;
        size_t bytes = 0;
        require(sao_ui_layer_request_redraw(layer));
        require(sao_ui_compositor_tick(compositor));
        const auto query = sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0, &width, &height, &bytes);
        if (query != SAO_STATUS_ERR_BUFFER_TOO_SMALL) require(query);
        check(width == 64 && height == 64 && bytes == size_t{64} * 64 * 4, "snapshot-shape");
        std::vector<uint8_t> image(bytes);
        require(sao_ui_compositor_snapshot_bgra(compositor, image.data(), image.size(),
                                               &width, &height, &bytes));
        check(width == 64 && height == 64 && bytes == image.size(), "snapshot-filled-shape");
        const std::array<uint8_t, 4> expected{
            static_cast<uint8_t>((unsigned(rgba[2]) * rgba[3] + 127) / 255),
            static_cast<uint8_t>((unsigned(rgba[1]) * rgba[3] + 127) / 255),
            static_cast<uint8_t>((unsigned(rgba[0]) * rgba[3] + 127) / 255), rgba[3]};
        size_t mismatches = 0;
        uint64_t hash = 14695981039346656037ull;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const bool colored = visible && x >= 8 && x < 8 + extent && y >= 8 && y < 8 + extent;
                for (size_t channel = 0; channel < 4; ++channel) {
                    const uint8_t actual = image[(size_t{y} * width + x) * 4 + channel];
                    const int wanted = colored ? expected[channel] : 0;
                    const int difference = static_cast<int>(actual) - wanted;
                    const int tolerance = colored && rgba[3] != 255 && channel < 3 ? 1 : 0;
                    mismatches += difference < -tolerance || difference > tolerance;
                    hash = (hash ^ actual) * 1099511628211ull;
                }
            }
        }
        std::fprintf(stderr,
                     "SHARED_TEXTURE_PIXELS variant=%s cycle=%u check=%s width=%u height=%u bytes=%zu expected_bgra=%u,%u,%u,%u visible=%d translucent_rgb_tolerance=1 alpha_tolerance=0 mismatches=%zu fnv64=%016llX\n",
                     variant, cycle, name, width, height, bytes, unsigned(expected[0]),
                     unsigned(expected[1]), unsigned(expected[2]), unsigned(expected[3]),
                     visible ? 1 : 0, mismatches, static_cast<unsigned long long>(hash));
        check(mismatches == 0, name);
    }

    void initialize() {
        SaoOverlayHostConfig host{};
        host.width = 64;
        host.height = 64;
        host.title_utf16 = L"SAO offline shared texture probe";
        require(sao_ui_overlay_host_create(&host, &overlay));
        require(sao_ui_compositor_create(overlay, nullptr, &compositor));
        bool available = false;
        require(sao_ui_compositor_gpu_interop_available(compositor, &available));
        check(available, "gpu-interop-available");
        SaoLayerConfig config{};
        config.struct_size = sizeof(config);
        config.name_utf8 = "preview.shared-texture.device";
        config.width = 64;
        config.height = 64;
        config.click_through = true;
        config.high_fps = true;
        require(sao_ui_layer_create(compositor, &config, &device_layer));
        require(sao_ui_layer_set_d3d11_render_fn(device_layer, capture_device, this));
        require(sao_ui_layer_set_visible(device_layer, true));
        require(sao_ui_compositor_tick(compositor));
        check(device.Get() != nullptr, "compositor-device-callback");
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        hr(device.As(&dxgi_device), "consumer-dxgi-device");
        hr(dxgi_device->GetAdapter(&adapter), "consumer-adapter");
        D3D_FEATURE_LEVEL selected{};
        hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                             &producer, &selected, &producer_context), "second-device");
        ComPtr<IDXGIDevice> producer_dxgi;
        ComPtr<IDXGIAdapter> producer_adapter;
        DXGI_ADAPTER_DESC consumer_desc{}, producer_desc{};
        hr(producer.As(&producer_dxgi), "producer-dxgi-device");
        hr(producer_dxgi->GetAdapter(&producer_adapter), "producer-adapter");
        hr(adapter->GetDesc(&consumer_desc), "consumer-adapter-description");
        hr(producer_adapter->GetDesc(&producer_desc), "producer-adapter-description");
        check(device.Get() != producer.Get() &&
              consumer_desc.AdapterLuid.LowPart == producer_desc.AdapterLuid.LowPart &&
              consumer_desc.AdapterLuid.HighPart == producer_desc.AdapterLuid.HighPart,
              "distinct-devices-same-adapter");
        std::fprintf(stderr, "SHARED_TEXTURE_DEVICES count=2 same_adapter=1 luid=%08X:%08lX feature_level=%u cross_process=0\n",
                     static_cast<unsigned>(consumer_desc.AdapterLuid.HighPart),
                     consumer_desc.AdapterLuid.LowPart, static_cast<unsigned>(selected));
        config.name_utf8 = "preview.shared-texture.fixture";
        config.width = extent;
        config.height = extent;
        config.x = 8;
        config.y = 8;
        require(sao_ui_layer_create(compositor, &config, &layer));
        require(sao_ui_layer_set_visible(layer, true));
        const auto initial = state("initial-inactive");
        check(!initial.configured && !initial.imported && !initial.active, "initial-inactive");
        pixels({}, false, "initial-clear-pixels");
    }

    std::unique_ptr<Fixture> make_fixture(bool nt, bool keyed) {
        auto fixture = std::make_unique<Fixture>();
        fixture->nt = nt;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = extent;
        desc.Height = extent;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
        if (nt) desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        hr(device->CreateTexture2D(&desc, nullptr, &fixture->original), "create-shared-rgba8");
        if (nt) {
            ComPtr<IDXGIResource1> resource;
            ComPtr<ID3D11Device1> producer1;
            hr(fixture->original.As(&resource), "nt-resource-interface");
            hr(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                             nullptr, &fixture->handle), "nt-shared-handle");
            hr(producer.As(&producer1), "producer-device1");
            hr(producer1->OpenSharedResource1(fixture->handle, IID_PPV_ARGS(&fixture->writable)),
               "nt-open-second-device");
        } else {
            ComPtr<IDXGIResource> resource;
            hr(fixture->original.As(&resource), "legacy-resource-interface");
            hr(resource->GetSharedHandle(&fixture->handle), "legacy-shared-handle");
            hr(producer->OpenSharedResource(fixture->handle, IID_PPV_ARGS(&fixture->writable)),
               "legacy-open-second-device");
        }
        if (keyed) hr(fixture->writable.As(&fixture->mutex), "producer-keyed-mutex");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr(producer->CreateTexture2D(&desc, nullptr, &fixture->staging), "producer-readback-texture");
        return fixture;
    }

    void publish(Fixture& fixture, const std::array<uint8_t, 4>& rgba, bool retain_lock = false) {
        if (fixture.mutex && !fixture.locked) {
            hr(fixture.mutex->AcquireSync(0, 50), "producer-acquire-key0");
            fixture.locked = true;
        }
        std::array<uint8_t, extent * extent * 4> data{};
        for (size_t offset = 0; offset < data.size(); offset += 4)
            std::copy(rgba.begin(), rgba.end(), data.begin() + offset);
        producer_context->UpdateSubresource(fixture.writable.Get(), 0, nullptr, data.data(), extent * 4, 0);
        producer_context->CopyResource(fixture.staging.Get(), fixture.writable.Get());
        producer_context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr(producer_context->Map(fixture.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "producer-readback-map");
        bool equal = mapped.RowPitch >= extent * 4;
        if (equal) {
            for (uint32_t row = 0; row < extent; ++row)
                equal = equal && std::memcmp(static_cast<const uint8_t*>(mapped.pData) + size_t{row} * mapped.RowPitch,
                                             data.data() + size_t{row} * extent * 4, extent * 4) == 0;
        }
        producer_context->Unmap(fixture.staging.Get(), 0);
        check(equal, "producer-known-pixels-completed");
        if (fixture.locked && !retain_lock) {
            hr(fixture.mutex->ReleaseSync(0), "producer-release-key0");
            fixture.locked = false;
        }
    }

    void run_variant(bool nt, bool keyed) {
        variant = nt ? "nt-keyed" : keyed ? "legacy-keyed" : "legacy-plain";
        std::unique_ptr<Fixture> current;
        for (cycle = 0; cycle < 4; ++cycle) {
            auto next = make_fixture(nt, keyed);
            const std::array<uint8_t, 4> rgba{static_cast<uint8_t>(37 + cycle * 23), 113, 201,
                                             static_cast<uint8_t>(cycle == 1 ? 128 : 255)};
            publish(*next, rgba);
            SaoUiSharedTextureSource source{};
            source.struct_size = sizeof(source);
            source.handle_kind = nt ? SAO_UI_SHARED_HANDLE_NT : SAO_UI_SHARED_HANDLE_LEGACY_DXGI;
            source.shared_handle = reinterpret_cast<uintptr_t>(next->handle);
            source.width = extent;
            source.height = extent;
            source.acquire_key = 0;
            source.release_key = 0;
            source.timeout_ms = 1;
            const auto before = state("before-replacement");
            if (!nt && !keyed && cycle == 0)
                require(sao_ui_layer_set_shared_texture(layer, next->handle, extent, extent));
            else
                require(sao_ui_layer_set_shared_texture_ex(layer, &source));
            const auto pending = state("import-before-first-frame");
            check(pending.configured && pending.imported && !pending.active &&
                  pending.last_status == SAO_STATUS_OK && pending.last_hresult == S_OK &&
                  pending.width == extent && pending.height == extent &&
                  pending.acquired_frames == 0 && pending.generation > before.generation,
                  "import-inactive-before-first-frame");
            current = std::move(next);
            pixels(rgba, true, "import-render-known-pixels");
            auto active = state("import-active");
            check(active.configured && active.imported && active.active &&
                  active.last_status == SAO_STATUS_OK && active.last_hresult == S_OK &&
                  active.width == extent && active.height == extent &&
                  active.generation > before.generation && active.acquired_frames > 0, "import-active");
            const auto invalid = [&](SaoUiSharedTextureSource candidate, const char* name,
                                     sao_status_t expected_status, HRESULT expected_hresult) {
                const auto old = state("before-invalid");
                const auto status = sao_ui_layer_set_shared_texture_ex(layer, &candidate);
                const auto kept = state(name);
                std::fprintf(stderr, "SHARED_TEXTURE_REJECT check=%s status=%d\n", name, status);
                                check(status != SAO_STATUS_OK && kept.last_status == status &&
                                            (expected_status == SAO_STATUS_OK ? FAILED(kept.last_hresult) :
                                             status == expected_status && kept.last_hresult == expected_hresult) &&
                                            kept.configured == old.configured &&
                      kept.imported == old.imported && kept.active == old.active &&
                      kept.generation == old.generation && kept.width == old.width && kept.height == old.height &&
                      kept.acquired_frames == old.acquired_frames, name);
                pixels(rgba, true, "invalid-retains-render");
            };
            auto malformed = source;
            malformed.width = 0;
            invalid(malformed, "zero-width-rejected", SAO_STATUS_ERR_INVALID_ARGUMENT, E_INVALIDARG);
            malformed = source;
            malformed.height = extent - 1;
            invalid(malformed, "mismatched-dimensions-rejected", SAO_STATUS_ERR_INVALID_ARGUMENT, E_INVALIDARG);
            malformed = source;
            malformed.handle_kind = 99;
            invalid(malformed, "invalid-handle-kind-rejected", SAO_STATUS_ERR_INVALID_ARGUMENT, E_INVALIDARG);
            malformed = source;
            malformed.struct_size = sizeof(uint32_t);
            invalid(malformed, "short-source-rejected", SAO_STATUS_ERR_ABI_MISMATCH, E_INVALIDARG);
            const auto close_event = [](void* value) { if (value != nullptr) CloseHandle(value); };
            std::unique_ptr<void, decltype(close_event)> event(
                CreateEventW(nullptr, TRUE, FALSE, nullptr), close_event);
            check(event != nullptr, "non-texture-handle-created");
            malformed = source;
            malformed.handle_kind = SAO_UI_SHARED_HANDLE_NT;
            malformed.shared_handle = reinterpret_cast<uintptr_t>(event.get());
            invalid(malformed, "non-texture-handle-rejected", SAO_STATUS_OK, S_OK);
            if (nt) {
                check(CloseHandle(current->handle) != FALSE, "nt-close-caller-handle");
                current->handle = nullptr;
                pixels(rgba, true, "nt-closed-caller-handle-import-retains-render");
            }
            if (keyed) {
                const std::array<uint8_t, 4> changed{221, 59, 17, 255};
                active = state("before-key0-timeout");
                publish(*current, changed, true);
                pixels(rgba, true, "key0-timeout-retains-last-frame");
                const auto timed_out = state("key0-timeout");
                check(timed_out.configured && timed_out.imported && timed_out.active &&
                      timed_out.last_status == SAO_STATUS_ERR_TIMEOUT &&
                      timed_out.last_hresult == static_cast<int32_t>(WAIT_TIMEOUT) &&
                      timed_out.generation == active.generation &&
                      timed_out.acquired_frames == active.acquired_frames, "key0-timeout-state");
                hr(current->mutex->ReleaseSync(0), "timeout-release-key0");
                current->locked = false;
                pixels(changed, true, "key0-resume-new-frame");
                const auto resumed = state("key0-resumed");
                                check(resumed.configured && resumed.imported && resumed.active &&
                                            resumed.last_status == SAO_STATUS_OK && resumed.last_hresult == S_OK &&
                                            resumed.generation == timed_out.generation &&
                      resumed.acquired_frames > timed_out.acquired_frames, "key0-resume-state");
            }
        }
                const auto attached = state("before-clear");
                require(sao_ui_layer_set_shared_texture_ex(layer, nullptr));
        const auto cleared = state("cleared-inactive");
                check(!cleared.configured && !cleared.imported && !cleared.active &&
              cleared.last_status == SAO_STATUS_OK && cleared.last_hresult == S_OK &&
              cleared.width == 0 && cleared.height == 0 && cleared.acquired_frames == 0 &&
              cleared.generation > attached.generation, "cleared-inactive");
        current.reset();
        pixels({}, false, "clear-removes-last-frame");
        require(sao_ui_layer_set_shared_texture_ex(layer, nullptr));
        const auto again = state("repeated-clear");
                check(!again.configured && !again.imported && !again.active &&
              again.last_status == SAO_STATUS_OK && again.last_hresult == S_OK &&
              again.width == 0 && again.height == 0 && again.acquired_frames == 0 &&
              again.generation > cleared.generation, "repeated-clear-inactive");
    }

    bool close() noexcept {
        if (device_layer != nullptr) {
            sao_ui_layer_destroy(device_layer);
            device_layer = nullptr;
        }
        if (layer != nullptr) {
            sao_ui_layer_destroy(layer);
            SaoUiSharedTextureState retired{};
            retired.struct_size = sizeof(retired);
            const auto status = sao_ui_layer_get_shared_texture_state(layer, &retired);
            std::fprintf(stderr, "SHARED_TEXTURE_TEARDOWN retired_layer_status=%d\n", status);
            layer = nullptr;
            teardown_failed = teardown_failed || status != SAO_STATUS_ERR_HANDLE_INVALID;
        }
        if (producer_context) {
            producer_context->ClearState();
            producer_context->Flush();
        }
        producer_context.Reset();
        producer.Reset();
        device.Reset();
        if (compositor != nullptr) {
            const auto status = sao_ui_compositor_try_destroy(compositor);
            std::fprintf(stderr, "SHARED_TEXTURE_TEARDOWN compositor_status=%d\n", status);
            if (status != SAO_STATUS_OK) {
                teardown_failed = true;
                return false;
            }
            compositor = nullptr;
        }
        if (overlay != nullptr) {
            const bool destroyed = sao_ui_overlay_host_destroy(overlay);
            std::fprintf(stderr, "SHARED_TEXTURE_TEARDOWN overlay_destroyed=%d\n", destroyed ? 1 : 0);
            if (!destroyed) {
                teardown_failed = true;
                return false;
            }
            overlay = nullptr;
        }
        return !teardown_failed;
    }

    ~SharedTextureProbe() { (void)close(); }
};

int run_shared_texture_probe() {
    SharedTextureProbe probe;
    try {
        probe.initialize();
        probe.run_variant(false, false);
        probe.run_variant(false, true);
        probe.run_variant(true, true);
        probe.check(probe.close(), "normal-teardown");
        std::fprintf(stderr, "SHARED_TEXTURE_PROBE_OK variants=3 replacements=12 devices=2 cross_process=0 checks=%zu failures=0\n",
                     probe.checks);
        return 0;
    } catch (const std::exception& error) {
        const bool closed = probe.close();
        std::fprintf(stderr, "SHARED_TEXTURE_PROBE_FAIL variant=%s cycle=%u checks=%zu teardown=%d reason=%s\n",
                     probe.variant, probe.cycle, probe.checks, closed ? 1 : 0, error.what());
        return 1;
    }
}

// One production DComp/compositor owner. The shell intentionally does not run
// the launcher pipeline, driver, or plugin discovery; AI backend launch occurs
// only when the caller explicitly supplies --backend without --offline.
struct Host {
    Options config;
    std::filesystem::path base_dir;
    HWND window{};
    sao_ui_overlay_host_handle_t overlay{};
    sao_ui_compositor_handle_t compositor{};
    sao_ui_entity_shell_handle_t entity{};
    sao_ui_plugin_tabs_handle_t plugin_tabs{};
    sao_ui_panel_handle_t plugin_canvas_panel{};
    SaoSdkContext plugin_draw_context{};
    bool plugin_draw_bound{};
    bool plugin_tabs_hidden{};
    uint32_t plugin_catalog_revision{};
    bool plugin_sample_removed{};
    uint32_t plugin_action_count{};
    int32_t plugin_last_action{-1};
    sao_status_t plugin_last_status{SAO_STATUS_OK};
    size_t readability_checks{};
    uint32_t readability_action_count{};
    int32_t readability_last_action{-1};
    sao_ui_fisheye_backdrop_handle_t backdrop{};
    sao_ui_input_router_deep_handle_t keyboard{};
    sao_ui_linkstart_handle_t intro{};
    bool startup_menu_pending{};
    sao_ai_editor_settings_panel_t ai_settings{};
    sao_ai_editor_main_panel_t ai_main{};
    sao_ai_editor_launcher_t backend{};
    sao_ui_file_picker_handle_t file_picker{};
    std::unique_ptr<sao::launcher::settings_owner::SettingsOwner> settings_owner;
    std::unique_ptr<sao::launcher::hotkey::Owner> hotkey_owner;
    std::unique_ptr<sao::launcher::plugin_manager_panel::Owner> plugin_manager;
    std::unique_ptr<sao::launcher::process_selector_panel::Owner> process_selector;
    std::unique_ptr<sao::launcher::memory_viewer_panel::Owner> memory_viewer;
    std::unique_ptr<sao::launcher::workshop_panel::Owner> workshop;
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
    std::unique_ptr<sao::launcher::license_panel::Owner> license;
#endif
    sao::launcher::UserMenu user_menu;
    bool user_menu_created{};
    ULONGLONG last_tick{};
    ULONGLONG last_service{};
    ULONGLONG intro_started{};
    int32_t audition_phase{-1};
    bool audition_flight_reported{};
    int32_t outro_phase{-1};
    ULONGLONG outro_elapsed{};
    bool outro_completed{};
    bool outro_failed{};
    uint32_t frame_width{};
    uint32_t frame_height{};
    bool sdk_bound{};
    bool closing{};
    bool shutdown_requested{};
    bool message_failed{};
    std::optional<int> quit_code;

    explicit Host(Options value)
        : config(std::move(value)), base_dir(executable_directory()) {}
    ~Host() {
        (void)close();
        (void)sao_ui_sound_shutdown();
    }

    sao_status_t publish_plugin_samples() {
        static constexpr SaoUiPluginTabAction disabled_actions[] = {
            {sizeof(SaoUiPluginTabAction), "已停用功能 / Disabled action", "sao:info", 2001, false, true, false, 0},
        };
        static constexpr SaoUiPluginTabAction initial_actions[] = {
            {sizeof(SaoUiPluginTabAction), "刷新菜单 / Refresh", "↻", 2000, true, true, false, 0},
            {sizeof(SaoUiPluginTabAction), "查看状态 / Inspect", "◎", 2001, true, true, false, 0},
            {sizeof(SaoUiPluginTabAction), "关闭后执行 / Close first", "×", 2002, true, false, true, 0},
        };
        static constexpr SaoUiPluginTabAction refreshed_actions[] = {
            {sizeof(SaoUiPluginTabAction), "再次刷新 / Refresh again", "↻", 2000, true, true, false, 0},
            {sizeof(SaoUiPluginTabAction), "已更新 / Updated status", "✓", 2101, true, true, false, 0},
            {sizeof(SaoUiPluginTabAction), "新增信息 / New information", "◇", 2102, true, true, false, 0},
            {sizeof(SaoUiPluginTabAction), "记录示例 / Log sample", "♫", 2103, true, true, false, 0},
            {sizeof(SaoUiPluginTabAction), "验证新增行 / Verify new row", "★", 2104, true, true, false, 0},
        };
        struct Sample { const char* id; const char* name; const char* icon; };
        static constexpr Sample samples[] = {
            {"preview.razorworks", "Razorworks", "⚔"},
            {"preview.cutegirl", "看板娘 / CuteGirl", "♡"},
            {"preview.flappy", "Flappy Bird", "◆"},
            {"preview.snake", "贪吃蛇 / Snake", "●"},
            {"preview.world-clock", "世界时钟 / World Clock", "◷"},
            {"preview.music", "音乐 / Music", "♪"},
            {"preview.notes", "便笺 / Notes", "✎"},
            {"preview.weather", "天气 / Weather", "☀"},
            {"preview.calendar", "日历 / Calendar", "▦"},
            {"preview.compass", "指南针 / Compass", "✦"},
            {"preview.monitor", "监视器 / Monitor", "▣"},
            {"preview.extra", "更多 / Overflow", "∞"},
            {"preview.camera", "相机 / Camera", "◉"},
            {"preview.search", "搜索 / Search", "⌕"},
            {"preview.settings", "配置 / Configure", "⚙"},
            {"preview.help", "帮助 / Help", "?"},
        };
        static_assert(std::size(samples) == kPreviewPluginCount);
        std::array<SaoUiPluginTab, std::size(samples)> items{};
        const bool refreshed = (plugin_catalog_revision & 1U) != 0;
        size_t count = 0;
        for (size_t index = plugin_sample_removed ? 1 : 0; index < std::size(samples); ++index) {
            auto& item = items[count++];
            item.struct_size = sizeof(item);
            item.plugin_id_utf8 = samples[index].id;
            item.name_utf8 = index == 0 && refreshed ? "Razorworks · 已刷新" : samples[index].name;
            item.icon_utf8 = samples[index].icon;
            item.actions = refreshed ? refreshed_actions : initial_actions;
            item.action_count = refreshed ? std::size(refreshed_actions) : std::size(initial_actions);
            item.enabled = true;
            item.status = config.readability_probe && index == 2 ? SAO_UI_PLUGIN_TAB_STATUS_UNKNOWN
                                                               : SAO_UI_PLUGIN_TAB_STATUS_ACTIVE;
            if (config.readability_probe && index == 1) {
                item.status = SAO_UI_PLUGIN_TAB_STATUS_DISABLED;
                item.actions = disabled_actions;
                item.action_count = std::size(disabled_actions);
            }
        }
        const auto status = sao_ui_plugin_tabs_set_items(plugin_tabs, items.data(), count);
        std::fprintf(stderr, "PLUGIN_TABS_CATALOG revision=%u tabs=%zu rows=%zu removed=%d status=%d first_label=%s\n",
                     plugin_catalog_revision, count, refreshed ? std::size(refreshed_actions) :
                 std::size(initial_actions), plugin_sample_removed ? 1 : 0, status, items[0].name_utf8);
        std::fflush(stderr);
        return status;
    }

    sao_status_t show_plugin_canvas() {
        if (plugin_canvas_panel == nullptr) {
            SaoPanelConfig panel{};
            panel.panel_id_utf8 = "preview.plugin-canvas";
            panel.title_utf8 = "插件绘制 / Native ctx";
            panel.default_x = 770;
            panel.default_y = 130;
            panel.default_width = 390;
            panel.default_height = 300;
            panel.movable = true;
            panel.show_titlebar = true;
            panel.show_close_button = true;
            const auto status = sao_ui_panel_create(compositor, &panel, &plugin_canvas_panel);
            if (status != SAO_STATUS_OK)
                return status;
        }
        static constexpr char spec[] = R"json({"type":"panel","title":"内部绘制 ctx","children":[
            {"type":"canvas","id":"draw-proof","width":340,"height":220,"bg":"#202830","ops":[
                {"op":"rect","x":20,"y":20,"w":120,"h":65,"fill":"#18c8d0","outline":"#ffffff","width":2},
                {"op":"oval","x":180,"y":20,"w":70,"h":70,"fill":"#f4ba4c","outline":"#ffffff","width":2},
                {"op":"line","x1":20,"y1":110,"x2":300,"y2":110,"fill":"#ffffff","width":3},
                {"op":"text","x":24,"y":150,"text":"插件 → 原生绘制 ctx","fill":"#ffffff","size":18,"anchor":"nw"}
            ]},
            {"type":"rgba_frame","id":"rgba-proof","width":2,"height":2,
             "frame_rgba_b64":"/wAA/wD/AIAAAP//AAAAAA==","premultiplied":false}
            ]})json";
        auto status = sao_ui_panel_set_spec(plugin_canvas_panel,
            reinterpret_cast<const uint8_t*>(spec), sizeof(spec) - 1);
        sao_ui_widget_handle_t widget{};
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_find_widget(plugin_canvas_panel, "draw-proof", &widget);
        size_t count = 0;
        if (status == SAO_STATUS_OK) {
            status = sao_ui_script_canvas_snapshot_ops(
                reinterpret_cast<sao_ui_script_canvas_handle_t>(widget), nullptr, 0, &count);
            if (status == SAO_STATUS_ERR_BUFFER_TOO_SMALL && count >= 4)
                status = SAO_STATUS_OK;
            else if (status == SAO_STATUS_OK && count < 4)
                status = SAO_STATUS_ERR_NOT_FOUND;
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_set_visible(plugin_canvas_panel, true);
        std::fprintf(stderr, "PLUGIN_CANVAS_NATIVE ops=%zu status=%d\n", count, status);
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_find_widget(plugin_canvas_panel, "rgba-proof", &widget);
        sao_ui_offscreen_raster_handle_t raster{};
        const SaoUiOffscreenRasterDesc raster_spec{2, 2, 0};
        if (status == SAO_STATUS_OK)
            status = sao_ui_offscreen_raster_create(&raster_spec, &raster);
        if (status == SAO_STATUS_OK)
            status = sao_ui_script_canvas_rasterize(
                reinterpret_cast<sao_ui_script_canvas_handle_t>(widget), raster, 0, 0);
        std::array<uint8_t, 16> pixels{};
        size_t bytes{};
        uint32_t rgba_width{}, rgba_height{}, rgba_stride{};
        if (status == SAO_STATUS_OK)
            status = sao_ui_offscreen_raster_snapshot(raster, pixels.data(), pixels.size(),
                                                     &bytes, &rgba_width, &rgba_height, &rgba_stride);
        if (raster != nullptr)
            sao_ui_offscreen_raster_destroy(raster);
        constexpr std::array<uint8_t, 16> expected{0,0,255,255, 0,128,0,128,
                                                   255,0,0,255, 0,0,0,0};
        if (status == SAO_STATUS_OK && (bytes != expected.size() || pixels != expected ||
                          rgba_width != 2 || rgba_height != 2 || rgba_stride != 8))
            status = SAO_STATUS_ERR_INVALID_ARGUMENT;
        std::fprintf(stderr, "PLUGIN_RGBA_NATIVE bytes=%zu status=%d\n", bytes, status);
        if (status != SAO_STATUS_OK)
            return status;
        if (!plugin_draw_bound) {
            const auto bound = sao_sdk_bind_context("preview.plugin.draw", "1.0", &plugin_draw_context);
            if (bound != SAO_SDK_OK)
                return static_cast<sao_status_t>(bound);
            plugin_draw_bound = true;
            const auto services = sao_sdk_context_bind_platform_services(&plugin_draw_context);
            if (services != SAO_SDK_OK)
                return static_cast<sao_status_t>(services);
        }
        static constexpr char overlay[] = R"json({"nodes":[{"type":"canvas","width":190,"height":86,
            "x":560,"y":520,"z":1200,"bg":"transparent","ops":[
                {"op":"oval","x":8,"y":8,"w":48,"h":48,"fill":"#f4ba4c"},
                {"op":"text","x":66,"y":22,"text":"Overlay ctx","fill":"#18c8d0","size":18,"bold":true}
            ]}]})json";
        const auto shown = sao_sdk_ui_set_overlay(&plugin_draw_context, "preview.draw",
            reinterpret_cast<const uint8_t*>(overlay), sizeof(overlay) - 1);
        static constexpr char invalid[] = R"({"type":"canvas","ops":[{"op":"invalid"}]})";
        const auto rejected = shown == SAO_SDK_OK ? sao_sdk_ui_set_overlay(&plugin_draw_context,
            "preview.draw", reinterpret_cast<const uint8_t*>(invalid), sizeof(invalid) - 1) : shown;
        std::fprintf(stderr, "PLUGIN_OVERLAY_NATIVE show=%d invalid=%d\n", shown, rejected);
        if (shown != SAO_SDK_OK || rejected != SAO_SDK_ERR_INVALID_ARGUMENT)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return status;
    }

    static sao_status_t SAO_UI_CALL plugin_action(int32_t action_id, void* user_data) noexcept {
        auto* host = static_cast<Host*>(user_data);
        sao_status_t status = SAO_STATUS_ERR_INVALID_ARGUMENT;
        SaoUiPluginTabsSnapshot snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        if (host != nullptr) {
            try {
                status = host->closing ? SAO_STATUS_ERR_CANCELLED :
                    sao_ui_plugin_tabs_get_snapshot(host->plugin_tabs, &snapshot);
                if (status == SAO_STATUS_OK) {
                    if (action_id == 2000) {
                        const auto previous = host->plugin_catalog_revision;
                        ++host->plugin_catalog_revision;
                        status = host->publish_plugin_samples();
                        if (status != SAO_STATUS_OK)
                            host->plugin_catalog_revision = previous;
                    } else if (action_id == 2104) {
                        status = host->show_plugin_canvas();
                    } else if (action_id != 2001 && action_id != 2002 &&
                               (action_id < 2101 || action_id > 2104)) {
                        status = SAO_STATUS_ERR_INVALID_ARGUMENT;
                    }
                }
            } catch (...) {
                status = SAO_STATUS_ERR_UNKNOWN;
            }
            ++host->plugin_action_count;
            host->plugin_last_action = action_id;
            host->plugin_last_status = status;
        }
        std::fprintf(stderr, "PLUGIN_TABS_ACTION id=%d status=%d selected=%s revision=%u calls=%u runtime=0\n",
                     action_id, status, snapshot.selected_plugin_id_utf8,
                     host ? host->plugin_catalog_revision : 0, host ? host->plugin_action_count : 0);
        std::fflush(stderr);
        return status;
    }

    static sao_status_t SAO_UI_CALL entity_action(SaoUiEntityAction action, void* user_data) {
        auto* host = static_cast<Host*>(user_data);
        if (host == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        if (host->closing)
            return SAO_STATUS_ERR_CANCELLED;
        if (host->config.readability_probe && static_cast<int32_t>(action) >= 2800 &&
            static_cast<int32_t>(action) < 2900) {
            ++host->readability_action_count;
            host->readability_last_action = static_cast<int32_t>(action);
            return SAO_STATUS_OK;
        }
        switch (static_cast<int32_t>(action)) {
        case kOpenLauncherSettings:
            return host->open_launcher_settings();
        case kOpenHotkeys:
            return host->open_hotkeys();
        case kOpenAiMain:
            return host->open_ai_main();
        case kOpenAiSettings:
            return host->open_ai_settings();
        case kOpenPluginManager:
            return host->open_plugin_manager();
        case kOpenWorkshop:
            return host->open_workshop();
        case kOpenProcessSelector:
            return host->open_process_selector();
        case kOpenMemoryViewer:
            return host->open_memory_viewer();
        case kOpenLicense:
            return host->open_license();
        case kOpenUserMenu:
            return sao_ui_compositor_post_input(host->compositor, [](void* data) {
                auto* owner = static_cast<Host*>(data);
                if (!owner->closing)
                    (void)owner->open_user_menu();
            }, host);
        case kOpenAboutGuide:
            return host->open_about_guide();
        case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
            return sao_ui_theme_set_active_id(SAO_UI_THEME_LIGHT);
        case SAO_UI_ENTITY_ACTION_SET_ALL_DARK:
            return sao_ui_theme_set_active_id(SAO_UI_THEME_DARK);
        case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS:
            return host->settings_owner ? host->settings_owner->save()
                                        : SAO_STATUS_ERR_NOT_INITIALIZED;
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }

    sao_status_t open_launcher_settings() noexcept {
        return sao::launcher::toggle_menu_panel(sao::launcher::settings::kSettingsPanelId,
            [] { return sao::launcher::settings::open_config_panel_status(); },
            [] { return sao::launcher::settings::close_for_testing(); });
    }
    sao_status_t open_hotkeys() noexcept {
        return hotkey_owner ? sao::launcher::toggle_menu_panel(sao::launcher::hotkey::kPanelId,
            [&] { return hotkey_owner->open(); }, [&] { return hotkey_owner->close(); }) : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_ai_settings() noexcept {
        if (ai_settings) {
            bool visible = false;
            const auto status = sao_ai_editor_settings_panel_is_visible(ai_settings, &visible);
            if (status != SAO_STATUS_OK) return status;
            if (visible) return sao_ai_editor_settings_panel_hide(ai_settings);
        }
        if (ai_settings == nullptr) {
            const sao_status_t status =
                sao_ai_editor_settings_panel_create(compositor, backend, &ai_settings);
            if (status != SAO_STATUS_OK)
                return status;
        }
        sao_status_t status = sao_ai_editor_settings_panel_show(ai_settings);
        if (status == SAO_STATUS_OK)
            status = sao_ai_editor_settings_panel_tick(ai_settings);
        return status;
    }
    sao_status_t open_ai_main() noexcept {
        if (ai_main) {
            bool visible = false;
            const auto status = sao_ai_editor_main_panel_is_visible(ai_main, &visible);
            if (status != SAO_STATUS_OK) return status;
            if (visible) return sao_ai_editor_main_panel_hide(ai_main);
        }
        sao::launcher::hideUserGuideWebView();
        if (ai_main == nullptr) {
            const sao_status_t status =
                sao_ai_editor_main_panel_create(compositor, backend, &ai_main);
            if (status != SAO_STATUS_OK)
                return status;
        }
        sao_status_t status = sao_ai_editor_main_panel_show(ai_main);
        if (status == SAO_STATUS_OK)
            status = sao_ai_editor_main_panel_tick(ai_main);
        return status;
    }
    sao_status_t open_plugin_manager() noexcept {
        return plugin_manager ? sao::launcher::toggle_menu_panel(sao::launcher::plugin_manager_panel::kPanelId.data(),
            [&] { return plugin_manager->open(); }, [&] { return plugin_manager->close(); }) : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_workshop() noexcept {
        return workshop ? sao::launcher::toggle_menu_panel(sao::launcher::workshop_panel::kPanelId,
            [&] { return workshop->open(); }, [&] { return workshop->hide(); }) : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_process_selector() noexcept {
        return process_selector ? sao::launcher::toggle_menu_panel(sao::launcher::process_selector_panel::kPanelId,
            [&] { return process_selector->open(); }, [&] { return process_selector->close(); }) : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_memory_viewer() noexcept {
        return memory_viewer ? sao::launcher::toggle_menu_panel(sao::launcher::memory_viewer_panel::kPanelId,
            [&] { return memory_viewer->open(); }, [&] { return memory_viewer->close(); }) : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_license() noexcept {
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        return license ? sao::launcher::toggle_menu_panel(sao::launcher::license_panel::kPanelId,
            [&] { return license->open(); }, [&] { return license->close(); }) : SAO_STATUS_ERR_NOT_INITIALIZED;
#else
        return SAO_STATUS_ERR_NOT_INITIALIZED;
#endif
    }
    sao_status_t open_user_menu() noexcept {
        if (!user_menu_created)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        user_menu.processCommandLine(L"", true);
        return SAO_STATUS_OK;
    }
    sao_status_t open_about_guide() noexcept {
        if (sao::launcher::userGuideWebViewVisible()) {
            sao::launcher::hideUserGuideWebView();
            return SAO_STATUS_OK;
        }
        return sao::launcher::openUserDocsIndex(base_dir.c_str(), window)
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_NOT_FOUND;
    }

    int32_t drain_close() noexcept {
        closing = true;
        if (window != nullptr)
            KillTimer(window, kUiService);
        if (!shutdown_requested) {
            shutdown_requested = true;
            if (user_menu_created)
                user_menu.beginExit();
            if (process_selector)
                process_selector->request_shutdown();
            if (memory_viewer)
                memory_viewer->request_shutdown();
        }
        const ULONGLONG deadline = GetTickCount64() + 5000;
        int32_t status = SAO_STATUS_ERR_CANCELLED;
        do {
            MSG quit{};
            if (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && !quit_code)
                quit_code = static_cast<int>(quit.wParam);
            status = close();
            if (status == SAO_STATUS_OK) {
                if (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && !quit_code)
                    quit_code = static_cast<int>(quit.wParam);
                return status;
            }
            MSG message{};
            for (unsigned dispatched = 0; dispatched < 64 && GetTickCount64() < deadline &&
                  PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++dispatched) {
                if (message.message == WM_QUIT) {
                    if (!quit_code)
                        quit_code = static_cast<int>(message.wParam);
                    continue;
                }
                if (message.hwnd == window && message.message == WM_TIMER) continue;
                if ((message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST) ||
                    (message.message >= WM_MOUSEFIRST && message.message <= WM_MOUSELAST) ||
                    message.message == WM_HOTKEY ||
                    message.message == SAO_UI_NATIVE_TEXT_TAB_MESSAGE)
                    continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            (void)MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < deadline);
        return status;
    }

    int32_t close() noexcept {
        if (plugin_draw_bound) {
            const auto status = sao_sdk_context_try_destroy(&plugin_draw_context);
            if (status != SAO_SDK_OK)
                return status;
            plugin_draw_bound = false;
        }
        if (plugin_canvas_panel != nullptr) {
            sao_ui_panel_destroy(plugin_canvas_panel);
            SaoPanelState state{};
            const auto status = sao_ui_panel_get_state(plugin_canvas_panel, &state);
            if (status != SAO_STATUS_ERR_HANDLE_INVALID)
                return status == SAO_STATUS_OK ? SAO_STATUS_ERR_CANCELLED : status;
            plugin_canvas_panel = nullptr;
        }
        if (plugin_tabs != nullptr) {
            if (!plugin_tabs_hidden) {
                const auto status = sao_ui_plugin_tabs_set_visible(plugin_tabs, false);
                if (status != SAO_STATUS_OK) return status;
                plugin_tabs_hidden = true;
            }
            const auto status = sao_ui_plugin_tabs_try_destroy(plugin_tabs);
            if (status != SAO_STATUS_OK) return status;
            plugin_tabs = nullptr;
            std::fprintf(stderr, "PLUGIN_TABS_LIFETIME hidden=1 destroyed=1\n");
            std::fflush(stderr);
        }
        if (file_picker != nullptr) {
            const auto status = sao_ui_file_picker_try_destroy(file_picker);
            if (status != SAO_STATUS_OK) return status;
            file_picker = nullptr;
        }
        if (!sao::launcher::shutdownUserGuideWebView())
            return SAO_STATUS_ERR_CANCELLED;
        if (settings_owner != nullptr && settings_owner->dirty()) {
            const sao_status_t status = settings_owner->save();
            if (status != SAO_STATUS_OK)
                return status;
        }
        if (intro != nullptr) {
            sao_ui_linkstart_destroy(intro);
            intro = nullptr;
        }
        if (ai_main != nullptr) {
            const sao_status_t status = sao_ai_editor_main_panel_try_destroy(ai_main);
            if (status != SAO_STATUS_OK)
                return status;
            ai_main = nullptr;
        }
        if (ai_settings != nullptr) {
            const sao_status_t status = sao_ai_editor_settings_panel_try_destroy(ai_settings);
            if (status != SAO_STATUS_OK)
                return status;
            ai_settings = nullptr;
        }
        if (user_menu_created) {
            user_menu.unbind_hotkey_owner(hotkey_owner.get());
            user_menu.destroy();
            user_menu_created = false;
        }
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        if (license != nullptr) {
            const sao_status_t status = license->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            license.reset();
        }
#endif
        if (workshop != nullptr) {
            const sao_status_t status = workshop->try_take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            workshop.reset();
        }
        if (memory_viewer != nullptr) {
            const sao_status_t status = memory_viewer->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            memory_viewer.reset();
        }
        if (process_selector != nullptr) {
            const sao_status_t status = process_selector->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            process_selector.reset();
        }
        if (plugin_manager != nullptr) {
            const sao_status_t status = plugin_manager->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            plugin_manager.reset();
        }
        if (hotkey_owner != nullptr) {
            const sao_status_t status = hotkey_owner->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            hotkey_owner.reset();
        }
        const sao_status_t settings_panel_status =
            sao::launcher::settings::take_offline_for_testing();
        if (settings_panel_status != SAO_STATUS_OK)
            return settings_panel_status;
        (void)sao::launcher::hotkey::unregister_all();
        sao::launcher::hotkey::clear_callbacks();
        (void)sao_launcher_hotkey_set_settings_owner(nullptr);
        (void)sao::launcher::settings::settings_profiles_unbind_owner(nullptr);
        (void)sao::launcher::settings::settings_panel_unbind_owner(nullptr);
        if (settings_owner != nullptr) {
            const sao_status_t status = settings_owner->save();
            if (status != SAO_STATUS_OK)
                return status;
            settings_owner.reset();
        }
        if (entity != nullptr) {
            const sao_status_t status = sao_ui_entity_shell_try_destroy(entity);
            if (status != SAO_STATUS_OK)
                return status;
            entity = nullptr;
        }
        if (backdrop != nullptr) {
            const sao_status_t status = sao_ui_fisheye_backdrop_try_destroy(backdrop);
            if (status != SAO_STATUS_OK)
                return status;
            backdrop = nullptr;
        }
        if (keyboard != nullptr) {
            const sao_status_t status = sao_ui_input_router_deep_try_destroy(keyboard);
            if (status != SAO_STATUS_OK)
                return status;
            keyboard = nullptr;
        }
        if (sdk_bound) {
            if (sao_sdk_platform_unbind_ui_compositor() != SAO_SDK_OK)
                return SAO_STATUS_ERR_UNKNOWN;
            sdk_bound = false;
        }
        if (compositor != nullptr) {
            const sao_status_t status = sao_ui_compositor_try_destroy(compositor);
            if (status != SAO_STATUS_OK)
                return status;
            compositor = nullptr;
        }
        if (overlay != nullptr) {
            if (!sao_ui_overlay_host_destroy(overlay))
                return SAO_UI_STATUS_ERR_BUSY;
            overlay = nullptr;
        }
        if (backend != nullptr) {
            int32_t exit_code = 0;
            (void)sao_ai_editor_shutdown(backend, 2000, &exit_code);
            sao_ai_editor_destroy(backend);
            backend = nullptr;
        }
        return SAO_STATUS_OK;
    }

    void sync_backdrop(uint32_t elapsed) {
        if (!backdrop || !entity)
            return;
        bool intro_active = false;
        if (intro != nullptr)
            require(sao_ui_linkstart_is_active(intro, &intro_active));
        SaoUiEntityShellSnapshot snapshot{};
        require(sao_ui_entity_shell_get_snapshot(entity, &snapshot));
        if (!intro_active && snapshot.menu_visible && snapshot.overlay_visible) {
            RECT rect{};
            if (GetClientRect(window, &rect) && rect.right > 0 && rect.bottom > 0) {
                SaoUiFisheyeBackdropRect bounds{0, 0, rect.right, rect.bottom};
                require(sao_ui_fisheye_backdrop_show(backdrop, &bounds, -50));
            }
        } else {
            require(sao_ui_fisheye_backdrop_hide(backdrop));
        }
        require(sao_ui_fisheye_backdrop_advance(backdrop, elapsed));
    }

    void resize() {
        if (closing || overlay == nullptr)
            return;
        if (IsIconic(window)) {
            require(sao_ui_overlay_host_set_visible(overlay, false));
            return;
        }
        RECT rect{};
        POINT origin{};
        if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin) || rect.right < 1 ||
            rect.bottom < 1)
            return;
        require(
            sao_ui_overlay_host_set_bounds(overlay, origin.x, origin.y, rect.right, rect.bottom));
        if (intro != nullptr) {
            require(sao_ui_linkstart_resize(intro, static_cast<uint32_t>(rect.right),
                                            static_cast<uint32_t>(rect.bottom),
                                            sao_ui_overlay_host_current_dpi(overlay)));
        }
        require(sao_ui_overlay_host_set_visible(overlay, true));
        sync_backdrop(0);
    }

    void initialize_settings() {
        // A visual preview must not save over the production workspace's
        // settings on close. Seed its independent profile once from real data.
        if (config.settings.empty()) {
            config.settings = config.workspace / L".sao" / L"ui-preview" / L"settings.json";
            std::filesystem::create_directories(config.settings.parent_path());
            const auto source = config.workspace / L"settings.json";
            if (!std::filesystem::exists(config.settings) &&
                std::filesystem::is_regular_file(source))
                std::filesystem::copy_file(source, config.settings);
        }
        const auto settings_path = config.settings.wstring();
        const auto profiles_path = (config.settings.parent_path() / L"profiles").wstring();
        sao::launcher::settings::set_profiles_directory_for_testing(profiles_path);
        require(
            sao::launcher::settings_owner::SettingsOwner::create(settings_path, settings_owner));
        sao::launcher::settings_owner::LoadInfo load_info{};
        require(settings_owner->load(load_info));
        require(sao::launcher::settings::settings_panel_bind_owner(settings_owner.get()));
        require(sao::launcher::settings::settings_profiles_bind_owner(settings_owner.get()));
        require(sao_launcher_hotkey_set_settings_owner(settings_owner.get()));
    }

    void initialize_entity() {
        static constexpr SaoUiMenuItem control[] = {
            {"设置", "sao:settings", kOpenLauncherSettings, true, {false, false, false}},
            {"快捷键", "sao:keyboard", kOpenHotkeys, true, {false, false, false}},
            {"保存设置",
             "sao:settings",
             SAO_UI_ENTITY_ACTION_SAVE_SETTINGS,
             true,
             {false, false, false}},
        };
        static constexpr SaoUiMenuItem tools[] = {
            {"AI 编辑器", "sao:chat", kOpenAiMain, true, {false, false, false}},
            {"AI 设置", "sao:settings", kOpenAiSettings, true, {false, false, false}},
            {"创意工坊", "sao:workshop", kOpenWorkshop, true, {false, false, false}},
            {"进程选择", "sao:process", kOpenProcessSelector, true, {false, false, false}},
            {"内存查看器", "sao:process", kOpenMemoryViewer, true, {false, false, false}},
        };
        static constexpr SaoUiMenuItem plugins[] = {
            {"插件管理", "sao:plugins", kOpenPluginManager, true, {false, false, false}},
            {"许可", "sao:license", kOpenLicense, true, {false, false, false}},
        };
        static constexpr SaoUiMenuItem appearance[] = {
            {"经典浅色",
             "sao:sun",
             SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT,
             true,
             {false, false, false}},
            {"经典深色",
             "sao:moon",
             SAO_UI_ENTITY_ACTION_SET_ALL_DARK,
             true,
             {false, false, false}},
        };
        static constexpr SaoUiMenuItem user[] = {
            {"用户菜单", "sao:user", kOpenUserMenu, true, {false, false, false}},
            {"关于与使用指南", "sao:info", kOpenAboutGuide, true, {false, false, false}},
        };
        static constexpr SaoUiEntityRootItem roots[] = {
            {sizeof(SaoUiEntityRootItem),
             "Control",
             "设置",
             "sao:settings",
             10,
             true,
             {0, 0, 0},
             control,
             std::size(control)},
            {sizeof(SaoUiEntityRootItem),
             "Tools",
             "工具",
             "sao:tools",
             11,
             true,
             {0, 0, 0},
             tools,
             std::size(tools)},
            {sizeof(SaoUiEntityRootItem),
             "Plugins",
             "插件",
             "sao:plugins",
             12,
             true,
             {0, 0, 0},
             plugins,
             std::size(plugins)},
            {sizeof(SaoUiEntityRootItem),
             "Skins",
             "外观",
             "sao:home",
             13,
             true,
             {0, 0, 0},
             appearance,
             std::size(appearance)},
            {sizeof(SaoUiEntityRootItem),
             "User",
             "用户",
             "sao:user",
             14,
             true,
             {0, 0, 0},
             user,
             std::size(user)},
        };
        SaoUiEntityShellConfig entity_config{};
        entity_config.action_fn = &Host::entity_action;
        entity_config.action_user_data = this;
        require(sao_ui_entity_shell_create_on_compositor(compositor, &entity_config, &entity));
        bool nervgear_enabled = true;
        require(settings_owner->get_truthy("nervgear_mode", true, nervgear_enabled));
        require(sao_ui_entity_shell_set_nervgear_mode(entity, nervgear_enabled));
        require(sao_ui_entity_shell_set_roots(entity, roots, std::size(roots)));
    }

    void show_linkstart() {
        RECT rect{};
        GetClientRect(window, &rect);
        SaoUiLinkStartConfig start{sizeof(SaoUiLinkStartConfig),
                                   static_cast<uint32_t>(rect.right),
                                   static_cast<uint32_t>(rect.bottom), 0, nullptr};
        require(sao_ui_linkstart_create(compositor, nullptr, &start, &intro));
        require(sao_ui_linkstart_resize(intro, static_cast<uint32_t>(rect.right),
                                        static_cast<uint32_t>(rect.bottom),
                                        sao_ui_overlay_host_current_dpi(overlay)));
        require(config.outro ? sao_ui_linkstart_show_outro(intro)
                             : sao_ui_linkstart_show(intro));
        if (config.intro_handoff) {
            require(sao_ui_linkstart_arm_bootstrap_hold(intro));
            SaoUiLinkStartBootstrap ready{};
            ready.struct_size = sizeof(ready);
            ready.stage_index = 0;
            ready.stage_count = 6;
            ready.stage_progress = 0.5F;
            ready.caption_utf8 = "LOADING";
            require(sao_ui_linkstart_set_bootstrap(intro, &ready));
            require(sao_ui_linkstart_tick(intro, 12000));
            SaoUiLinkStartPhase held_phase{};
            float held_progress = 0.0F;
            require(sao_ui_linkstart_get_phase(intro, &held_phase, &held_progress));
            for (uint32_t stage = 0; stage <= ready.stage_count; ++stage) {
                ready.stage_index = std::min(stage, ready.stage_count - 1u);
                ready.stage_progress = stage == ready.stage_count - 1u ? 0.95F : 1.0F;
                ready.caption_utf8 = stage == ready.stage_count ? "READY" : "LOADING";
                require(sao_ui_linkstart_set_bootstrap(intro, &ready));
                require(sao_ui_linkstart_tick(intro, 3000));
                bool waiting = false;
                SaoUiLinkStartPhase waiting_phase{};
                float waiting_progress = 0.0F;
                SaoUiLinkStartCompletionReason waiting_reason{};
                SaoUiEntityShellSnapshot shell{};
                require(sao_ui_linkstart_is_active(intro, &waiting));
                require(sao_ui_linkstart_get_phase(intro, &waiting_phase, &waiting_progress));
                require(sao_ui_linkstart_poll_completion(intro, &waiting_reason));
                require(sao_ui_entity_shell_get_snapshot(entity, &shell));
                if (!waiting || waiting_phase != held_phase || waiting_progress != held_progress ||
                    waiting_reason != SAO_UI_LINKSTART_COMPLETION_NONE ||
                    shell.overlay_visible || shell.menu_visible)
                    throw std::runtime_error("Loading stage started the menu handoff before release");
                std::fprintf(stderr, "INTRO_LOADING stage=%u/6 fill=%.2f held=1 menu=0\n",
                             ready.stage_index + 1u, ready.stage_progress);
            }
            require(sao_ui_linkstart_release_bootstrap_hold(intro, 0));
            require(sao_ui_linkstart_release_bootstrap_hold(intro, 0));
            require(sao_ui_linkstart_tick(intro, 3000));
            bool active = false;
            SaoUiLinkStartPhase resumed_phase{};
            float resumed_progress = 0.0F;
            SaoUiLinkStartCompletionReason reason{};
            require(sao_ui_linkstart_is_active(intro, &active));
            require(sao_ui_linkstart_get_phase(intro, &resumed_phase, &resumed_progress));
            require(sao_ui_linkstart_poll_completion(intro, &reason));
            if (!active || held_phase != SAO_UI_LINKSTART_PHASE_CONNECTED ||
                resumed_phase != held_phase || resumed_progress != held_progress ||
                reason != SAO_UI_LINKSTART_COMPLETION_NONE)
                throw std::runtime_error("Bootstrap release skipped the SYSTEM transition");
            std::fprintf(stderr, "INTRO_HANDOFF stages_held=6 ready_held=1 delayed_tick_ms=3000 active=1 phase=%d progress=%.6f\n",
                         static_cast<int>(resumed_phase), resumed_progress);
        }
        intro_started = GetTickCount64();
        report_outro(0);
    }

    void report_outro(uint32_t elapsed) {
        if (!config.outro || intro == nullptr || outro_completed)
            return;
        outro_elapsed += elapsed;
        SaoUiLinkStartPhase phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
        float progress = 0.0F;
        require(sao_ui_linkstart_get_phase(intro, &phase, &progress));
        if (outro_phase != phase) {
            std::fprintf(stderr, "outro phase=%d elapsed_ms=%llu wall_ms=%llu\n",
                         static_cast<int>(phase), outro_elapsed, GetTickCount64() - intro_started);
            outro_phase = phase;
        }
        bool active = false;
        require(sao_ui_linkstart_is_active(intro, &active));
        if (!active) {
            SaoUiLinkStartCompletionReason reason = SAO_UI_LINKSTART_COMPLETION_NONE;
            require(sao_ui_linkstart_poll_completion(intro, &reason));
            outro_completed = true;
            outro_failed = outro_failed || reason != SAO_UI_LINKSTART_COMPLETION_OUTRO;
            std::fprintf(stderr, "outro completion=%d elapsed_ms=%llu wall_ms=%llu failed=%d\n",
                         static_cast<int>(reason), outro_elapsed,
                         GetTickCount64() - intro_started, outro_failed ? 1 : 0);
            if (config.frame_out.empty())
                closing = true;
        }
        std::fflush(stderr);
    }

    void initialize() {
        if (config.outro)
            require(sao_ui_sound_set_enabled(false));
        if (config.backend_explicit && !config.offline) {
            std::filesystem::path backend_workspace = config.workspace;
            if (config.local_backend_fixture) {
                config.backend = sibling_executable(L"SaoAiEditor.exe");
                backend_workspace = config.workspace / L".sao" / L"ui-preview" / L"ai-backend";
                std::error_code directory_error;
                std::filesystem::create_directories(backend_workspace, directory_error);
                if (directory_error || !std::filesystem::is_directory(backend_workspace))
                    throw std::runtime_error("Preview backend workspace unavailable");
            }
            if (!std::filesystem::is_regular_file(config.backend))
                throw std::runtime_error("Preview backend executable unavailable");
            const std::string executable = utf8(config.backend);
            const std::string workspace = utf8(backend_workspace);
            const std::string extra = "--headless --workspace \"" + workspace + "\"";
            SaoAiEditorLaunchConfig launch{};
            launch.executable_utf8 = executable.c_str();
            launch.base_dir_utf8 = workspace.c_str();
            launch.extra_args_utf8 = extra.c_str();
            launch.handshake_timeout_ms = 30000;
            require(sao_ai_editor_create(&launch, &backend));
            int32_t exit_code = 0;
            require(sao_ai_editor_launch(backend, &exit_code));
        }
        SaoOverlayHostConfig host{};
        host.width = 1280;
        host.height = 820;
        host.title_utf16 = L"SAO Classic — production UI shell";
        require(sao_ui_overlay_host_create(&host, &overlay));
        require(sao_ui_compositor_create(overlay, nullptr, &compositor));
        if (config.outro) {
            resize();
            show_linkstart();
            last_tick = GetTickCount64();
            if (!SetTimer(window, kUiService, 16, nullptr))
                throw std::runtime_error("UI timer unavailable");
            return;
        }
        require(sao_ui_fisheye_backdrop_create(compositor, &backdrop));
        require_sdk(sao_sdk_platform_bind_ui_compositor(compositor));
        sdk_bound = true;
        initialize_settings();
        hotkey_owner = std::make_unique<sao::launcher::hotkey::Owner>(compositor);
        require(hotkey_owner->set_owner_wake_window(window));
        plugin_manager = std::make_unique<sao::launcher::plugin_manager_panel::Owner>(compositor);
        process_selector =
            std::make_unique<sao::launcher::process_selector_panel::Owner>(compositor, nullptr);
        memory_viewer = std::make_unique<sao::launcher::memory_viewer_panel::Owner>(compositor);
        workshop = std::make_unique<sao::launcher::workshop_panel::Owner>(
            compositor, config.workspace, detached_workshop_operations());
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        license = std::make_unique<sao::launcher::license_panel::Owner>(compositor);
#endif
        if (!user_menu.create(base_dir.c_str()))
            throw std::runtime_error("Production user menu unavailable");
        user_menu.bind_hotkey_owner(hotkey_owner.get());
        user_menu_created = true;
        initialize_entity();
        require(sao_ui_entity_shell_bring_online(entity));
        sao::launcher::hotkey::clear_callbacks();
        sao::launcher::hotkey::set_callback("toggle_sao_menu", [this] {
            SaoUiEntityShellSnapshot shell{};
            sao_status_t status = sao_ui_entity_shell_get_snapshot(entity, &shell);
            const bool was_hidden = status == SAO_STATUS_OK && !shell.overlay_visible;
            if (was_hidden)
                status = sao_ui_entity_shell_insert(entity);
            if (status == SAO_STATUS_OK)
                status = sao_ui_entity_shell_home(entity);
            if (status == SAO_STATUS_OK && was_hidden) {
                SaoUiEntityShellSnapshot restored{};
                status = sao_ui_entity_shell_get_snapshot(entity, &restored);
                if (status == SAO_STATUS_OK && !restored.menu_visible)
                    status = sao_ui_entity_shell_home(entity);
            }
            if (status == SAO_STATUS_OK)
                (void)sao_ui_compositor_tick(compositor);
        });
        sao::launcher::hotkey::load_or_default({
            {"toggle_sao_menu", "Home", VK_HOME, MOD_NOREPEAT},
        });
        if (config.frame_out.empty() && !config.intro_audition &&
            !sao::launcher::hotkey::register_all().empty())
            throw std::runtime_error("Production hotkey registration unavailable");
        require(sao_ui_input_router_deep_create(compositor, &keyboard));
        bool intro_sound_enabled = false;
        require(sao_ui_sound_get_enabled(&intro_sound_enabled));
        if (!config.frame_out.empty() || config.intro)
            require(sao_ui_sound_set_enabled(false));
        if (config.initial_surface == InitialSurface::ai_main)
            require(open_ai_main());
        if (config.initial_surface == InitialSurface::ai_settings)
            require(open_ai_settings());
        if (config.initial_surface == InitialSurface::settings)
            require(open_launcher_settings());
        if (config.initial_surface == InitialSurface::hotkeys)
            require(open_hotkeys());
        if (config.initial_surface == InitialSurface::plugins)
            require(open_plugin_manager());
        if (config.initial_surface == InitialSurface::workshop)
            require(open_workshop());
        if (config.initial_surface == InitialSurface::process)
            require(open_process_selector());
        if (config.initial_surface == InitialSurface::license)
            require(open_license());
        if (config.initial_surface == InitialSurface::user && !config.intro)
            require(open_user_menu());
        if (config.initial_surface == InitialSurface::files) {
            require(sao_ui_file_picker_create(compositor, &file_picker));
            const auto location = utf8(config.workspace);
            const SaoUiFilePickerConfig picker{"选择文件", location.c_str(), "全部文件", SAO_UI_FILE_PICKER_OPEN};
            require(sao_ui_file_picker_show(file_picker, &picker,
                [](sao_status_t, const char*, void*) {}, nullptr));
        }
        if (config.initial_surface == InitialSurface::about)
            require(open_about_guide());
        if (config.initial_surface == InitialSurface::root && !config.intro)
            require(sao_ui_entity_shell_home(entity));
        if (config.intro) {
            startup_menu_pending = true;
            require(sao_ui_entity_shell_insert(entity));
        }
        resize();
        if (config.plugin_tabs || config.readability_probe) {
            require(sao_ui_plugin_tabs_create(compositor, &Host::plugin_action, this, &plugin_tabs));
            require(publish_plugin_samples());
            require(sao_ui_plugin_tabs_set_visible(plugin_tabs, true));
            require(sao_ui_plugin_tabs_tick(plugin_tabs, 0));
            const auto snapshot = plugin_snapshot();
            verify_plugin_tabs(snapshot.visible && snapshot.tab_count == kPreviewPluginCount &&
                               snapshot.width > 0 && snapshot.height > 0, "created-visible");
            std::fprintf(stderr, "PLUGIN_TABS_LIFETIME created=1 visible=1 offline=1 runtime=0\n");
            std::fflush(stderr);
        }
        if (config.intro) {
            if (!config.frame_out.empty())
                require(sao_ui_sound_set_enabled(false));
            else if (config.intro_audition) {
                require(sao_ui_sound_set_enabled(true));
                require(sao_ui_sound_set_volume(55));
            } else
                require(sao_ui_sound_set_enabled(intro_sound_enabled));
            show_linkstart();
        }
        last_tick = GetTickCount64();
        if (!SetTimer(window, kUiService, 16, nullptr))
            throw std::runtime_error("UI timer unavailable");
    }

    void complete_startup_menu() {
        if (config.outro || !startup_menu_pending || intro == nullptr)
            return;
        bool active = false;
        require(sao_ui_linkstart_is_active(intro, &active));
        if (config.intro_handoff && active) {
            SaoUiEntityShellSnapshot shell{};
            require(sao_ui_entity_shell_get_snapshot(entity, &shell));
            if (shell.overlay_visible || shell.menu_visible)
                throw std::runtime_error("Startup menu became visible before SYSTEM exit");
        }
        if (active)
            return;
        SaoUiLinkStartCompletionReason reason = SAO_UI_LINKSTART_COMPLETION_NONE;
        require(sao_ui_linkstart_poll_completion(intro, &reason));
        startup_menu_pending = false;
        if (reason != SAO_UI_LINKSTART_COMPLETION_NATURAL &&
            reason != SAO_UI_LINKSTART_COMPLETION_SKIPPED)
            return;
        require(sao_ui_entity_shell_insert(entity));
        if (!config.intro_audition) {
            if (config.initial_surface == InitialSurface::user)
                require(open_user_menu());
            else if (config.initial_surface == InitialSurface::root)
                require(sao_ui_entity_shell_home(entity));
        }
    if (config.intro_handoff)
        std::fprintf(stderr, "INTRO_HANDOFF menu_opened=1 reason=%d\n", static_cast<int>(reason));
#ifndef NDEBUG
    std::fprintf(stderr, "STARTUP_MENU_AFTER_INTRO reason=%d\n", static_cast<int>(reason));
#endif
    }

    int32_t menu_motion_ms{};
    size_t menu_motion_event{};
    int32_t plugin_tabs_ms{};
    size_t plugin_tabs_event{};
    SaoUiPluginTabsSnapshot plugin_drag_start{};
    SaoUiPluginTabsSnapshot plugin_drag_moved{};
    POINT plugin_drag_cursor{};
    static constexpr std::array<int32_t, 17> plugin_event_times{
        600, 1200, 1800, 2200, 2400, 2600, 2800, 3200, 3600,
        4000, 4400, 4800, 5000, 5200, 5400, 5800, 6200};
    static constexpr int32_t plugin_first_row_center = 56;
    static constexpr int32_t plugin_child_row_stride = 47;

    SaoUiPluginTabsSnapshot plugin_snapshot() {
        SaoUiPluginTabsSnapshot snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        require(sao_ui_plugin_tabs_get_snapshot(plugin_tabs, &snapshot));
        return snapshot;
    }

    void verify_plugin_tabs(bool passed, const char* check) {
        const auto snapshot = plugin_snapshot();
        std::fprintf(stderr,
                     "PLUGIN_TABS_VERIFY check=%s pass=%d time=%d x=%d y=%d w=%d h=%d tabs=%zu first=%zu visible=%d dragging=%d selected=%s calls=%u action=%d status=%d\n",
                     check, passed ? 1 : 0, plugin_tabs_ms, snapshot.x, snapshot.y,
                     snapshot.width, snapshot.height, snapshot.tab_count, snapshot.first_visible,
                     snapshot.visible ? 1 : 0, snapshot.dragging ? 1 : 0,
                     snapshot.selected_plugin_id_utf8, plugin_action_count,
                     plugin_last_action, plugin_last_status);
        std::fflush(stderr);
        if (!passed)
            throw std::runtime_error(std::string("Plugin tabs verification failed: ") + check);
    }

    void plugin_pointer(uint32_t message, int32_t x, int32_t y, int32_t wheel = 0) {
        const HWND render = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        POINT point{x, y};
        if (render == nullptr || !ClientToScreen(render, &point))
            throw std::runtime_error("Plugin tabs host origin unavailable");
        require(sao_ui_compositor_dispatch_mouse(compositor, message, point.x, point.y,
            message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ? 0 : -1, wheel));
    }

    void plugin_click(int32_t x, int32_t y) {
        plugin_pointer(WM_MOUSEMOVE, x, y);
        plugin_pointer(WM_LBUTTONDOWN, x, y);
        plugin_pointer(WM_LBUTTONUP, x, y);
    }

    void plugin_first_tab_action_click(int32_t row) {
        const auto snapshot = plugin_snapshot();
        SaoOverlayHostClientRect viewport{};
        require(sao_ui_overlay_host_get_client_rect(overlay, &viewport));
        const int32_t gap = viewport.width >= 48 ? 12 : 0;
        const int32_t width = std::min(240, std::max(1, viewport.width - snapshot.width - gap));
        const int32_t count = (plugin_catalog_revision & 1U) != 0 ? 5 : 3;
        const int32_t height = std::min(viewport.height, 34 + (count - 1) * plugin_child_row_stride + 44);
        const int32_t x = static_cast<int64_t>(snapshot.x) + snapshot.width + gap + width <= viewport.width
            ? snapshot.x + snapshot.width + gap : std::max(0, snapshot.x - gap - width);
        const int32_t y = std::clamp(snapshot.y, 0, viewport.height - height);
        plugin_click(x + width / 2, y + plugin_first_row_center + row * plugin_child_row_stride);
    }

    void plugin_drag_begin() {
        plugin_drag_start = plugin_snapshot();
        plugin_drag_cursor = {plugin_drag_start.x + plugin_drag_start.width / 2,
                              plugin_drag_start.y + 12};
        plugin_pointer(WM_MOUSEMOVE, plugin_drag_cursor.x, plugin_drag_cursor.y);
        plugin_pointer(WM_LBUTTONDOWN, plugin_drag_cursor.x, plugin_drag_cursor.y);
        verify_plugin_tabs(plugin_snapshot().dragging, "header-capture");
    }

    void plugin_drag_move(int32_t dx, int32_t dy) {
        plugin_drag_cursor.x += dx;
        plugin_drag_cursor.y += dy;
        plugin_pointer(WM_MOUSEMOVE, plugin_drag_cursor.x, plugin_drag_cursor.y);
        plugin_drag_moved = plugin_snapshot();
        verify_plugin_tabs(plugin_drag_moved.dragging &&
                           plugin_drag_moved.x == plugin_drag_start.x + dx &&
                           plugin_drag_moved.y == plugin_drag_start.y + dy,
                           "drag-delta");
    }

    void verify_plugin_stationary() {
        for (int repeat = 0; repeat < 32; ++repeat) {
            plugin_pointer(WM_MOUSEMOVE, plugin_drag_cursor.x, plugin_drag_cursor.y);
            require(sao_ui_plugin_tabs_tick(plugin_tabs, 0));
            const auto snapshot = plugin_snapshot();
            if (!snapshot.dragging || snapshot.x != plugin_drag_moved.x ||
                snapshot.y != plugin_drag_moved.y || snapshot.width != plugin_drag_moved.width ||
                snapshot.height != plugin_drag_moved.height)
                verify_plugin_tabs(false, "stationary-no-drift");
        }
        verify_plugin_tabs(true, "stationary-no-drift");
    }

    void plugin_drag_end() {
        plugin_pointer(WM_LBUTTONUP, plugin_drag_cursor.x, plugin_drag_cursor.y);
        const auto snapshot = plugin_snapshot();
        verify_plugin_tabs(!snapshot.dragging && snapshot.x == plugin_drag_moved.x &&
                           snapshot.y == plugin_drag_moved.y, "drag-release");
    }

    void plugin_resize_clamp() {
        RECT client{};
        POINT origin{};
        const HWND render = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        if (!GetClientRect(render, &client) || !ClientToScreen(render, &origin))
            throw std::runtime_error("Plugin tabs resize bounds unavailable");
        const auto before = plugin_snapshot();
        const int32_t width = client.right - 160;
        const int32_t height = client.bottom - 100;
        verify_plugin_tabs(width > before.width && height > before.height &&
                           (before.x + before.width > width || before.y + before.height > height),
                           "resize-precondition");
        require(sao_ui_overlay_host_set_bounds(overlay, origin.x, origin.y, width, height));
        try {
            require(sao_ui_plugin_tabs_tick(plugin_tabs, 0));
            const auto after = plugin_snapshot();
            verify_plugin_tabs(after.visible && after.x >= 0 && after.y >= 0 &&
                               after.x + after.width <= width && after.y + after.height <= height &&
                               (after.x != before.x || after.y != before.y), "resize-clamp");
        } catch (...) {
            (void)sao_ui_overlay_host_set_bounds(overlay, origin.x, origin.y, client.right, client.bottom);
            throw;
        }
        require(sao_ui_overlay_host_set_bounds(overlay, origin.x, origin.y, client.right, client.bottom));
        require(sao_ui_plugin_tabs_tick(plugin_tabs, 0));
        std::fprintf(stderr, "PLUGIN_TABS_RESIZE temporary=%dx%d restored=%ldx%ld\n",
                     width, height, client.right, client.bottom);
    }

    void advance_plugin_tabs(int32_t elapsed_ms) {
        if (!config.plugin_tabs || config.frame_out.empty())
            return;
        const int32_t target = plugin_tabs_ms + elapsed_ms;
        while (plugin_tabs_event < plugin_event_times.size() &&
               plugin_event_times[plugin_tabs_event] <= target) {
            const int32_t event_time = plugin_event_times[plugin_tabs_event];
            require(sao_ui_plugin_tabs_tick(plugin_tabs, static_cast<uint32_t>(event_time - plugin_tabs_ms)));
            plugin_tabs_ms = event_time;
            auto snapshot = plugin_snapshot();
            switch (plugin_tabs_event) {
            case 0:
                verify_plugin_tabs(snapshot.visible && snapshot.tab_count == kPreviewPluginCount &&
                                   snapshot.first_visible == 0, "initial-overflow-catalog");
                plugin_click(snapshot.x + snapshot.width / 2, snapshot.y + plugin_first_row_center);
                snapshot = plugin_snapshot();
                verify_plugin_tabs(std::string_view(snapshot.selected_plugin_id_utf8) ==
                                   "preview.razorworks", "child-open");
                break;
            case 1:
                plugin_first_tab_action_click(0);
                snapshot = plugin_snapshot();
                verify_plugin_tabs(plugin_action_count == 1 && plugin_last_action == 2000 &&
                                   plugin_last_status == SAO_STATUS_OK && plugin_catalog_revision == 1 &&
                                   std::string_view(snapshot.selected_plugin_id_utf8) == "preview.razorworks",
                                   "refresh-keep-open");
                break;
            case 2:
                plugin_first_tab_action_click(4);
                snapshot = plugin_snapshot();
                verify_plugin_tabs(plugin_action_count == 2 && plugin_last_action == 2104 &&
                                   plugin_last_status == SAO_STATUS_OK &&
                                   std::string_view(snapshot.selected_plugin_id_utf8) == "preview.razorworks",
                                   "refreshed-fifth-row-dispatch");
                break;
            case 3: plugin_drag_begin(); break;
            case 4: plugin_drag_move(96, 48); break;
            case 5: verify_plugin_stationary(); break;
            case 6: plugin_drag_end(); break;
            case 7: {
                SaoUiEntityShellSnapshot main{};
                require(sao_ui_entity_shell_get_snapshot(entity, &main));
                verify_plugin_tabs(main.menu_visible, "main-open-before-close");
                require(sao_ui_entity_shell_handle_key(entity, VK_ESCAPE, 0));
                break;
            }
            case 8: {
                SaoUiEntityShellSnapshot main{};
                require(sao_ui_entity_shell_get_snapshot(entity, &main));
                verify_plugin_tabs(!main.menu_visible && snapshot.visible &&
                                   std::string_view(snapshot.selected_plugin_id_utf8) == "preview.razorworks",
                                   "main-closed-tabs-independent");
                break;
            }
            case 9:
                verify_plugin_tabs(std::string_view(snapshot.selected_plugin_id_utf8) ==
                                   "preview.razorworks", "remove-while-child-open");
                plugin_sample_removed = true;
                require(publish_plugin_samples());
                break;
            case 10:
                verify_plugin_tabs(snapshot.visible && snapshot.tab_count == kPreviewPluginCount - 1 &&
                                   snapshot.selected_plugin_id_utf8[0] == '\0' &&
                                   plugin_action_count == 2, "selected-removed-child-cleared");
                break;
            case 11:
                plugin_pointer(WM_MOUSEMOVE, snapshot.x + snapshot.width / 2, snapshot.y + plugin_first_row_center);
                plugin_pointer(WM_MOUSEWHEEL, snapshot.x + snapshot.width / 2,
                               snapshot.y + plugin_first_row_center, -WHEEL_DELTA);
                break;
            case 12:
                verify_plugin_tabs(snapshot.first_visible > 0 && snapshot.first_visible < snapshot.tab_count,
                                   "overflow-scroll");
                break;
            case 13:
                plugin_pointer(WM_MOUSEWHEEL, snapshot.x + snapshot.width / 2,
                               snapshot.y + plugin_first_row_center, WHEEL_DELTA * 12);
                verify_plugin_tabs(plugin_snapshot().first_visible == 0, "overflow-return");
                break;
            case 14: {
                RECT client{};
                if (!GetClientRect(static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor)), &client))
                    throw std::runtime_error("Plugin tabs drag bounds unavailable");
                plugin_drag_begin();
                plugin_drag_move(client.right - snapshot.width - 12 - snapshot.x,
                                 client.bottom - snapshot.height - 12 - snapshot.y);
                verify_plugin_stationary();
                plugin_drag_end();
                break;
            }
            case 15: plugin_resize_clamp(); break;
            case 16: {
                SaoUiEntityShellSnapshot main{};
                require(sao_ui_entity_shell_get_snapshot(entity, &main));
                verify_plugin_tabs(snapshot.visible && !snapshot.dragging && snapshot.tab_count == kPreviewPluginCount - 1 &&
                                   snapshot.selected_plugin_id_utf8[0] == '\0' &&
                                   !main.menu_visible && plugin_action_count == 2 &&
                                   plugin_last_status == SAO_STATUS_OK, "timeline-complete");
                break;
            }
            default: break;
            }
            std::fprintf(stderr, "PLUGIN_TABS_EVENT event=%zu time=%d\n", plugin_tabs_event, plugin_tabs_ms);
            std::fflush(stderr);
            ++plugin_tabs_event;
        }
        require(sao_ui_plugin_tabs_tick(plugin_tabs, static_cast<uint32_t>(target - plugin_tabs_ms)));
        plugin_tabs_ms = target;
    }

    void advance_menu_motion(int32_t elapsed_ms) {
        if (!config.menu_motion)
            return;
        menu_motion_ms += elapsed_ms;
        static constexpr std::array<int32_t, 18> times{
            140, 220, 800, 900, 940, 1160, 1270, 1800, 2250, 2500, 3650, 4300, 5100, 5700, 6400,
            7000, 7600, 8400};
        const auto pointer = [this](uint32_t message, int32_t x, int32_t y) {
            SaoUiEntityShellSnapshot snapshot{};
            require(sao_ui_entity_shell_get_snapshot(entity, &snapshot));
            require(sao_ui_compositor_dispatch_mouse(compositor, message,
                snapshot.origin_x + snapshot.menu_x + x,
                snapshot.origin_y + snapshot.menu_y + y,
                message == WM_MOUSEMOVE ? -1 : 0, 0));
        };
        const auto root = [&pointer](int32_t slot) {
            pointer(WM_MOUSEMOVE, 75, 75 + slot * 70);
            pointer(WM_LBUTTONDOWN, 75, 75 + slot * 70);
            pointer(WM_LBUTTONUP, 75, 75 + slot * 70);
        };
        while (menu_motion_event < times.size() && menu_motion_ms >= times[menu_motion_event]) {
            switch (menu_motion_event) {
            case 0: case 1: case 15: case 16: case 17:
                require(sao_ui_entity_shell_home(entity)); break;
            case 2: pointer(WM_MOUSEMOVE, 75, 75); break;
            case 3: pointer(WM_LBUTTONDOWN, 75, 75); break;
            case 4: pointer(WM_LBUTTONUP, 75, 75); break;
            case 5: root(1); break;
            case 6: root(3); break;
            case 7: case 9: require(sao_ui_theme_set_active_id(SAO_UI_THEME_DARK)); break;
            case 8: case 13: require(sao_ui_theme_set_active_id(SAO_UI_THEME_LIGHT)); break;
            case 10: root(0); break;
            case 11:
                pointer(WM_MOUSEMOVE, 210, 75);
                pointer(WM_LBUTTONDOWN, 210, 75);
                pointer(WM_LBUTTONUP, 210, 75);
                break;
            case 12: {
                sao_ui_panel_handle_t panel{};
                require(sao_ui_panel_find_by_id(compositor, sao::launcher::settings::kSettingsPanelId, &panel));
                SaoPanelState state{};
                require(sao_ui_panel_get_state(panel, &state));
                if (!state.visible)
                    throw std::runtime_error("Menu motion Settings panel did not open");
                SaoUiEntityShellSnapshot snapshot{};
                require(sao_ui_entity_shell_get_snapshot(entity, &snapshot));
                const int32_t x = snapshot.origin_x + state.x + state.width - 16;
                const int32_t y = snapshot.origin_y + state.y + 16;
                require(sao_ui_compositor_dispatch_mouse(compositor, WM_MOUSEMOVE, x, y, -1, 0));
                require(sao_ui_compositor_dispatch_mouse(compositor, WM_LBUTTONDOWN, x, y, 0, 0));
                require(sao_ui_compositor_dispatch_mouse(compositor, WM_LBUTTONUP, x, y, 0, 0));
                require(sao_ui_panel_get_state(panel, &state));
                if (state.visible)
                    throw std::runtime_error("Menu motion Settings panel did not close");
                break;
            }
            case 14: {
                require(sao_ui_entity_shell_handle_key(entity, VK_ESCAPE, 0));
                SaoUiEntityShellSnapshot snapshot{};
                require(sao_ui_entity_shell_get_snapshot(entity, &snapshot));
                if (snapshot.menu_visible)
                    require(sao_ui_entity_shell_handle_key(entity, VK_ESCAPE, 0));
                break;
            }
            default: break;
            }
            std::fprintf(stderr, "MENU_MOTION event=%zu time=%d\n", menu_motion_event, menu_motion_ms);
            ++menu_motion_event;
        }
    }

    void verify_readability(bool passed, const char* check) {
        ++readability_checks;
        std::fprintf(stderr, "READABILITY_VERIFY check=%s pass=%d\n", check, passed ? 1 : 0);
        if (!passed)
            throw std::runtime_error(std::string("Readability verification failed: ") + check);
    }

    void probe_label_layout() {
        struct Resources {
            sao_ui_widget_handle_t label{};
            sao_ui_paint_ctx_handle_t paint{};
            sao_ui_offscreen_raster_handle_t raster{};
            ~Resources() {
                if (label) sao_ui_widget_destroy(label);
                if (paint) sao_ui_paint_ctx_destroy(paint);
                if (raster) sao_ui_offscreen_raster_destroy(raster);
            }
        } resources;
        const SaoUiOffscreenRasterDesc raster_spec{720, 960, 0xfff6f7f9};
        require(sao_ui_offscreen_raster_create(&raster_spec, &resources.raster));
        require(sao_ui_paint_ctx_create_offscreen(resources.raster, &resources.paint));
        SaoUiLabelSpec spec{};
        spec.text_utf8 = "SAO Interface 0123456789";
        spec.font_slot = SAO_UI_FONT_SAO;
        spec.font_size_px = 16;
        spec.font_weight = SAO_UI_WEIGHT_NORMAL;
        spec.fg_argb = 0xff293440;
        spec.align = SAO_UI_ALIGN_LEFT;
        spec.anchor = SAO_UI_ANCHOR_NW;
        require(sao_ui_label_create(nullptr, &spec, &resources.label));
        require(sao_ui_paint_ctx_begin_frame(resources.paint));
        require(sao_ui_paint_ctx_draw_utf8(resources.paint, 14, 12,
            "LABEL / TRUE FONT METRICS", 22, 0xff293440));
        int32_t y = 58;
        const auto sample = [&](const char* name, const char* text, int32_t size,
                                 int32_t available, bool wrap, int32_t max_lines,
                                 float spacing, int32_t weight = SAO_UI_WEIGHT_NORMAL) {
            spec.text_utf8 = text;
            spec.font_size_px = size;
            spec.wrap = wrap;
            spec.max_lines = max_lines;
            spec.letter_spacing_px = spacing;
            spec.font_weight = weight;
            require(sao_ui_label_update(resources.label, &spec));
            int32_t width{}, height{}, repeat_width{}, repeat_height{};
            require(sao_ui_label_measure(resources.label, available, &width, &height));
            require(sao_ui_label_measure(resources.label, available, &repeat_width, &repeat_height));
            verify_readability(width == repeat_width && height == repeat_height, "label-repeat-measure");
            verify_readability(width >= 0 && height > 0 && (!available || width <= available),
                               "label-measured-bounds");
            verify_readability(y + height + 8 < 960, "label-sample-surface-bounds");
            const int32_t paint_width = std::min(580, available > 0 ? available : width);
            require(sao_ui_paint_ctx_fill_rect(resources.paint, 12, static_cast<float>(y),
                static_cast<float>(paint_width + 4), static_cast<float>(height + 4), 0xffe7ecf1));
            require(sao_ui_widget_paint_at(resources.label, resources.paint, 12, y,
                                           paint_width + 4, height + 4, 1.0F));
            require(sao_ui_paint_ctx_draw_utf8(resources.paint, 604, static_cast<float>(y),
                name, 12, 0xff526679));
            std::fprintf(stderr, "READABILITY_LABEL case=%s size=%d spacing=%.1f width=%d height=%d\n",
                         name, size, spacing, width, height);
            y += height + 20;
            return std::pair{width, height};
        };
        const auto small_label = sample("16 px", "SAO Interface 0123456789", 16, 0, false, 1, 0);
        const auto large_label = sample("28 px", "SAO Interface 0123456789", 28, 0, false, 1, 0);
        verify_readability(large_label.first > small_label.first * 3 / 2 && large_label.second > small_label.second,
                           "label-real-size-scaling");
        const auto mixed = sample("Mixed", "设置 Settings / 中文 ABC 123", 18, 0, false, 1, 0);
        const auto tracked = sample("Tracking", "设置 Settings / 中文 ABC 123", 18, 0, false, 1, 2);
        verify_readability(tracked.first > mixed.first, "label-explicit-tracking");
        const auto wrapped = sample("Wrap", "中文说明 Mixed labels with long words and 0123456789", 18, 180, true, 0, 0);
        const auto capped = sample("Max 2", "中文说明 Mixed labels with long words and 0123456789", 18, 180, true, 2, 0);
        verify_readability(wrapped.second >= capped.second && capped.second > mixed.second,
                           "label-max-lines-height");
        sample("Ellipsis", "一个很长的插件标题 / Long plugin title", 18, 155, false, 1, 0);
        sample("Bold", "设置 Settings / 中文 ABC 123", 18, 0, false, 1, 0, SAO_UI_WEIGHT_BOLD);
        sample("Default", "Default size 默认字号", 0, 0, false, 1, 0);
        sample("Empty line", "第一行\n\nThird line", 16, 220, true, 0, 0);
        require(sao_ui_paint_ctx_end_frame(resources.paint));
        size_t written{};
        uint32_t width{}, height{}, stride{};
        std::vector<uint8_t> pixels(static_cast<size_t>(raster_spec.width_px) * raster_spec.height_px * 4);
        require(sao_ui_offscreen_raster_snapshot(resources.raster, pixels.data(), pixels.size(),
            &written, &width, &height, &stride));
        verify_readability(written == pixels.size() && stride == width * 4, "label-raster-export");
        BITMAPFILEHEADER file{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(written);
        BITMAPINFOHEADER image{};
        image.biSize = sizeof(image);
        image.biWidth = static_cast<LONG>(width);
        image.biHeight = -static_cast<LONG>(height);
        image.biPlanes = 1;
        image.biBitCount = 32;
        image.biCompression = BI_RGB;
        image.biSizeImage = static_cast<DWORD>(written);
        auto output_path = config.frame_out;
        output_path.replace_filename(output_path.stem().wstring() + L".labels.bmp");
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&image), sizeof(image));
        output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(written));
        output.close();
        verify_readability(static_cast<bool>(output), "label-file-written");
    }

    void probe_keyboard_navigation() {
        std::array<std::string, 12> names;
        std::array<SaoUiMenuItem, 12> children{};
        for (size_t i = 0; i < children.size(); ++i) {
            names[i] = "导航行 / Action " + std::to_string(i);
            children[i].name_utf8 = names[i].c_str();
            children[i].icon_utf8 = "sao:info";
            children[i].action_id = 2800 + static_cast<int32_t>(i);
            children[i].can_activate = i != 0 && i != 2;
        }
        children[2].name_utf8 = "────────";
        children[2].action_id = -1;
        SaoUiEntityRootItem root{};
        root.struct_size = sizeof(root);
        root.root_id_utf8 = "preview.keyboard";
        root.name_utf8 = "键盘导航 / Keyboard";
        root.icon_utf8 = "sao:keyboard";
        root.action_id = -1;
        root.can_activate = true;
        root.children = children.data();
        root.child_count = children.size();
        const auto settle = [this] {
            for (unsigned i = 0; i < 40; ++i) {
                require(sao_ui_entity_shell_tick(entity, 20));
                sync_backdrop(20);
            }
        };
        const auto press = [this](uint32_t key_code) {
            require(sao_ui_entity_shell_handle_key(entity, key_code, 0));
        };
        const auto visible = [this] {
            SaoUiEntityShellSnapshot snapshot{};
            require(sao_ui_entity_shell_get_snapshot(entity, &snapshot));
            return snapshot.menu_visible;
        };
        const auto at_root = [this] {
            SaoUiEntityRootSnapshot snapshot{};
            require(sao_ui_entity_shell_get_root_snapshot(entity, &snapshot));
            return snapshot.active_root_id_utf8[0] == '\0';
        };
        const auto invoked = [this](int32_t action, uint32_t count, const char* check) {
            verify_readability(readability_last_action == action && readability_action_count == count, check);
        };
        require(sao_ui_entity_shell_set_roots(entity, &root, 1));
        if (!visible()) require(sao_ui_entity_shell_home(entity));
        settle();
        press(VK_RIGHT);
        press(VK_RETURN);
        verify_readability(readability_action_count == 0, "keyboard-opening-not-activated");
        settle();
        press(VK_RETURN);
        invoked(2801, 1, "keyboard-first-enabled-child");
        press(VK_DOWN);
        press(VK_SPACE);
        invoked(2803, 2, "keyboard-skip-disabled-separator");
        for (unsigned i = 0; i < 8; ++i) press(VK_DOWN);
        press(VK_RETURN);
        invoked(2811, 3, "keyboard-scroll-past-eight-rows");
        press(VK_DOWN);
        press(VK_RETURN);
        invoked(2801, 4, "keyboard-wrap-enabled-rows");
        press(VK_ESCAPE);
        verify_readability(visible() && at_root(), "keyboard-escape-to-root");
        press(VK_ESCAPE);
        verify_readability(!visible(), "keyboard-second-escape-closes");
        require(sao_ui_entity_shell_home(entity));
        settle();
        press(VK_RIGHT);
        press(VK_LEFT);
        verify_readability(visible() && at_root(), "keyboard-left-cancels-opening");
        settle();
        press(VK_RIGHT);
        settle();
        press(VK_DOWN);
        for (auto& child : children)
            if (child.can_activate) child.action_id += 50;
        require(sao_ui_entity_shell_set_roots(entity, &root, 1));
        settle();
        press(VK_RETURN);
        verify_readability(readability_action_count == 4, "keyboard-refresh-invalidates-old-focus");
        press(VK_DOWN);
        press(VK_RETURN);
        invoked(2851, 5, "keyboard-new-publication-action");
        require(sao_ui_entity_shell_handle_mouse(entity, WM_CANCELMODE, 0, 0, -1, 0));
        press(VK_RETURN);
        verify_readability(readability_action_count == 5, "keyboard-cancel-clears-focus");
        for (auto& child : children) child.can_activate = false;
        require(sao_ui_entity_shell_set_roots(entity, &root, 1));
        settle();
        press(VK_DOWN);
        press(VK_RETURN);
        verify_readability(readability_action_count == 5, "keyboard-all-disabled-no-action");
        for (size_t i = 0; i < children.size(); ++i)
            children[i].can_activate = i != 0 && i != 2;
        require(sao_ui_entity_shell_set_roots(entity, &root, 1));
        press(VK_LEFT);
        settle();
        press(VK_RIGHT);
        settle();
    }

    void probe_plugin_status() {
        auto original = plugin_snapshot();
        SaoUiPluginTab candidate{};
        candidate.struct_size = sizeof(candidate);
        candidate.plugin_id_utf8 = "preview.cutegirl";
        candidate.name_utf8 = "看板娘 / CuteGirl";
        candidate.icon_utf8 = "sao:plugins";
        candidate.enabled = true;
        candidate.status = 255;
        verify_readability(sao_ui_plugin_tabs_set_items(plugin_tabs, &candidate, 1) ==
                           SAO_STATUS_ERR_INVALID_ARGUMENT, "plugin-invalid-status-rejected");
        verify_readability(plugin_snapshot().tab_count == original.tab_count,
                           "plugin-invalid-status-keeps-catalog");
        candidate.status = SAO_UI_PLUGIN_TAB_STATUS_UNKNOWN;
        candidate.reserved[0] = 1;
        verify_readability(sao_ui_plugin_tabs_set_items(plugin_tabs, &candidate, 1) ==
                           SAO_STATUS_ERR_INVALID_ARGUMENT, "plugin-reserved-rejected");
        candidate.reserved[0] = 0;
        require(sao_ui_plugin_tabs_set_items(plugin_tabs, &candidate, 1));
        verify_readability(plugin_snapshot().tab_count == 1, "plugin-legacy-zero-status");
        candidate.status = SAO_UI_PLUGIN_TAB_STATUS_DISABLED;
        require(sao_ui_plugin_tabs_set_items(plugin_tabs, &candidate, 1));
        auto snapshot = plugin_snapshot();
        plugin_click(snapshot.x + snapshot.width / 2, snapshot.y + plugin_first_row_center);
        verify_readability(std::string_view(plugin_snapshot().selected_plugin_id_utf8) ==
                           "preview.cutegirl", "plugin-disabled-empty-selectable");
        require(publish_plugin_samples());
        snapshot = plugin_snapshot();
        verify_readability(snapshot.tab_count == kPreviewPluginCount &&
                           std::string_view(snapshot.selected_plugin_id_utf8) == "preview.cutegirl",
                           "plugin-status-refresh-keeps-selection");
        const auto calls = plugin_action_count;
        plugin_click(snapshot.x + snapshot.width + 12 + 60,
                     snapshot.y + plugin_child_row_stride + plugin_first_row_center);
        verify_readability(plugin_action_count == calls, "plugin-disabled-action-blocked");
        require(sao_ui_plugin_tabs_tick(plugin_tabs, 0));
    }

    void export_frame() {
        for (int32_t remaining = config.frame_ms; remaining > 0;) {
            const int32_t step = std::min(remaining, intro != nullptr || config.menu_motion || config.plugin_tabs ? 16 : 1000);
            advance_menu_motion(step);
            advance_plugin_tabs(step);
            if (entity != nullptr)
                require(sao_ui_entity_shell_tick(entity, static_cast<uint32_t>(step)));
            if (intro != nullptr) {
                bool active = false;
                require(sao_ui_linkstart_is_active(intro, &active));
                if (active)
                    require(sao_ui_linkstart_tick(intro, step));
                report_outro(active ? static_cast<uint32_t>(step) : 0);
                complete_startup_menu();
            }
            sync_backdrop(static_cast<uint32_t>(step));
            remaining -= step;
        }
        if (config.readability_probe) {
            probe_label_layout();
            probe_keyboard_navigation();
            probe_plugin_status();
            std::fprintf(stderr, "READABILITY_PROBE_OK checks=%zu actions=%u abi=%u\n",
                         readability_checks, readability_action_count, sao_ui_abi_version());
        }
        for (int32_t index = 0; index < config.frame_count; ++index) {
            if (index > 0) {
                const int32_t step = index * 1000 / 60 - (index - 1) * 1000 / 60;
                advance_menu_motion(step);
                advance_plugin_tabs(step);
                if (entity != nullptr)
                    require(sao_ui_entity_shell_tick(entity, static_cast<uint32_t>(step)));
                sync_backdrop(static_cast<uint32_t>(step));
                if (intro != nullptr) {
                    bool active = false;
                    require(sao_ui_linkstart_is_active(intro, &active));
                    if (active)
                        require(sao_ui_linkstart_tick(intro, step));
                    report_outro(active ? static_cast<uint32_t>(step) : 0);
                    complete_startup_menu();
                }
            }
            write_frame();
        }
        if (config.plugin_tabs) {
            const auto expected = static_cast<size_t>(std::count_if(plugin_event_times.begin(),
                plugin_event_times.end(), [this](int32_t time) { return time <= plugin_tabs_ms; }));
            verify_plugin_tabs(plugin_tabs_event == expected, "export-event-coverage");
            std::fprintf(stderr, "PLUGIN_TABS_EXPORT time=%d frames=%d events=%zu/%zu complete=%d\n",
                         plugin_tabs_ms, config.frame_count, plugin_tabs_event, plugin_event_times.size(),
                         plugin_tabs_event == plugin_event_times.size() ? 1 : 0);
            std::fflush(stderr);
        }
    }

    void write_frame() {
        uint32_t width = 0, height = 0;
        size_t bytes = 0;
        const auto query = sao_ui_compositor_snapshot_bgra(
            compositor, nullptr, 0, &width, &height, &bytes);
        if (query != SAO_STATUS_ERR_BUFFER_TOO_SMALL)
            require(query);
        const bool empty_outro = config.outro && outro_completed && !outro_failed && bytes == 0;
        if (empty_outro) {
            RECT client{};
            if (!GetClientRect(window, &client)) throw std::runtime_error("Outro frame bounds unavailable");
            width = frame_width != 0 ? frame_width : static_cast<uint32_t>(std::max(0L, client.right));
            height = frame_height != 0 ? frame_height : static_cast<uint32_t>(std::max(0L, client.bottom));
            const uint64_t required = static_cast<uint64_t>(width) * height * 4u;
            if (required > UINT32_MAX - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
                throw std::runtime_error("Outro frame bounds too large");
            bytes = static_cast<size_t>(required);
        }
        if (bytes == 0 || bytes > UINT32_MAX - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
            throw std::runtime_error("Invalid native frame dimensions");
        std::vector<uint8_t> pixels(bytes);
        if (!empty_outro)
            require(sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(),
                                                     &width, &height, &bytes));
        frame_width = width;
        frame_height = height;
        BITMAPFILEHEADER file{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(bytes);
        BITMAPINFOHEADER image{};
        image.biSize = sizeof(image);
        image.biWidth = static_cast<LONG>(width);
        image.biHeight = -static_cast<LONG>(height);
        image.biPlanes = 1;
        image.biBitCount = 32;
        image.biCompression = BI_RGB;
        image.biSizeImage = static_cast<DWORD>(bytes);
        if (config.frame_out == L"-") {
            const HANDLE pipe = GetStdHandle(STD_OUTPUT_HANDLE);
            if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || GetFileType(pipe) != FILE_TYPE_PIPE)
                throw std::runtime_error("Frame stream requires a redirected stdout pipe");
            const auto write = [pipe](const void* data, DWORD size) {
                const auto* cursor = static_cast<const uint8_t*>(data);
                while (size > 0) {
                    DWORD written = 0;
                    if (!WriteFile(pipe, cursor, size, &written, nullptr) || written == 0)
                        throw std::runtime_error("Frame stream closed");
                    cursor += written;
                    size -= written;
                }
            };
            write(&file, sizeof(file));
            write(&image, sizeof(image));
            write(pixels.data(), static_cast<DWORD>(bytes));
            return;
        }
        std::filesystem::create_directories(config.frame_out.parent_path());
        std::ofstream output(config.frame_out, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&image), sizeof(image));
        output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(bytes));
        output.close();
        if (!output)
            throw std::runtime_error("Native frame export failed");
    }

    void service() {
        const ULONGLONG now = GetTickCount64();
        if (closing)
            return;
        const uint32_t elapsed = static_cast<uint32_t>(std::min<ULONGLONG>(now - last_tick, 1000U));
        if (!config.outro)
            sao::launcher::tickUserGuideWebView();
        if (entity != nullptr)
            require(sao_ui_entity_shell_tick(entity, elapsed));
        if (plugin_tabs != nullptr)
            require(sao_ui_plugin_tabs_tick(plugin_tabs, elapsed));
        sync_backdrop(elapsed);
        if (now - last_service >= 50) {
            if (ai_settings != nullptr)
                require(sao_ai_editor_settings_panel_tick(ai_settings));
            if (ai_main != nullptr)
                require(sao_ai_editor_main_panel_tick(ai_main));
            if (plugin_manager != nullptr)
                require_active(plugin_manager->service_ui());
            if (process_selector != nullptr)
                require_active(process_selector->service_ui());
            if (memory_viewer != nullptr)
                require_active(memory_viewer->service_ui());
            if (workshop != nullptr)
                require_active(workshop->service_ui());
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
            if (license != nullptr)
                require_active(license->service_ui());
#endif
            last_service = now;
        }
        if (intro != nullptr) {
            bool active = false;
            require(sao_ui_linkstart_is_active(intro, &active));
            const auto intro_elapsed = static_cast<int32_t>(
                std::min<ULONGLONG>(now - last_tick, static_cast<ULONGLONG>(INT32_MAX)));
            if (active) {
                const sao_status_t status = sao_ui_linkstart_tick(intro, intro_elapsed);
                if (status != SAO_STATUS_OK) {
                    if (config.outro) {
                        outro_failed = true;
                        std::fprintf(stderr, "outro tick_failed status=%d\n", status);
                    }
                    (void)sao_ui_linkstart_dismiss(intro);
                }
            }
            report_outro(active ? static_cast<uint32_t>(intro_elapsed) : 0);
            complete_startup_menu();
            if (config.intro_audition) {
                SaoUiLinkStartPhase phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
                float progress = 0.0F;
                require(sao_ui_linkstart_get_phase(intro, &phase, &progress));
                SaoUiLinkStartAudioState audio = SAO_UI_LINKSTART_AUDIO_READY;
                sao_status_t audio_status = SAO_STATUS_OK;
                require(sao_ui_linkstart_get_audio_state(intro, &audio, &audio_status));
                if (audition_phase != phase) {
                    std::fprintf(stderr, "intro phase=%d wall_ms=%llu audio=%d status=%d\n",
                                 static_cast<int>(phase), GetTickCount64() - intro_started,
                                 static_cast<int>(audio), audio_status);
                    audition_phase = phase;
                }
                if (!audition_flight_reported && phase == SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL &&
                    progress > 0.0F) {
                    std::fprintf(stderr, "intro first_flight wall_ms=%llu\n",
                                 GetTickCount64() - intro_started);
                    audition_flight_reported = true;
                }
                require(sao_ui_linkstart_is_active(intro, &active));
                if (!active)
                    closing = true;
            }
        }
        last_tick = now;
        if (!IsIconic(window)) {
            sao_status_t status = sao_ui_compositor_tick(compositor);
            if (status != SAO_STATUS_OK && intro != nullptr) {
                if (config.outro) {
                    outro_failed = true;
                    std::fprintf(stderr, "outro present_failed status=%d\n", status);
                    std::fflush(stderr);
                }
                (void)sao_ui_linkstart_dismiss(intro);
                status = sao_ui_compositor_tick(compositor);
            }
            if (status != SAO_STATUS_ERR_DEVICE_LOST)
                require(status);
        }
    }

    void mouse(UINT message, WPARAM wp, LPARAM lp) noexcept {
        if (compositor == nullptr)
            return;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (message != WM_MOUSEWHEEL)
            ClientToScreen(window, &point);
        int32_t button = -1;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
            button = 0;
        else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
            button = 1;
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            button = 2;
        const int32_t wheel = message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wp) : 0;
        (void)sao_ui_compositor_dispatch_mouse(compositor, message, point.x, point.y, button, wheel);
    }

    bool key(const MSG& message) {
        if (closing)
            return true;
        if (config.outro &&
            (message.message == WM_KEYDOWN || message.message == WM_KEYUP ||
             message.message == WM_CHAR || message.message == WM_HOTKEY ||
             message.message == SAO_UI_NATIVE_TEXT_TAB_MESSAGE))
            return true;
        if (intro != nullptr &&
            (message.message == WM_KEYDOWN || message.message == WM_KEYUP ||
             message.message == WM_CHAR || message.message == WM_HOTKEY ||
             message.message == SAO_UI_NATIVE_TEXT_TAB_MESSAGE)) {
            bool intro_active = false;
            require(sao_ui_linkstart_is_active(intro, &intro_active));
            if (intro_active)
                return true;
        }
        if (message.message == WM_HOTKEY && sao::launcher::userGuideWebViewVisible())
            return true;
        if (message.message == WM_HOTKEY)
            return sao::launcher::hotkey::dispatch_by_native_id(static_cast<int>(message.wParam));
        if (message.message == sao::launcher::hotkey::kCaptureCompletionMessage &&
            hotkey_owner != nullptr) {
            require(hotkey_owner->drain_capture_for_owner());
            return true;
        }
        if (message.message != WM_KEYDOWN && message.message != WM_KEYUP &&
            message.message != WM_CHAR && message.message != SAO_UI_NATIVE_TEXT_TAB_MESSAGE)
            return false;
        const HWND render = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        if (GetFocus() != render && GetFocus() != window)
            return false;
        if (sao::launcher::userGuideWebViewVisible()) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
                sao::launcher::hideUserGuideWebView();
            return true;
        }
        bool consumed = false;
        const sao_status_t status = sao_ui_input_router_feed_raw_win32(
            keyboard, message.message, message.wParam, message.lParam, &consumed);
        if (status != SAO_STATUS_ERR_NOT_FOUND)
            require(status);
        if (!consumed && message.message == WM_KEYDOWN && entity != nullptr) {
            const sao_status_t shell_status =
                sao_ui_entity_shell_handle_key(entity, static_cast<uint32_t>(message.wParam), 0);
            if (shell_status != SAO_STATUS_ERR_NOT_FOUND)
                require(shell_status);
            consumed = shell_status == SAO_STATUS_OK;
        }
        return consumed;
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (host == nullptr)
        return DefWindowProcW(window, message, wp, lp);
    try {
        switch (message) {
        case WM_SIZE:
        case WM_MOVE:
            host->resize();
            return 0;
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            if (!host->closing)
                host->mouse(message, wp, lp);
            return 0;
        case WM_CANCELMODE:
            if (!host->closing)
                host->mouse(message, 0, 0);
            return DefWindowProcW(window, message, wp, lp);
        case WM_CAPTURECHANGED:
            if (!host->closing && reinterpret_cast<HWND>(lp) != window)
                host->mouse(message, 0, 0);
            return DefWindowProcW(window, message, wp, lp);
        case WM_KILLFOCUS:
            if (!host->closing)
                host->mouse(WM_CANCELMODE, 0, 0);
            return DefWindowProcW(window, message, wp, lp);
        case WM_ACTIVATEAPP:
            if (!host->closing && !wp)
                host->mouse(WM_CANCELMODE, 0, 0);
            return DefWindowProcW(window, message, wp, lp);
        case WM_TIMER:
            if (wp == kUiService)
                host->service();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {820, 620};
            return 0;
        case WM_SYSCOMMAND:
            if (host->closing)
                return 0;
            break;
        case WM_CLOSE:
            host->closing = true;
            if (!PostMessageW(window, WM_NULL, 0, 0))
                PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            host->closing = true;
            KillTimer(window, kUiService);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            host->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Production UI message %u: %s\n", message, error.what());
        std::fflush(stderr);
        SetWindowTextA(window, error.what());
        host->message_failed = true;
        if (host->config.outro)
            host->outro_failed = true;
        host->closing = true;
        if (!PostMessageW(window, WM_NULL, 0, 0))
            PostQuitMessage(1);
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool frame_export = false;
    int argument_count = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            frame_export = frame_export || argument == L"--frame-out" || argument == L"--frame-ms" ||
                           argument == L"--frame-count";
        }
        LocalFree(arguments);
    }
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_status)) {
        std::fprintf(stderr, "STA COM initialization failed: %ld\n", static_cast<long>(com_status));
        std::fflush(stderr);
        return 1;
    }
    int code = 1;
    bool retained_host = false;
    try {
        auto config = options();
        if (config.shared_texture_probe) {
            code = run_shared_texture_probe();
            CoUninitialize();
            return code;
        }
        auto owner = std::make_unique<Host>(std::move(config));
        Host& host = *owner;
        frame_export = !host.config.frame_out.empty();
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = L"SaoProductionUiHost";
        if (!RegisterClassW(&cls))
            throw std::runtime_error("Window class unavailable");
        HWND window = CreateWindowExW(0, cls.lpszClassName, L"SAO Classic · production UI shell",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
                                      nullptr, nullptr, instance, &host);
        if (window == nullptr)
            throw std::runtime_error("Window creation failed");
        try {
            host.initialize();
            if (frame_export) {
                host.export_frame();
                require(host.drain_close());
                code = host.outro_failed || host.message_failed ? 1 : host.quit_code.value_or(0);
            } else {
                ShowWindow(window, show);
                MSG message{};
                BOOL received = 0;
                while (!host.closing) {
                    received = GetMessageW(&message, nullptr, 0, 0);
                    if (received <= 0 || host.closing) break;
                    if (!host.key(message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (host.closing)
                        break;
                }
                if (received == 0 && message.message == WM_QUIT)
                    host.quit_code = static_cast<int>(message.wParam);
                require(host.drain_close());
                code = received < 0 || host.outro_failed || host.message_failed
                           ? 1
                           : host.quit_code.value_or(0);
            }
            if (IsWindow(window))
                DestroyWindow(window);
        } catch (...) {
            if (host.drain_close() == SAO_STATUS_OK) {
                if (IsWindow(window)) DestroyWindow(window);
            } else {
                (void)owner.release();
                retained_host = true;
            }
            throw;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Production UI startup: %s\n", error.what());
        std::fflush(stderr);
    }
    if (!retained_host) CoUninitialize();
    return code;
}
