#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sao/core/status.h"
#include "sao/ui/alerts.h"
#include "sao/ui/compositor.h"
#include "sao/ui/d3d11_device.h"
#include "sao/ui/dc_mutation.h"
#include "sao/ui/dcomp_bridge.h"
#include "sao/ui/dxgi_dup.h"
#include "sao/ui/gpu_overlay_window.h"
#include "sao/ui/input_router.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/z_order.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_dc_mut_test_last_op(
    sao_ui_dc_mutation_coordinator_handle_t handle, char* out_op, size_t out_op_cap,
    char* out_method, size_t out_method_cap, char* out_args, size_t out_args_cap);
extern "C" SAO_UI_API uint32_t SAO_UI_CALL sao_ui_gpu_overlay_test_pump_trace_remaining(void);
extern "C" SAO_UI_API uint64_t SAO_UI_CALL sao_ui_gpu_overlay_test_pump_trace_emitted(void);
extern "C" SAO_UI_API void SAO_UI_CALL
sao_ui_gpu_overlay_test_set_create_failure_point(int32_t point);

namespace {

SaoGpuOverlayWindowConfig gpu_window_config() {
    SaoGpuOverlayWindowConfig config{};
    config.x = 10;
    config.y = 20;
    config.width = 320;
    config.height = 180;
    config.click_through = true;
    config.title_utf8 = "state_completion";
    config.z_order = 100;
    return config;
}

struct ReentrantHotkeyLog {
    sao_ui_input_router_deep_handle_t router = nullptr;
    sao_ui_hotkey_binding_t binding = 0;
    std::atomic<uint32_t> calls{0};
};

void SAO_UI_CALL unregistering_hotkey_callback(const char*, const SaoUiInputEvent*,
                                               void* user_data) {
    auto* log = static_cast<ReentrantHotkeyLog*>(user_data);
    ++log->calls;
    CHECK(sao_ui_input_router_unregister_hotkey(log->router, log->binding) == SAO_STATUS_OK);
}

void SAO_UI_CALL count_gpu_render_tick(void*, float, void* user_data) {
    ++*static_cast<std::atomic<uint32_t>*>(user_data);
}

struct BlockingGpuCallback {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::atomic<bool> returned{false};
};

void SAO_UI_CALL blocking_gpu_render_tick(void*, float, void* user_data) {
    auto* callback = static_cast<BlockingGpuCallback*>(user_data);
    std::unique_lock<std::mutex> lock(callback->mutex);
    callback->entered = true;
    callback->cv.notify_all();
    callback->cv.wait(lock, [callback] { return callback->release; });
    callback->returned.store(true, std::memory_order_release);
}

struct ReentrantGpuCallback {
    sao_ui_gpu_overlay_window_handle_t window = nullptr;
    std::atomic<uint32_t> calls{0};
    std::atomic<bool> destroy_returned{false};
    std::atomic<sao_status_t> stale_status{SAO_STATUS_OK};
};

void SAO_UI_CALL destroying_gpu_render_tick(void*, float, void* user_data) {
    auto* callback = static_cast<ReentrantGpuCallback*>(user_data);
    ++callback->calls;
    sao_ui_gpu_overlay_window_destroy(callback->window);
    callback->destroy_returned.store(true, std::memory_order_release);
    callback->stale_status.store(sao_ui_gpu_overlay_window_show(callback->window),
                                 std::memory_order_release);
}

#if defined(_WIN32)

sao_status_t SAO_UI_CALL accept_physical_exstyle(void*, void*, uint32_t, uint32_t) {
    return SAO_STATUS_OK;
}

struct HiddenWindow {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    std::wstring class_name = L"SaoUiStateCompletionWindow";
    ATOM atom = 0;
    HWND hwnd = nullptr;

    HiddenWindow() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = ::DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name.c_str();
        atom = ::RegisterClassExW(&window_class);
        hwnd = ::CreateWindowExW(0, class_name.c_str(), L"state completion", WS_POPUP, 0, 0, 64, 64,
                                 nullptr, nullptr, instance, nullptr);
    }

    ~HiddenWindow() {
        if (hwnd != nullptr)
            ::DestroyWindow(hwnd);
        if (atom != 0)
            ::UnregisterClassW(class_name.c_str(), instance);
    }
};

bool drain_dc_mutations_with_message_pump(sao_ui_dc_mutation_coordinator_handle_t coordinator,
                                          std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        SaoDcMutationStats stats{};
        if (sao_ui_dc_mutation_coordinator_stats(coordinator, &stats) == SAO_STATUS_OK &&
            stats.inflight_operations == 0 && stats.queued_operations == 0) {
            return true;
        }
        (void)::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return false;
}

#endif

} // namespace

TEST_CASE("gpu overlay window delegates state to compositor layer",
          "[ui][completion][gpu_overlay][state]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_host(compositor) == nullptr);
    CHECK(sao_ui_compositor_host_hwnd(compositor) == nullptr);

    const SaoGpuOverlayWindowConfig config = gpu_window_config();
    sao_ui_gpu_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &config, &window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_show(window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_set_geometry(window, 30, 40, 640, 360) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_set_click_through(window, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_set_alpha(window, 0.5F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_set_z(window, 250) == SAO_STATUS_OK);

    SaoGpuOverlayWindowState state{};
    REQUIRE(sao_ui_gpu_overlay_window_get_state(window, &state) == SAO_STATUS_OK);
    CHECK(state.x == 30);
    CHECK(state.y == 40);
    CHECK(state.width == 640);
    CHECK(state.height == 360);
    CHECK(state.z_order == 250);
    CHECK(state.alpha == 0.5F);
    CHECK(state.visible);
    CHECK_FALSE(state.click_through);
    CHECK(sao_ui_gpu_overlay_window_layer(window) != nullptr);
    CHECK(sao_ui_gpu_overlay_window_gl_ctx(window) == nullptr);

    sao_ui_gpu_overlay_window_destroy(window);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("gpu overlay pump trace consumes exactly eight real present ticks",
          "[ui][completion][gpu_overlay][pump_trace]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);

    std::atomic<uint32_t> first_callbacks{0};
    std::atomic<uint32_t> second_callbacks{0};
    SaoGpuOverlayWindowConfig first_config = gpu_window_config();
    first_config.title_utf8 = "pump_trace_first";
    first_config.render_fn = reinterpret_cast<void*>(&count_gpu_render_tick);
    first_config.render_fn_user_data = &first_callbacks;
    SaoGpuOverlayWindowConfig second_config = first_config;
    second_config.title_utf8 = "pump_trace_second";
    second_config.render_fn_user_data = &second_callbacks;

    sao_ui_gpu_overlay_window_handle_t first = nullptr;
    sao_ui_gpu_overlay_window_handle_t second = nullptr;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &first_config, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &second_config, &second) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_show(first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_show(second) == SAO_STATUS_OK);

    const uint64_t emitted_before = sao_ui_gpu_overlay_test_pump_trace_emitted();
    sao_ui_arm_pump_trace();
    CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 8u);
    SaoGpuOverlayWindowState state{};
    REQUIRE(sao_ui_gpu_overlay_window_get_state(first, &state) == SAO_STATUS_OK);
    CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 8u);

    for (uint32_t tick = 0; tick < 8; ++tick) {
        CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
        CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 7u - tick);
    }
    CHECK(sao_ui_gpu_overlay_test_pump_trace_emitted() == emitted_before + 8u);
    CHECK(first_callbacks.load() == 8u);
    CHECK(second_callbacks.load() == 8u);

    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 0u);
    CHECK(sao_ui_gpu_overlay_test_pump_trace_emitted() == emitted_before + 8u);
    CHECK(first_callbacks.load() == 9u);
    CHECK(second_callbacks.load() == 9u);

    sao_ui_arm_pump_trace();
    REQUIRE(sao_ui_gpu_overlay_test_pump_trace_remaining() == 8u);
    sao_ui_gpu_overlay_window_destroy(first);
    first = nullptr;
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 7u);
    sao_ui_arm_pump_trace();
    CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 8u);
    for (uint32_t tick = 0; tick < 8; ++tick) {
        CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    }
    CHECK(sao_ui_gpu_overlay_test_pump_trace_remaining() == 0u);

    sao_ui_gpu_overlay_window_destroy(second);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("gpu overlay destroy waits for in-flight callback user data",
          "[ui][completion][gpu_overlay][lifecycle]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);

    BlockingGpuCallback callback{};
    SaoGpuOverlayWindowConfig config = gpu_window_config();
    config.title_utf8 = "blocking_callback";
    config.render_fn = reinterpret_cast<void*>(&blocking_gpu_render_tick);
    config.render_fn_user_data = &callback;
    sao_ui_gpu_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &config, &window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_show(window) == SAO_STATUS_OK);

    std::atomic<bool> destroy_started{false};
    std::atomic<bool> destroy_returned{false};
    std::atomic<bool> destroy_was_blocked{false};
    std::thread destroyer([&] {
        {
            std::unique_lock<std::mutex> lock(callback.mutex);
            callback.cv.wait(lock, [&callback] { return callback.entered; });
        }
        destroy_started.store(true, std::memory_order_release);
        sao_ui_gpu_overlay_window_destroy(window);
        destroy_returned.store(true, std::memory_order_release);
    });
    std::thread releaser([&] {
        while (!destroy_started.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        destroy_was_blocked.store(!destroy_returned.load(std::memory_order_acquire),
                                  std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(callback.mutex);
            callback.release = true;
        }
        callback.cv.notify_all();
    });

    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    destroyer.join();
    releaser.join();
    CHECK(destroy_was_blocked.load(std::memory_order_acquire));
    CHECK(callback.returned.load(std::memory_order_acquire));
    CHECK(destroy_returned.load(std::memory_order_acquire));
    CHECK(sao_ui_gpu_overlay_window_show(window) == SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("gpu overlay callback can destroy itself without deadlock",
          "[ui][completion][gpu_overlay][reentry]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);

    ReentrantGpuCallback callback{};
    SaoGpuOverlayWindowConfig config = gpu_window_config();
    config.title_utf8 = "reentrant_destroy";
    config.render_fn = reinterpret_cast<void*>(&destroying_gpu_render_tick);
    config.render_fn_user_data = &callback;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &config, &callback.window) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_show(callback.window) == SAO_STATUS_OK);

    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(callback.calls.load() == 1u);
    CHECK(callback.destroy_returned.load(std::memory_order_acquire));
    CHECK(callback.stale_status.load(std::memory_order_acquire) == SAO_STATUS_ERR_HANDLE_INVALID);
    size_t layer_count = 1;
    REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &layer_count) == SAO_STATUS_OK);
    CHECK(layer_count == 0u);

    sao_ui_gpu_overlay_window_destroy(callback.window);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("gpu overlay visibility keeps callback binding and layer aligned",
          "[ui][completion][gpu_overlay][visibility]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);

    std::atomic<uint32_t> callbacks{0};
    SaoGpuOverlayWindowConfig config = gpu_window_config();
    config.title_utf8 = "visibility_consistency";
    config.render_fn = reinterpret_cast<void*>(&count_gpu_render_tick);
    config.render_fn_user_data = &callbacks;
    sao_ui_gpu_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &config, &window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_show(window) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(callbacks.load() == 1u);

    REQUIRE(sao_ui_gpu_overlay_window_hide(window) == SAO_STATUS_OK);
    SaoGpuOverlayWindowState state{};
    REQUIRE(sao_ui_gpu_overlay_window_get_state(window, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.visible);
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(callbacks.load() == 1u);

    REQUIRE(sao_ui_gpu_overlay_window_show(window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_gpu_overlay_window_get_state(window, &state) == SAO_STATUS_OK);
    CHECK(state.visible);
    CHECK(sao_ui_compositor_present(compositor) == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(callbacks.load() == 2u);

    sao_ui_gpu_overlay_window_destroy(window);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("gpu overlay concurrent APIs reject a retired stable handle",
          "[ui][completion][gpu_overlay][concurrency]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    SaoGpuOverlayWindowConfig config = gpu_window_config();
    config.title_utf8 = "concurrent_destroy";
    sao_ui_gpu_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &config, &window) == SAO_STATUS_OK);

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> unexpected_statuses{0};
    std::vector<std::thread> callers;
    for (int32_t index = 0; index < 6; ++index) {
        callers.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (!stop.load(std::memory_order_acquire)) {
                const sao_status_t status = index % 3 == 0 ? sao_ui_gpu_overlay_window_show(window)
                                            : index % 3 == 1
                                                ? sao_ui_gpu_overlay_window_hide(window)
                                                : sao_ui_gpu_overlay_window_set_alpha(window, 0.5F);
                if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_HANDLE_INVALID)
                    ++unexpected_statuses;
                SaoGpuOverlayWindowState state{};
                const sao_status_t get_status = sao_ui_gpu_overlay_window_get_state(window, &state);
                if (get_status != SAO_STATUS_OK && get_status != SAO_STATUS_ERR_HANDLE_INVALID)
                    ++unexpected_statuses;
            }
        });
    }
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sao_ui_gpu_overlay_window_destroy(window);
    stop.store(true, std::memory_order_release);
    for (auto& caller : callers)
        caller.join();

    CHECK(unexpected_statuses.load() == 0u);
    CHECK(sao_ui_gpu_overlay_window_show(window) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_gpu_overlay_window_layer(window) == nullptr);
    CHECK(sao_ui_gpu_overlay_window_hwnd(window) == nullptr);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("gpu overlay create failures roll back every admitted layer",
          "[ui][completion][gpu_overlay][rollback]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    const SaoGpuOverlayWindowConfig config = gpu_window_config();

    for (int32_t failure_point = 1; failure_point <= 4; ++failure_point) {
        sao_ui_gpu_overlay_test_set_create_failure_point(failure_point);
        sao_ui_gpu_overlay_window_handle_t window =
            reinterpret_cast<sao_ui_gpu_overlay_window_handle_t>(static_cast<uintptr_t>(1));
        const sao_status_t status = sao_ui_gpu_overlay_window_create(compositor, &config, &window);
        sao_ui_gpu_overlay_test_set_create_failure_point(0);
        CHECK(status == SAO_STATUS_ERR_UNKNOWN);
        CHECK(window == nullptr);
        size_t layer_count = 1;
        REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &layer_count) ==
                SAO_STATUS_OK);
        CHECK(layer_count == 0u);
    }
    sao_ui_gpu_overlay_window_handle_t recovered = nullptr;
    REQUIRE(sao_ui_gpu_overlay_window_create(compositor, &config, &recovered) == SAO_STATUS_OK);
    sao_ui_gpu_overlay_window_destroy(recovered);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("input router dispatches subset hotkey outside its lock",
          "[ui][completion][input_router]") {
    sao_ui_input_router_deep_handle_t router = nullptr;
    REQUIRE(sao_ui_input_router_deep_create(nullptr, &router) == SAO_STATUS_OK);

    ReentrantHotkeyLog log{};
    log.router = router;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = "plain_f5";
    spec.virtual_key = 0x74;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    spec.prevent_default = true;
    REQUIRE(sao_ui_input_router_register_hotkey(router, "core", &spec,
                                                &unregistering_hotkey_callback, &log,
                                                &log.binding) == SAO_STATUS_OK);

    SaoUiInputEvent event{};
    event.kind = SAO_UI_INPUT_KEY_DOWN;
    event.virtual_key = 0x74;
    event.modifiers = SAO_UI_MOD_CTRL_BIT;
    bool consumed = false;
    REQUIRE(sao_ui_input_router_route_event(router, &event, &consumed) == SAO_STATUS_OK);
    CHECK(consumed);
    CHECK(log.calls.load() == 1);

#if defined(_WIN32)
    CHECK(sao_ui_input_router_feed_raw_win32(router, WM_NULL, 0, 0, &consumed) ==
          SAO_STATUS_ERR_NOT_FOUND);
#endif
    sao_ui_input_router_deep_destroy(router);
}

TEST_CASE("alert drain text remains valid until banner hide", "[ui][completion][alerts]") {
    const uint16_t first[] = {'f', 'i', 'r', 's', 't', 0};
    const uint16_t second[] = {'s', 'e', 'c', 'o', 'n', 'd', 0};
    uint64_t first_id = 0;
    uint64_t second_id = 0;
    REQUIRE(sao_ui_alerts_banner_show(first, 1000, 1, &first_id) == SAO_STATUS_OK);
    SaoUiAlertBanner drained{};
    size_t count = 0;
    REQUIRE(sao_ui_alerts_banner_drain(&drained, 1, &count) == SAO_STATUS_OK);
    REQUIRE(count == 1);
    REQUIRE(drained.text_utf16 != nullptr);
    REQUIRE(sao_ui_alerts_banner_show(second, 1000, 2, &second_id) == SAO_STATUS_OK);
    CHECK(drained.text_len == 5);
    CHECK(drained.text_utf16[0] == 'f');
    CHECK(drained.text_utf16[4] == 't');
    REQUIRE(sao_ui_alerts_banner_hide(first_id) == SAO_STATUS_OK);
    REQUIRE(sao_ui_alerts_banner_hide(second_id) == SAO_STATUS_OK);
}

TEST_CASE("graphics state snapshots zero invalid outputs", "[ui][completion][graphics][state]") {
    SaoD3d11DeviceState d3d_state{};
    d3d_state.feature_level = 0xFFFFFFFFu;
    CHECK(sao_ui_d3d11_device_get_state(nullptr, &d3d_state) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(d3d_state.feature_level == 0u);

    SaoDcompBridgeState dcomp_state{};
    dcomp_state.width = 99;
    CHECK(sao_ui_dcomp_bridge_get_state(nullptr, &dcomp_state) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(dcomp_state.width == 0u);

    SaoDxgiDupState dxgi_state{};
    dxgi_state.output_index = 99;
    CHECK(sao_ui_dxgi_dup_get_state(nullptr, &dxgi_state) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(dxgi_state.output_index == 0u);
}

#if defined(_WIN32)

TEST_CASE("overlay z order submits configured exstyle mutation",
          "[ui][completion][overlay_host][z_order]") {
    SaoUiDcMutationProviderV2 provider{};
    provider.struct_size = sizeof(provider);
    provider.hide_exstyle = &accept_physical_exstyle;
    sao_ui_dc_mutation_coordinator_handle_t coordinator = nullptr;
    REQUIRE(sao_ui_dc_mutation_coordinator_create_ex_v2(&provider, &coordinator) ==
            SAO_STATUS_OK);
    SaoOverlayHostConfig config{};
    config.width = 320;
    config.height = 180;
    config.dc_mutation_coordinator = coordinator;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&config, &host) == SAO_STATUS_OK);
    CHECK(sao_ui_overlay_host_dc_mutation_coordinator(host) == coordinator);

    sao_ui_z_order_manager_handle_t z_order = nullptr;
    REQUIRE(sao_ui_z_order_manager_create(host, &z_order) == SAO_STATUS_OK);
    REQUIRE(sao_ui_z_order_hide_exstyle_mask(z_order, SAO_UI_WS_EX_TOPMOST |
                                                          SAO_UI_WS_EX_LAYERED) == SAO_STATUS_OK);
    REQUIRE(drain_dc_mutations_with_message_pump(coordinator, std::chrono::seconds(1)));
    char operation[64]{};
    char method[64]{};
    char args[128]{};
    REQUIRE(sao_ui_dc_mut_test_last_op(coordinator, operation, sizeof(operation), method,
                                       sizeof(method), args, sizeof(args)));
    CHECK(std::string(operation) == "host-exstyle");
    CHECK(std::string(method) == "hide_exstyle");
    CHECK(std::string(args).find("mask") != std::string::npos);

    sao_ui_z_order_manager_destroy(z_order);
    REQUIRE(sao_ui_overlay_host_destroy(host));
    sao_ui_dc_mutation_coordinator_destroy(coordinator);
}

TEST_CASE("D3D11 and DComp snapshots track live resources", "[ui][completion][graphics][state]") {
    SaoD3d11DeviceConfig device_config{};
    device_config.prefer_warp = true;
    sao_ui_d3d11_device_handle_t device = nullptr;
    if (sao_ui_d3d11_device_create(&device_config, &device) != SAO_STATUS_OK) {
        SKIP("D3D11 WARP unavailable");
    }
    SaoD3d11DeviceState device_state{};
    REQUIRE(sao_ui_d3d11_device_get_state(device, &device_state) == SAO_STATUS_OK);
    CHECK(device_state.device_ready);
    CHECK(device_state.context_ready);
    CHECK(device_state.factory_ready);
    CHECK(device_state.adapter_ready);
    CHECK(device_state.feature_level != 0u);
    CHECK(device_state.owner_thread_id == ::GetCurrentThreadId());

    HiddenWindow window;
    if (window.hwnd == nullptr) {
        sao_ui_d3d11_device_destroy(device);
        SKIP("window station unavailable");
    }
    SaoDcompBridgeConfig bridge_config{};
    bridge_config.hwnd = window.hwnd;
    bridge_config.d3d11_device = sao_ui_d3d11_device_ptr(device);
    bridge_config.alpha_mode = 1;
    bridge_config.buffer_count = 2;
    bridge_config.width = 2;
    bridge_config.height = 2;
    sao_ui_dcomp_bridge_handle_t bridge = nullptr;
    if (sao_ui_dcomp_bridge_create(nullptr, &bridge_config, &bridge) != SAO_STATUS_OK) {
        sao_ui_d3d11_device_destroy(device);
        SKIP("DirectComposition unavailable");
    }
    SaoDcompBridgeState bridge_state{};
    REQUIRE(sao_ui_dcomp_bridge_get_state(bridge, &bridge_state) == SAO_STATUS_OK);
    CHECK(bridge_state.alive);
    CHECK(bridge_state.attached);
    CHECK(bridge_state.swap_chain_ready);
    CHECK(bridge_state.width == 2u);
    CHECK(bridge_state.height == 2u);

    const std::array<uint8_t, 16> pixels = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    REQUIRE(sao_ui_dcomp_bridge_upload_bgra(bridge, pixels.data(), 2, 2, 8) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dcomp_bridge_get_state(bridge, &bridge_state) == SAO_STATUS_OK);
    CHECK(bridge_state.upload_texture_ready);
    REQUIRE(sao_ui_dcomp_bridge_resize(bridge, 3, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_dcomp_bridge_get_state(bridge, &bridge_state) == SAO_STATUS_OK);
    CHECK(bridge_state.width == 3u);
    CHECK(bridge_state.height == 1u);
    CHECK_FALSE(bridge_state.upload_texture_ready);

    sao_ui_dcomp_bridge_destroy(bridge);
    sao_ui_d3d11_device_destroy(device);
}

TEST_CASE("DXGI duplication snapshot exposes configured state or skips",
          "[ui][completion][dxgi][state]") {
    SaoDxgiDupConfig invalid_config{};
    invalid_config.staging_width = 4;
    sao_ui_dxgi_dup_handle_t invalid_duplication = nullptr;
    CHECK(sao_ui_dxgi_dup_create(&invalid_config, &invalid_duplication) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(invalid_duplication == nullptr);

    SaoDxgiDupConfig config{};
    config.acquire_timeout_ms = 37;
    config.auto_recover = true;
    config.staging_width = 4;
    config.staging_height = 4;
    sao_ui_dxgi_dup_handle_t duplication = nullptr;
    const sao_status_t status = sao_ui_dxgi_dup_create(&config, &duplication);
    if (status != SAO_STATUS_OK)
        SKIP("DXGI duplication unavailable");
    SaoDxgiDupState state{};
    REQUIRE(sao_ui_dxgi_dup_get_state(duplication, &state) == SAO_STATUS_OK);
    CHECK(state.output_index == 0u);
    CHECK(state.adapter_index == 0u);
    CHECK(state.acquire_timeout_ms == 37u);
    CHECK(state.staging_width == 4u);
    CHECK(state.staging_height == 4u);
    CHECK(state.auto_recover);
    CHECK(state.alive);
    CHECK_FALSE(state.frame_held);
    sao_ui_dxgi_dup_destroy(duplication);
}

#else

TEST_CASE("Win32 production graphics completion requires Windows", "[ui][completion][graphics]") {
    SUCCEED("Windows-only state APIs");
}

#endif
