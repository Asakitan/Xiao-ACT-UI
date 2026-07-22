#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
#ifdef SAO_STATUS_OK
#undef SAO_STATUS_OK
#endif
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/overlay_host.h"
extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_live_context_count(void);
#endif

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
extern "C" size_t sao_launcher_test_platform_timer_count() noexcept;
extern "C" size_t sao_launcher_test_platform_timer_worker_count() noexcept;
extern "C" uint64_t sao_launcher_test_platform_timer_unregister_attempt_count() noexcept;
extern "C" void sao_launcher_test_fire_timer_during_register(bool enabled) noexcept;
extern "C" void sao_launcher_test_fail_next_timer_unregister(bool enabled) noexcept;
#endif

namespace {

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
constexpr int32_t kPythonNoHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_UNCONFIGURED;
constexpr int32_t kPythonMissingHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_UNAVAILABLE;
#else
constexpr int32_t kPythonNoHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE;
constexpr int32_t kPythonMissingHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE;
#endif

namespace fs = std::filesystem;

class temporary_tree {
  public:
    explicit temporary_tree(const char* label) {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                (std::string("sao_launcher_provider_") + label + "_" + std::to_string(suffix));
        REQUIRE(fs::create_directories(root_));
    }

    ~temporary_tree() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    const fs::path& root() const {
        return root_;
    }

  private:
    fs::path root_;
};

class current_path_guard {
  public:
    explicit current_path_guard(const fs::path& replacement) : original_(fs::current_path()) {
        fs::current_path(replacement);
    }

    ~current_path_guard() {
        std::error_code error;
        fs::current_path(original_, error);
    }

  private:
    fs::path original_;
};

void write_text(const fs::path& path, const std::string& content) {
    REQUIRE((fs::create_directories(path.parent_path()) || fs::is_directory(path.parent_path())));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

std::vector<fs::path> configured_roots() {
    std::vector<fs::path> roots;
    for (const auto& root : sao::launcher::launcherProviderConfigurationSnapshot().plugins.roots) {
        roots.push_back(fs::weakly_canonical(root));
    }
    return roots;
}

std::string path_utf8(const fs::path& path) {
    const auto wide = path.wstring();
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(size > 0);
    std::string output(static_cast<size_t>(size), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), output.data(), size, nullptr,
                                nullptr) == size);
    return output;
}

sao::plugins::loader::plugin_handle_t add_external_plugin(const char* plugin_id,
                                                           const fs::path& source_path) {
    sao::plugins::loader::plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "fixture.emma";
    manifest.language = sao::plugins::loader::engine_kind::emma;
    manifest.source_path = source_path.string();
    manifest.abi_version = 2;
    sao::plugins::loader::plugin_handle_t handle = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_registry_add_plugin(
                sao::plugins::loader::sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    return handle;
}

void platform_timer_callback(void*) {}

int32_t platform_render_callback(const char*, const char*, char**, void*) {
    return SAO_OK;
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kMouseWheel = 0x020A;

struct PlatformCompositorProbe {
    sao::plugins::loader::plugin_context_t* context = nullptr;
    uint32_t cursor_calls = 0;
    uint32_t button_calls = 0;
    uint32_t leave_calls = 0;
    uint32_t scroll_calls = 0;
    float x = -1.0F;
    float y = -1.0F;
    uint32_t button = UINT32_MAX;
    bool pressed = false;
    float scroll_y = 0.0F;
    bool rebind_on_cursor = false;
    bool rebind_on_button_release = false;
    PlatformCompositorProbe* rebind_target = nullptr;
    int32_t rebind_status = SAO_OK;
};

void platform_compositor_cursor(float x, float y, void* user_data);
void platform_compositor_button(uint32_t button, bool pressed, void* user_data);
void platform_compositor_leave(void* user_data);
void platform_compositor_scroll(float dx, float dy, void* user_data);

void platform_compositor_cursor(float x, float y, void* user_data) {
    auto& probe = *static_cast<PlatformCompositorProbe*>(user_data);
    ++probe.cursor_calls;
    probe.x = x;
    probe.y = y;
    if (probe.rebind_on_cursor) {
        probe.rebind_on_cursor = false;
        probe.rebind_status =
            sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
                probe.context, "probe", &platform_compositor_cursor,
                &platform_compositor_button, &platform_compositor_leave,
                &platform_compositor_scroll, &probe);
    }
}

void platform_compositor_button(uint32_t button, bool pressed, void* user_data) {
    auto& probe = *static_cast<PlatformCompositorProbe*>(user_data);
    ++probe.button_calls;
    probe.button = button;
    probe.pressed = pressed;
    if (!pressed && probe.rebind_on_button_release) {
        probe.rebind_on_button_release = false;
        auto* target = probe.rebind_target == nullptr ? &probe : probe.rebind_target;
        probe.rebind_status =
            sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
                probe.context, "probe", &platform_compositor_cursor,
                &platform_compositor_button, &platform_compositor_leave,
                &platform_compositor_scroll, target);
    }
}

void platform_compositor_leave(void* user_data) {
    ++static_cast<PlatformCompositorProbe*>(user_data)->leave_calls;
}

void platform_compositor_scroll(float, float dy, void* user_data) {
    auto& probe = *static_cast<PlatformCompositorProbe*>(user_data);
    ++probe.scroll_calls;
    probe.scroll_y = dy;
}

size_t compositor_layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    return sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK
               ? count
               : SIZE_MAX;
}

std::vector<HWND> top_level_process_windows() {
    struct EnumState {
        DWORD process_id;
        std::vector<HWND> windows;
    } state{GetCurrentProcessId(), {}};
    EnumWindows(
        [](HWND hwnd, LPARAM parameter) -> BOOL {
            auto& state = *reinterpret_cast<EnumState*>(parameter);
            DWORD process_id = 0;
            GetWindowThreadProcessId(hwnd, &process_id);
            if (process_id == state.process_id &&
                (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) == 0)
                state.windows.push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    std::ranges::sort(state.windows, {}, [](HWND hwnd) {
        return reinterpret_cast<uintptr_t>(hwnd);
    });
    return state.windows;
}
#endif

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK) && defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
struct CallbackUnloadObservation {
    uint32_t calls = 0;
    int32_t last_status = SAO_OK;
    bool all_busy = true;
    bool all_states_preserved = true;
    bool stop_unchanged = true;
};

struct CallbackUnloadProbe {
    sao::plugins::loader::plugin_handle_t handle = nullptr;
    sao::plugins::loader::plugin_context_t* context = nullptr;
    int32_t callback_destroy_status = SAO_OK;
    CallbackUnloadObservation cursor;
    CallbackUnloadObservation button;
    CallbackUnloadObservation leave;
    CallbackUnloadObservation scroll;
};

void record_callback_unload(CallbackUnloadProbe& probe, CallbackUnloadObservation& observation) {
    ++observation.calls;
    observation.last_status = sao::plugins::loader::sao_plugins_lifecycle_unload(probe.handle);
    observation.all_busy =
        observation.all_busy &&
        observation.last_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    observation.all_states_preserved =
        observation.all_states_preserved &&
        sao::plugins::loader::sao_plugins_lifecycle_state(probe.handle) ==
            sao::plugins::loader::lifecycle_state::loaded_active;
    observation.stop_unchanged =
        observation.stop_unchanged && !sao::plugins::loader::sao_plugins_ctx_should_stop(probe.context);
}

void callback_unload_cursor(float, float, void* user_data) {
    auto& probe = *static_cast<CallbackUnloadProbe*>(user_data);
    record_callback_unload(probe, probe.cursor);
    probe.callback_destroy_status =
        sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(probe.context,
                                                                       "callback_gate");
}

void callback_unload_button(uint32_t, bool, void* user_data) {
    auto& probe = *static_cast<CallbackUnloadProbe*>(user_data);
    record_callback_unload(probe, probe.button);
}

void callback_unload_leave(void* user_data) {
    auto& probe = *static_cast<CallbackUnloadProbe*>(user_data);
    record_callback_unload(probe, probe.leave);
}

void callback_unload_scroll(float, float, void* user_data) {
    auto& probe = *static_cast<CallbackUnloadProbe*>(user_data);
    record_callback_unload(probe, probe.scroll);
}
#endif

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
struct FailedSdkBindMemoryProvider {
    int close_failures = 2;
    int close_attempts = 0;
    SaoSdkMemoryProviderVTable provider{};

    FailedSdkBindMemoryProvider() {
        provider.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
        provider.struct_size = sizeof(provider);
        provider.user_data = this;
        provider.retain = &retain;
        provider.release = &release;
        provider.open_session = &openSession;
        provider.close_session = &closeSession;
        provider.attach = &attach;
        provider.detach = &detach;
        provider.read = &read;
        provider.enumerate_modules = &enumerateModules;
    }

    static void SAO_SDK_CALL retain(void*) {}
    static void SAO_SDK_CALL release(void*) {}

    static sao_sdk_status_t SAO_SDK_CALL openSession(void* user_data, const char*, void** out) {
        if (out != nullptr)
            *out = user_data;
        return SAO_SDK_ERR_INTERNAL;
    }

    static sao_sdk_status_t SAO_SDK_CALL closeSession(void* user_data, void*) {
        auto* self = static_cast<FailedSdkBindMemoryProvider*>(user_data);
        ++self->close_attempts;
        if (self->close_failures > 0) {
            --self->close_failures;
            return SAO_SDK_ERR_INTERNAL;
        }
        return SAO_SDK_OK;
    }

    static sao_sdk_status_t SAO_SDK_CALL attach(void*, void*,
                                                const SaoSdkMemoryTargetIdentity*) {
        return SAO_SDK_OK;
    }

    static sao_sdk_status_t SAO_SDK_CALL detach(void*, void*) {
        return SAO_SDK_OK;
    }

    static sao_sdk_status_t SAO_SDK_CALL read(void*, void*, uint64_t, void*, size_t,
                                              size_t* out_bytes_read) {
        if (out_bytes_read != nullptr)
            *out_bytes_read = 0;
        return SAO_SDK_ERR_READ_FAULT;
    }

    static sao_sdk_status_t SAO_SDK_CALL enumerateModules(void*, void*, SaoSdkMemoryModule*,
                                                          size_t, size_t, size_t* out_count) {
        if (out_count != nullptr)
            *out_count = 0;
        return SAO_SDK_OK;
    }
};
#endif

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
template <typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

struct one_shot_probe {
    sao::plugins::loader::plugin_context_t* context = nullptr;
    std::array<char, 32> token{};
    std::atomic_bool token_ready{false};
    std::atomic_uint32_t callbacks{0};
    std::atomic_uint32_t callbacks_before_token{0};
    bool complete_loader_ledger = true;
    bool block_callback = false;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
};

void one_shot_callback(void* user_data) {
    auto& probe = *static_cast<one_shot_probe*>(user_data);
    if (!probe.token_ready.load(std::memory_order_acquire)) {
        probe.callbacks_before_token.fetch_add(1, std::memory_order_relaxed);
    } else if (probe.complete_loader_ledger) {
        (void)sao::plugins::loader::sao_plugins_ctx_complete_timer(probe.context,
                                                                   probe.token.data());
    }
    probe.callbacks.fetch_add(1, std::memory_order_relaxed);
    if (!probe.block_callback)
        return;
    std::unique_lock lock(probe.mutex);
    probe.entered = true;
    probe.condition.notify_all();
    probe.condition.wait(lock, [&probe] { return probe.release; });
}

void publish_timer_token(one_shot_probe& probe, char* token) {
    REQUIRE(token != nullptr);
    REQUIRE(token[0] != '\0');
    REQUIRE(strlen(token) < probe.token.size());
    strcpy_s(probe.token.data(), probe.token.size(), token);
    sao::plugins::loader::sao_plugins_ctx_free_string(token);
    probe.token_ready.store(true, std::memory_order_release);
}

bool wait_for_blocked_callback(one_shot_probe& probe) {
    std::unique_lock lock(probe.mutex);
    return probe.condition.wait_for(lock, std::chrono::seconds(5),
                                    [&probe] { return probe.entered; });
}

void release_blocked_callback(one_shot_probe& probe) {
    {
        std::lock_guard lock(probe.mutex);
        probe.release = true;
    }
    probe.condition.notify_all();
}
#endif

} // namespace

TEST_CASE("launcher owns real platform provider sessions and excludes unwired capabilities",
          "[launcher][provider][plugins][platform][focused]") {
    temporary_tree tree("platform_provider");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    SaoCompositorConfig compositor_config{};
    compositor_config.target_hz = 60;
    compositor_config.enable_temporal_union = true;
    compositor_config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) ==
            SAO_STATUS_OK);
    REQUIRE(sao_sdk_platform_bind_ui_compositor(compositor) == SAO_SDK_OK);
#endif

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);

    const auto handle = add_external_plugin("launcher_platform_provider_probe", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    wchar_t* selected_path = reinterpret_cast<wchar_t*>(1);
    CHECK(sao::plugins::loader::sao_plugins_ctx_open_file(context, "[]", "Pick", L"", 0,
                                                           &selected_path) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(selected_path == nullptr);
    CHECK(sao::plugins::loader::sao_plugins_ctx_open_window(context, "probe", 320, 240) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao::plugins::loader::sao_plugins_ctx_register_hotkey(
              context, "probe", "F12", "Probe", platform_timer_callback, nullptr) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    uint32_t render_token = 0;
    CHECK(sao::plugins::loader::sao_plugins_ctx_register_render_hook(
              context, "probe", 0.0F, platform_render_callback, nullptr, &render_token) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(render_token == 0);
    CHECK(sao::plugins::loader::sao_plugins_ctx_set_overlay(context, "probe", "{}") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao::plugins::loader::sao_plugins_ctx_request_redraw(context, "probe", "test") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    const std::string maximum_public_name(
        SAO_PLUGIN_CONTEXT_COMPOSITOR_LAYER_NAME_MAX_BYTES, 'n');
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
                context, maximum_public_name.c_str(), 4, 4, 3, 5, 20, false, false, 60) ==
            SAO_OK);
    CHECK(compositor_layer_count(compositor) == 1);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(
                context, maximum_public_name.c_str()) == SAO_OK);
    CHECK(compositor_layer_count(compositor) == 0);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
                context, "probe", 4, 4, 3, 5, 20, false, false, 60) == SAO_OK);
    CHECK(compositor_layer_count(compositor) == 1);
    CHECK(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
              context, "probe", 4, 4, 3, 5, 20, false, false, 60) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    const std::array<uint8_t, 4U * 4U * 4U> frame = [] {
        std::array<uint8_t, 4U * 4U * 4U> pixels{};
        pixels.fill(255);
        return pixels;
    }();
    int32_t foreign_upload_status = SAO_ERR_NOT_INITIALIZED;
    std::thread foreign_upload([&] {
        foreign_upload_status = sao::plugins::loader::sao_plugins_ctx_upload_compositor_frame(
            context, "probe", frame.data(), frame.size(), 4, 4);
    });
    foreign_upload.join();
    REQUIRE(foreign_upload_status == SAO_OK);
    CHECK(sao::plugins::loader::sao_plugins_ctx_upload_compositor_frame(
              context, "probe", frame.data(), frame.size(), 2, 8) ==
          SAO_ERR_INVALID_ARGUMENT);
    int32_t foreign_position_status = SAO_OK;
    std::thread foreign_position([&] {
        foreign_position_status =
            sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_position(
                context, "probe", 10, 20);
    });
    foreign_position.join();
    REQUIRE(foreign_position_status == SAO_OK);
    PlatformCompositorProbe compositor_probe;
    compositor_probe.context = context;
    compositor_probe.rebind_on_cursor = true;
    int32_t foreign_input_status = SAO_ERR_NOT_INITIALIZED;
    std::thread foreign_input([&] {
        foreign_input_status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
            context, "probe", &platform_compositor_cursor, &platform_compositor_button,
            &platform_compositor_leave, &platform_compositor_scroll, &compositor_probe);
    });
    foreign_input.join();
    REQUIRE(foreign_input_status == SAO_OK);
    int32_t foreign_visible_status = SAO_ERR_NOT_INITIALIZED;
    std::thread foreign_hide([&] {
        foreign_visible_status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
            context, "probe", false);
    });
    foreign_hide.join();
    REQUIRE(foreign_visible_status == SAO_OK);
    bool hit = true;
    REQUIRE(sao_ui_compositor_hit_test(compositor, 11, 22, &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);
    std::thread foreign_show([&] {
        foreign_visible_status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
            context, "probe", true);
    });
    foreign_show.join();
    REQUIRE(foreign_visible_status == SAO_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 11, 22, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.cursor_calls == 1);
    CHECK(compositor_probe.rebind_status == SAO_OK);
    CHECK(compositor_probe.x == 1.0F);
    CHECK(compositor_probe.y == 2.0F);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 11, 22, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.button_calls == 1);
    CHECK(compositor_probe.button == 0);
    CHECK(compositor_probe.pressed);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 11, 22, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.button_calls == 2);
    CHECK_FALSE(compositor_probe.pressed);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseWheel, 11, 22, -1, 120) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.scroll_calls == 1);
    CHECK(compositor_probe.scroll_y == 1.0F);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 100, 100, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.leave_calls == 1);
    PlatformCompositorProbe replacement_probe;
    replacement_probe.context = context;
    compositor_probe.rebind_target = &replacement_probe;
    compositor_probe.rebind_on_button_release = true;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 11, 22, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 11, 22, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 100, 100, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.rebind_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(compositor_probe.leave_calls == 2);
    CHECK(replacement_probe.button_calls == 0);
    CHECK(replacement_probe.leave_calls == 0);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 11, 22, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.cursor_calls == 3);
    CHECK(replacement_probe.cursor_calls == 0);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 100, 100, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(compositor_probe.leave_calls == 3);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 11, 22, -1, 0) ==
            SAO_STATUS_OK);
    int32_t foreign_hide_status = SAO_ERR_NOT_INITIALIZED;
    int32_t foreign_destroy_status = SAO_ERR_NOT_INITIALIZED;
    std::thread foreign_hide_and_destroy([&] {
        foreign_hide_status = sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
            context, "probe", false);
        foreign_destroy_status =
            sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(context, "probe");
    });
    foreign_hide_and_destroy.join();
    REQUIRE(foreign_hide_status == SAO_OK);
    CHECK(foreign_destroy_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(compositor_layer_count(compositor) == 1);
    PlatformCompositorProbe post_drain_probe;
    post_drain_probe.context = context;
    CHECK(sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
              context, "probe", &platform_compositor_cursor, &platform_compositor_button,
              &platform_compositor_leave, &platform_compositor_scroll, &post_drain_probe) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_position(
                context, "probe", 12, 24) == SAO_OK);
    const sao_status_t owner_tick_status = sao_ui_compositor_tick(compositor);
    CHECK((owner_tick_status == SAO_STATUS_OK ||
           owner_tick_status == SAO_STATUS_ERR_NOT_INITIALIZED));
    CHECK(compositor_probe.leave_calls == 4);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
                context, "probe", &platform_compositor_cursor, &platform_compositor_button,
                &platform_compositor_leave, &platform_compositor_scroll, &post_drain_probe) ==
            SAO_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
                context, "probe", true) == SAO_OK);
    const uint32_t old_cursor_calls = compositor_probe.cursor_calls;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 13, 25, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(post_drain_probe.cursor_calls == 1);
    CHECK(compositor_probe.cursor_calls == old_cursor_calls);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_visible(
                context, "probe", false) == SAO_OK);
    CHECK(post_drain_probe.leave_calls == 1);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(context, "probe") ==
            SAO_OK);
    CHECK(compositor_layer_count(compositor) == 0);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
                context, "cleanup", 2, 2, 0, 0, 1, true, false, 0) == SAO_OK);
    CHECK(compositor_layer_count(compositor) == 1);

    char* timer_token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_interval(
                context, platform_timer_callback, 60.0, nullptr, &timer_token) == SAO_OK);
    REQUIRE(timer_token != nullptr);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_clear_timer(context, timer_token) == SAO_OK);
    sao::plugins::loader::sao_plugins_ctx_free_string(timer_token);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_notify(context, "Provider", "Ready", 0.1,
                                                          "info") == SAO_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_dismiss_notify(context) == SAO_OK);
#else
    char* timer_token = reinterpret_cast<char*>(1);
    CHECK(sao::plugins::loader::sao_plugins_ctx_set_interval(
              context, platform_timer_callback, 60.0, nullptr, &timer_token) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(timer_token == nullptr);
    CHECK(sao::plugins::loader::sao_plugins_ctx_notify(context, "Provider", "Ready", 0.1,
                                                        "info") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
#endif

    CHECK(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    sao::plugins::loader::sao_plugins_ctx_destroy(context);
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    CHECK(compositor_layer_count(compositor) == 0);
#endif
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
#endif
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
TEST_CASE("launcher transfers a half-initialized SDK context to loader cleanup",
          "[launcher][provider][plugins][platform][sdk][cleanup][focused]") {
    temporary_tree tree("platform_sdk_bind_cleanup");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    const auto handle = add_external_plugin("launcher_sdk_bind_cleanup", tree.root());
    const auto live_contexts = sao_sdk_test_live_context_count();
    FailedSdkBindMemoryProvider provider;
    REQUIRE(sao_sdk_platform_memory_configure_provider(&provider.provider) == SAO_SDK_OK);

    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);

    REQUIRE(sao_sdk_platform_memory_configure_provider(nullptr) == SAO_SDK_OK);
    CHECK(context == nullptr);
    CHECK(provider.close_attempts == 3);
    CHECK(sao_sdk_test_live_context_count() == live_contexts);

    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}
#endif

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
TEST_CASE("Entity SDK and plugin layers share one visible compositor HWND",
      "[launcher][provider][plugins][platform][compositor][coexistence][focused]") {
    temporary_tree tree("central_compositor_coexistence");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    SaoOverlayHostConfig host_config{};
    host_config.width = 640;
    host_config.height = 480;
    host_config.title_utf16 = L"SAO central compositor coexistence test";
    sao_ui_overlay_host_handle_t host = nullptr;
    if (sao_ui_overlay_host_create(&host_config, &host) != SAO_STATUS_OK) {
        SKIP("overlay host unavailable in this desktop session");
    }
    sao_ui_compositor_handle_t compositor = nullptr;
    if (sao_ui_compositor_create(host, nullptr, &compositor) != SAO_STATUS_OK) {
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D11/DirectComposition unavailable in this environment");
    }
    REQUIRE(sao_ui_overlay_host_set_visible(host, true) == SAO_STATUS_OK);
    const auto top_level_windows = top_level_process_windows();
    REQUIRE(top_level_windows.size() >= 3);
    REQUIRE(sao_sdk_platform_bind_ui_compositor(compositor) == SAO_SDK_OK);

    SaoUiEntityShellConfig entity_config{};
    sao_ui_entity_shell_handle_t entity = nullptr;
    REQUIRE(sao_ui_entity_shell_create_on_compositor(compositor, &entity_config, &entity) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_bring_online(entity) == SAO_STATUS_OK);
    CHECK(compositor_layer_count(compositor) == 2);
    CHECK(top_level_process_windows() == top_level_windows);

    SaoSdkContext sdk_context{};
    REQUIRE(sao_sdk_bind_context("launcher.central.coexistence", "1.0", &sdk_context) ==
            SAO_SDK_OK);
    SaoSdkPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = "launcher.central.panel";
    descriptor.title_utf8 = "Central Panel";
    descriptor.default_width_px = 240;
    descriptor.default_height_px = 120;
    descriptor.min_width_px = 80;
    descriptor.min_height_px = 60;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = true;
    descriptor.initial_opacity = 1.0F;
    sao_sdk_ui_panel_t panel = nullptr;
    REQUIRE(sao_sdk_register_ui_panel(&sdk_context, &descriptor, &panel) == SAO_SDK_OK);
    CHECK(compositor_layer_count(compositor) == 3);
    CHECK(top_level_process_windows() == top_level_windows);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    const auto handle = add_external_plugin("launcher_central_coexistence", tree.root());
    auto* plugin_context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(plugin_context != nullptr);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
                plugin_context, "plugin_panel", 16, 16, 300, 200, 10, true, false, 0) ==
            SAO_OK);
    CHECK(compositor_layer_count(compositor) == 4);
    CHECK(top_level_process_windows() == top_level_windows);
    void* sdk_compositor = nullptr;
    REQUIRE(sao_sdk_platform_get_ui_compositor(&sdk_compositor) == SAO_SDK_OK);
    CHECK(sdk_compositor == compositor);

    REQUIRE(sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(
                plugin_context, "plugin_panel") == SAO_OK);
    sao::plugins::loader::sao_plugins_ctx_destroy(plugin_context);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
        sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_unregister_ui_panel(&sdk_context, panel) == SAO_SDK_OK);
    REQUIRE(sao_sdk_context_try_destroy(&sdk_context) == SAO_SDK_OK);
    REQUIRE(sao_ui_entity_shell_take_offline(entity) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_try_destroy(entity) == SAO_STATUS_OK);
    CHECK(compositor_layer_count(compositor) == 0);
    CHECK(sao_ui_overlay_host_visible(host));
    CHECK(top_level_process_windows() == top_level_windows);
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}
#endif

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
TEST_CASE("launcher one-shot timeouts release SDK timers without map or worker growth",
          "[launcher][provider][plugins][platform][timer][one-shot][focused]") {
    temporary_tree tree("platform_timeout_stress");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_stress", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    REQUIRE(sao_launcher_test_platform_timer_count() == 0);
    REQUIRE(sao_launcher_test_platform_timer_worker_count() == 0);

    constexpr size_t kBatchSize = 48;
    for (int batch = 0; batch < 2; ++batch) {
        std::vector<one_shot_probe> probes(kBatchSize);
        for (auto& probe : probes) {
            probe.context = context;
            char* token = nullptr;
            REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                        context, one_shot_callback, 0.1, &probe, &token) == SAO_OK);
            publish_timer_token(probe, token);
        }
        REQUIRE(wait_until([&probes] {
            return std::all_of(probes.begin(), probes.end(), [](const one_shot_probe& probe) {
                return probe.callbacks.load(std::memory_order_relaxed) == 1;
            });
        }));
        REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));
        CHECK(std::all_of(probes.begin(), probes.end(), [](const one_shot_probe& probe) {
            return probe.callbacks_before_token.load(std::memory_order_relaxed) == 0;
        }));
        CHECK(sao_launcher_test_platform_timer_worker_count() == 1);
    }

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_worker_count() == 0; }));
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("launcher defers a synchronous registration callback until timer publication",
          "[launcher][provider][plugins][platform][timer][registration][focused]") {
    temporary_tree tree("platform_timeout_sync_register");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_sync_register", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    one_shot_probe probe;
    probe.context = context;
    sao_launcher_test_fire_timer_during_register(true);
    char* token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                context, one_shot_callback, 0.1, &probe, &token) == SAO_OK);
    CHECK(probe.callbacks.load(std::memory_order_relaxed) == 0);
    publish_timer_token(probe, token);
    REQUIRE(wait_until([&probe] {
        return probe.callbacks.load(std::memory_order_relaxed) == 1;
    }));
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));
    CHECK(probe.callbacks_before_token.load(std::memory_order_relaxed) == 0);

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("launcher timer clear and shutdown race deferred cleanup idempotently",
          "[launcher][provider][plugins][platform][timer][concurrency][focused]") {
    temporary_tree tree("platform_timeout_concurrent_cleanup");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_concurrent_cleanup", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    one_shot_probe probe;
    probe.context = context;
    probe.complete_loader_ledger = false;
    probe.block_callback = true;
    char* token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                context, one_shot_callback, 0.02, &probe, &token) == SAO_OK);
    publish_timer_token(probe, token);
    REQUIRE(wait_for_blocked_callback(probe));

    const auto attempts = sao_launcher_test_platform_timer_unregister_attempt_count();
    std::atomic_int32_t clear_status{SAO_ERR_NOT_INITIALIZED};
    std::thread clear_thread([&] {
        clear_status.store(sao::plugins::loader::sao_plugins_ctx_clear_timer(
                               context, probe.token.data()),
                           std::memory_order_release);
    });
    REQUIRE(wait_until([attempts] {
        return sao_launcher_test_platform_timer_unregister_attempt_count() > attempts;
    }));
    CHECK(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    release_blocked_callback(probe);
    clear_thread.join();
    CHECK(clear_status.load(std::memory_order_acquire) == SAO_OK);
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("launcher preserves failed one-shot unregister for teardown retry",
          "[launcher][provider][plugins][platform][timer][retry][focused]") {
    temporary_tree tree("platform_timeout_unregister_retry");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_unregister_retry", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    one_shot_probe probe;
    probe.context = context;
    char* token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                context, one_shot_callback, 0.1, &probe, &token) == SAO_OK);
    publish_timer_token(probe, token);
    const auto attempts = sao_launcher_test_platform_timer_unregister_attempt_count();
    sao_launcher_test_fail_next_timer_unregister(true);
    REQUIRE(wait_until([&probe] {
        return probe.callbacks.load(std::memory_order_relaxed) == 1;
    }));
    REQUIRE(wait_until([attempts] {
        return sao_launcher_test_platform_timer_unregister_attempt_count() > attempts;
    }));
    CHECK(sao_launcher_test_platform_timer_count() == 1);

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_worker_count() == 0; }));
    CHECK(sao_launcher_test_platform_timer_unregister_attempt_count() >= attempts + 2);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}
#endif

TEST_CASE("launcher owns dependency path sessions across shutdown retry",
          "[launcher][provider][plugins][deps][focused]") {
    temporary_tree tree("deps_provider");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    const auto dependency_path = tree.root() / "libs";
    REQUIRE(fs::create_directories(dependency_path));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);

    sao::plugins::loader::deps_bootstrap_record record;
    record.added_paths.push_back(fs::absolute(dependency_path).wstring());
    sao::plugins::loader::deps_session_t session = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_deps_attach(
                "launcher_deps_provider_probe", tree.root().c_str(), &record, &session) == SAO_OK);
    REQUIRE(session != nullptr);

    CHECK(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_deps_session_close(session) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);

    registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("failed second discovery preserves the first registry provider owners",
          "[launcher][provider][plugins][registration][rollback][focused]") {
    temporary_tree tree("provider_registration_rollback");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    const auto dependency_path = tree.root() / "libs";
    REQUIRE(fs::create_directories(dependency_path));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* first = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &first) == SAO_STATUS_OK);
    REQUIRE(first != nullptr);
    sao_plugins_registry* second = nullptr;
    CHECK(sao_plugins_discover(nullptr, &second) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    CHECK(second == nullptr);

    sao::plugins::loader::deps_bootstrap_record record;
    record.added_paths.push_back(fs::absolute(dependency_path).wstring());
    sao::plugins::loader::deps_session_t session = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_deps_attach(
                "launcher_provider_registration_probe", tree.root().c_str(), &record, &session) ==
            SAO_OK);
    REQUIRE(session != nullptr);
    CHECK(sao_plugins_shutdown(first) != SAO_STATUS_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_deps_session_close(session) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(first) == SAO_STATUS_OK);
}

TEST_CASE("provider defaults use only existing packaged roots",
          "[launcher][provider][plugins][focused]") {
    temporary_tree tree("packaged");
    const auto base = tree.root() / "package";
    const auto packaged_plugins = base / "plugins";
    const auto packaged_python_plugins = base / "python" / "plugins";
    REQUIRE(fs::create_directories(packaged_plugins));
    REQUIRE(fs::create_directories(packaged_python_plugins));

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), nullptr) ==
            SAO_STATUS_OK);
    const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    REQUIRE(configuration.enabled);
    REQUIRE(configured_roots() == std::vector<fs::path>{fs::canonical(packaged_plugins),
                                                        fs::canonical(packaged_python_plugins)});
    REQUIRE(configuration.python_home.empty());
}

TEST_CASE("provider defaults derive source roots without current path",
          "[launcher][provider][plugins][focused]") {
    temporary_tree tree("source");
    const auto source = tree.root() / "workspace" / "sao_auto";
    const auto base = source / "C" / "build" / "focused" / "bin";
    const auto top_plugins = source.parent_path() / "plugins";
    const auto source_python_plugins = source / "python" / "plugins";
    const auto unrelated = tree.root() / "unrelated";
    REQUIRE(fs::create_directories(base));
    REQUIRE(fs::create_directories(top_plugins));
    REQUIRE(fs::create_directories(source_python_plugins));
    REQUIRE(fs::create_directories(unrelated / "plugins"));
    write_text(source / "C" / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.28)\n");

    current_path_guard current_path(unrelated);
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), nullptr) ==
            SAO_STATUS_OK);
    const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    REQUIRE(configuration.enabled);
    REQUIRE(configured_roots() == std::vector<fs::path>{fs::canonical(top_plugins),
                                                        fs::canonical(source_python_plugins)});
    REQUIRE(configuration.python_home.empty());
}

TEST_CASE("provider defaults stay disabled when no candidate root exists",
          "[launcher][provider][plugins][focused]") {
    temporary_tree tree("missing");
    const auto base = tree.root() / "empty";
    REQUIRE(fs::create_directories(base));

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), nullptr) ==
            SAO_STATUS_OK);
    const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    REQUIRE_FALSE(configuration.enabled);
    REQUIRE(configuration.roots.empty());
    REQUIRE(configuration.python_home.empty());
}

TEST_CASE("provider explicit native-only manifest does not require a script entry",
          "[launcher][provider][plugins][manifest][focused]") {
    temporary_tree tree("native_manifest");
    const auto base = tree.root() / "package";
    const auto plugin = base / "native_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "native_fixture.dll", "fixture");
    write_text(
        manifest_path,
        R"({"id":"provider_native_only","enabled":false,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","abi_version":2})");

    nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.struct_size == sizeof(status));
    CHECK(status.python_runtime_status == kPythonNoHomeStatus);
    CHECK(status.discovered_count == 1);

    constexpr std::size_t prefix_size =
        offsetof(sao_plugins_status_snapshot_t, operational_status) +
        sizeof(status.operational_status);
    sao_plugins_status_snapshot_t prefix{};
    std::memset(reinterpret_cast<std::byte*>(&prefix) + prefix_size, 0x5a,
                sizeof(prefix) - prefix_size);
    prefix.struct_size = static_cast<std::uint32_t>(prefix_size);
    REQUIRE(sao_plugins_status_snapshot(registry, &prefix) == SAO_STATUS_OK);
    CHECK(prefix.struct_size == prefix_size);
    CHECK(prefix.python_runtime_status == kPythonNoHomeStatus);
    CHECK(prefix.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    const auto* suffix = reinterpret_cast<const unsigned char*>(&prefix) + prefix_size;
    CHECK(std::all_of(suffix, reinterpret_cast<const unsigned char*>(&prefix) + sizeof(prefix),
                      [](unsigned char value) { return value == 0x5a; }));
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);

    registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("unavailable optional Python runtime does not block native discovery",
          "[launcher][provider][plugins][runtime][focused]") {
    temporary_tree tree("unavailable_python");
    const auto base = tree.root() / "package";
    const auto plugin = base / "native_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "native_fixture.dll", "fixture");
    write_text(
        manifest_path,
        R"({"id":"provider_native_without_python","enabled":false,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","abi_version":2})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(manifest_path)})},
          {"python_home", path_utf8(base / "missing_python_runtime")}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.python_runtime_status == kPythonMissingHomeStatus);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("enabled Python without runtime is deferred and visible as degraded",
          "[launcher][provider][plugins][adapter][degraded][focused]") {
    temporary_tree tree("missing_adapter");
    const auto base = tree.root() / "package";
    const auto plugin = base / "python_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.py", "def on_load(ctx):\n    return True\n");
    write_text(
        manifest_path,
        R"({"id":"provider_python_without_home","language":"python","entry":"plugin.py","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    REQUIRE(sao::launcher::launcherProviderConfigurationSnapshot().plugins.python_home.empty());
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.python_runtime_status == kPythonNoHomeStatus);
    CHECK(status.python_launch_strategy == SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    CHECK(status.deferred_count == 1);
    CHECK(status.loaded_count == 0);
    CHECK(status.enabled_count == 0);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("Python degradation propagates through disabled dependency nodes",
          "[launcher][provider][plugins][dependencies][degraded][focused]") {
    temporary_tree tree("python_disabled_dependency_closure");
    const auto base = tree.root() / "package";
    const auto python = base / "python_base";
    const auto bridge = base / "disabled_bridge";
    const auto leaf = base / "enabled_leaf";
    write_text(python / "plugin.py", "def on_load(ctx):\n    return True\n");
    write_text(python / "plugin.json",
               R"({"id":"python_base","language":"python","entry":"plugin.py","enabled":false})");
    write_text(bridge / "bridge.dll", "fixture");
    write_text(
        bridge / "plugin.json",
        R"({"id":"disabled_bridge","enabled":false,"native_entry":"bridge.dll","native_abi":"sao_plugin_v2","abi_version":2,"requires":["python_base"]})");
    write_text(leaf / "leaf.dll", "fixture");
    write_text(
        leaf / "plugin.json",
        R"({"id":"enabled_leaf","enabled":true,"native_entry":"leaf.dll","native_abi":"sao_plugin_v2","abi_version":2,"requires":["disabled_bridge"]})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(python / "plugin.json"),
                                               path_utf8(bridge / "plugin.json"),
                                               path_utf8(leaf / "plugin.json")})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.discovered_count == 3);
    CHECK(status.deferred_count == 1);
    CHECK(status.loaded_count == 0);
    CHECK(status.enabled_count == 0);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
struct lifecycle_reentry_probe {
    sao_plugins_registry* registry = nullptr;
    bool entered = false;
    sao_status_t status_status = SAO_STATUS_OK;
    sao_status_t reload_status = SAO_STATUS_OK;
    sao_status_t shutdown_status = SAO_STATUS_OK;
};

void lifecycle_reentry_callback(sao::plugins::loader::plugin_handle_t,
                                sao::plugins::loader::lifecycle_event, const char*,
                                void* user_data) {
    auto& probe = *static_cast<lifecycle_reentry_probe*>(user_data);
    if (probe.entered)
        return;
    probe.entered = true;
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    probe.status_status = sao_plugins_status_snapshot(probe.registry, &status);
    probe.reload_status = sao_plugins_reload_all(probe.registry);
    probe.shutdown_status = sao_plugins_shutdown(probe.registry);
}

TEST_CASE("provider lifecycle subscribers get BUSY on operation reentry",
          "[launcher][provider][plugins][lifecycle][reentry][focused]") {
    temporary_tree tree("emma_lifecycle_reentry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    write_text(plugin / "plugin.emma", "fn on_load(ctx)\n    return true\nend\n"
                                       "fn on_enable()\n    return true\nend\n"
                                       "fn on_disable()\n    return true\nend\n"
                                       "fn on_unload()\n    return true\nend\n");
    write_text(
        plugin / "plugin.json",
        R"({"id":"provider_emma_reentry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(plugin / "plugin.json")})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    lifecycle_reentry_probe probe{registry};
    std::uint32_t token = 0;
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_subscribe(&lifecycle_reentry_callback,
                                                                  &probe, &token) == SAO_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_unsubscribe(token) == SAO_OK);
    REQUIRE(probe.entered);
    CHECK(probe.status_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.reload_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.shutdown_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
TEST_CASE("real compositor callbacks reject reentrant unload before lifecycle mutation",
          "[launcher][provider][plugins][platform][compositor][lifecycle][reentry][focused]") {
    temporary_tree tree("emma_compositor_unload_reentry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               "fn on_load(ctx)\n    return true\nend\n"
               "fn on_enable()\n    return true\nend\n"
               "fn on_disable()\n    return true\nend\n"
               "fn on_unload()\n    return true\nend\n");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_compositor_reentry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_platform_bind_ui_compositor(compositor) == SAO_SDK_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    const auto handle = sao::plugins::loader::sao_plugins_registry_find(
        sao::plugins::loader::sao_plugins_registry_instance(),
        "provider_emma_compositor_reentry");
    REQUIRE(handle != nullptr);
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
            sao::plugins::loader::lifecycle_state::loaded_active);
    sao::plugins::loader::plugin_context_t* context = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    REQUIRE(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
                context, "callback_gate", 8, 8, 10, 20, 1, false, false, 0) == SAO_OK);
    const std::array<uint8_t, 8U * 8U * 4U> frame = [] {
        std::array<uint8_t, 8U * 8U * 4U> pixels{};
        pixels.fill(255);
        return pixels;
    }();
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_upload_compositor_frame(
                context, "callback_gate", frame.data(), frame.size(), 8, 8) == SAO_OK);
    CallbackUnloadProbe probe{handle, context};
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_compositor_layer_input(
                context, "callback_gate", &callback_unload_cursor, &callback_unload_button,
                &callback_unload_leave, &callback_unload_scroll, &probe) == SAO_OK);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 12, 22, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 12, 22, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 12, 22, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseWheel, 12, 22, -1, 120) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 100, 100, -1, 0) ==
            SAO_STATUS_OK);

    CHECK(probe.cursor.calls == 1);
    CHECK(probe.button.calls == 2);
    CHECK(probe.leave.calls == 1);
    CHECK(probe.scroll.calls == 1);
    CHECK(probe.callback_destroy_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(compositor_layer_count(compositor) == 1);
    const std::array<const CallbackUnloadObservation*, 4> observations = {
        &probe.cursor, &probe.button, &probe.leave, &probe.scroll};
    for (const auto* observation : observations) {
        CHECK(observation->all_busy);
        CHECK(observation->all_states_preserved);
        CHECK(observation->stop_unchanged);
    }
    CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
          sao::plugins::loader::lifecycle_state::loaded_active);
    CHECK_FALSE(sao::plugins::loader::sao_plugins_ctx_should_stop(context));

    REQUIRE(sao::plugins::loader::sao_plugins_ctx_destroy_compositor_layer(context,
                                                                           "callback_gate") ==
            SAO_OK);
    CHECK(compositor_layer_count(compositor) == 0);
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    REQUIRE(sao_sdk_platform_unbind_ui_compositor() == SAO_SDK_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}
#endif

TEST_CASE("provider repeat discover and shutdown releases Emma adapter ownership",
          "[launcher][provider][plugins][emma][focused]") {
    temporary_tree tree("emma_repeat");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               R"EMMA(fn on_load(ctx)
    return true
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    return true
end
)EMMA");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_repeat","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    for (int iteration = 0; iteration < 2; ++iteration) {
        sao_plugins_registry* registry = nullptr;
        REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
        REQUIRE(registry != nullptr);
        REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
        REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    }
}

TEST_CASE("provider preserves BUSY cleanup for retry",
          "[launcher][provider][plugins][cleanup_retry][focused]") {
    temporary_tree tree("emma_busy_retry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               R"EMMA(let unload_attempts = 0
fn on_load(ctx)
    return true
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    unload_attempts = unload_attempts + 1
    return unload_attempts > 1
end
)EMMA");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_busy_retry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    const auto handle = sao::plugins::loader::sao_plugins_registry_find(
        sao::plugins::loader::sao_plugins_registry_instance(), "provider_emma_busy_retry");
    REQUIRE(handle != nullptr);
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
            sao::plugins::loader::lifecycle_state::loaded_active);
    REQUIRE(sao_plugins_reload_all(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
          sao::plugins::loader::lifecycle_state::loaded_active);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    CHECK(status.last_operation_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(status.last_rollback_status == SAO_STATUS_OK);
    CHECK(status.rollback_attempted == 1);
    CHECK(status.rollback_succeeded == 1);
    CHECK(status.loaded_count == 1);
    CHECK(status.enabled_count == 1);
    REQUIRE(sao_plugins_reload_all(registry) == SAO_STATUS_OK);
    CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
          sao::plugins::loader::lifecycle_state::loaded_active);
    REQUIRE(sao_plugins_shutdown(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider reload restores a multi-node dependency graph exactly",
          "[launcher][provider][plugins][reload][dependencies][rollback][focused]") {
    temporary_tree tree("emma_dependency_rollback");
    const auto base = tree.root() / "package";
    const auto make_plugin = [&](const char* id, const char* requires_json,
                                 const char* unload_body) {
        const auto plugin = base / id;
        write_text(plugin / "plugin.emma", std::string{"fn on_load(ctx)\n    return true\nend\n"
                                                       "fn on_enable()\n    return true\nend\n"
                                                       "fn on_disable()\n    return true\nend\n"} +
                                               unload_body + "\n");
        write_text(plugin / "plugin.json", std::string{"{\"id\":\""} + id +
                                               "\",\"language\":\"emma\",\"entry\":\"plugin.emma\","
                                               "\"enabled\":true,\"requires\":" +
                                               requires_json + "}");
        return plugin / "plugin.json";
    };
    const auto base_manifest = make_plugin(
        "dep_base", "[]",
        "let unload_attempts = 0\nfn on_unload()\n    unload_attempts = unload_attempts + 1\n"
        "    return unload_attempts > 1\nend");
    const auto mid_manifest =
        make_plugin("dep_mid", "[\"dep_base\"]", "fn on_unload()\n    return true\nend");
    const auto leaf_manifest =
        make_plugin("dep_leaf", "[\"dep_mid\"]", "fn on_unload()\n    return true\nend");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(base_manifest), path_utf8(mid_manifest),
                                               path_utf8(leaf_manifest)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    const auto global = sao::plugins::loader::sao_plugins_registry_instance();
    const auto base_handle = sao::plugins::loader::sao_plugins_registry_find(global, "dep_base");
    const auto mid_handle = sao::plugins::loader::sao_plugins_registry_find(global, "dep_mid");
    const auto leaf_handle = sao::plugins::loader::sao_plugins_registry_find(global, "dep_leaf");
    REQUIRE(base_handle != nullptr);
    REQUIRE(mid_handle != nullptr);
    REQUIRE(leaf_handle != nullptr);

    REQUIRE(sao_plugins_reload_all(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    for (const auto handle : {base_handle, mid_handle, leaf_handle}) {
        CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
              sao::plugins::loader::lifecycle_state::loaded_active);
    }
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    CHECK(status.last_operation_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(status.last_rollback_status == SAO_STATUS_OK);
    CHECK(status.rollback_attempted == 1);
    CHECK(status.rollback_succeeded == 1);
    CHECK(status.loaded_count == 3);
    CHECK(status.enabled_count == 3);

    REQUIRE(sao_plugins_reload_all(registry) == SAO_STATUS_OK);
    for (const auto handle : {base_handle, mid_handle, leaf_handle}) {
        CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
              sao::plugins::loader::lifecycle_state::loaded_active);
    }
    REQUIRE(sao_plugins_shutdown(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider reload rollback failure publishes degraded internal",
          "[launcher][provider][plugins][reload][rollback][degraded][focused]") {
    temporary_tree tree("emma_reload_degraded");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma", "fn on_load(ctx)\n    return true\nend\n"
                                       "fn on_enable()\n    return true\nend\n"
                                       "fn on_disable()\n    return true\nend\n"
                                       "fn on_unload()\n    return true\nend\n");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_degraded","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    write_text(plugin / "plugin.emma",
               "fn on_load(ctx)\n    missing_function()\n    return true\nend\n");

    CHECK(sao_plugins_reload_all(registry) == SAO_STATUS_INTERNAL);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_DEGRADED);
    CHECK(status.last_operation_status != SAO_STATUS_OK);
    CHECK(status.last_rollback_status != SAO_STATUS_OK);
    CHECK(status.rollback_attempted == 1);
    CHECK(status.rollback_succeeded == 0);
    CHECK(status.loaded_count == 0);
    CHECK(status.enabled_count == 0);
    CHECK(sao_plugins_reload_all(registry) == SAO_STATUS_INTERNAL);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider stable shell serializes reload status and shutdown",
          "[launcher][provider][plugins][concurrency][shutdown][focused]") {
    temporary_tree tree("stable_shell_concurrency");
    const auto base = tree.root() / "package";
    const auto plugin = base / "native_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "native_fixture.dll", "fixture");
    write_text(
        manifest_path,
        R"({"id":"provider_concurrent_native","enabled":false,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","abi_version":2})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);

    std::atomic_bool start{false};
    std::atomic_uint32_t unexpected{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 100; ++iteration) {
                sao_plugins_status_snapshot_t status{};
                status.struct_size = sizeof(status);
                const auto result = sao_plugins_status_snapshot(registry, &status);
                if (result != SAO_STATUS_OK && result != SAO_STATUS_INTERNAL) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int iteration = 0; iteration < 50; ++iteration) {
            const auto result = sao_plugins_reload_all(registry);
            if (result != SAO_STATUS_OK && result != SAO_STATUS_INTERNAL) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    start.store(true, std::memory_order_release);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(unexpected.load(std::memory_order_relaxed) == 0);
    CHECK(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    CHECK(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_INTERNAL);
}

TEST_CASE("provider retries failed resident runtime cleanup",
          "[launcher][provider][plugins][cleanup_retry][failed][focused]") {
    temporary_tree tree("emma_failed_retry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               R"EMMA(let unload_attempts = 0
fn on_load(ctx)
    return true
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    unload_attempts = unload_attempts + 1
    if unload_attempts == 1
        missing_function()
    end
    return true
end
)EMMA");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_failed_retry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}
#endif
