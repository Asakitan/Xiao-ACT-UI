// Wave 6 / Agent e tests for the overlay_adapter first slice.
//
// Coverage (3 test cases):
//   * adapter_lifecycle_setters      — create → show → hide → destroy
//     visits the correct call log; the snapshot state (geometry,
//     visibility, click_through, z, alpha) tracks the setter inputs.
//     Matches the Python `CompositorOverlayWindow` public API.
//   * adapter_unique_layer_names     — two windows with the same
//     title get distinct layer names ("<title>_<n>") — the same
//     uniqueness guarantee as Python's `_gen_layer_name`.
//   * adapter_bgra_presenter_frame   — set_frame() stages BGRA bytes
//     of the exact dimensions; alpha snap/get parity matches the
//     Python `CompositorBgraPresenter` alpha accessor.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/adapter.h"
#include "sao/ui/compositor.h"
#include "sao/ui/gpu_overlay_window.h"
#include "sao/ui/overlay_host.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
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
#endif

// Wave 6 test-only helpers.  SAO_UI_API forward-decls match the DLL
// import macro used by SAO_UI_USING_DLL callers so the symbols
// resolve against the shared library's export table.
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_adapter_test_call_count(sao_ui_compositor_overlay_window_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_last_call(
    sao_ui_compositor_overlay_window_handle_t handle, char* out_call, size_t out_call_cap);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_layer_name(
    sao_ui_compositor_overlay_window_handle_t handle, char* out_name, size_t out_name_cap);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_snapshot(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t* x, int32_t* y, int32_t* w,
    int32_t* h, bool* visible, bool* destroyed, bool* click_through, bool* input_proxy_attached,
    int32_t* z_order, float* alpha);
extern "C" SAO_UI_API size_t SAO_UI_CALL
sao_ui_adapter_test_presenter_frame_bytes(sao_ui_compositor_bgra_presenter_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_presenter_size(
    sao_ui_compositor_bgra_presenter_handle_t handle, uint32_t* w, uint32_t* h);
extern "C" SAO_UI_API int32_t SAO_UI_CALL
sao_ui_adapter_test_target_fps(sao_ui_compositor_overlay_window_handle_t handle);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_adapter_test_dispatch_button(sao_ui_compositor_overlay_window_handle_t handle);
extern "C" SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_adapter_test_fail_next_host_sync(
    sao_ui_compositor_overlay_window_handle_t handle, int32_t phase, sao_status_t status);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_policy_snapshot(
    sao_ui_compositor_overlay_window_handle_t handle, bool* layer_visible,
    bool* layer_click_through, bool* layer_input_enabled, bool* degraded,
    sao_status_t* degraded_status);

namespace {

using namespace std::chrono_literals;

SaoGpuOverlayWindowConfig make_cfg(const char* title, int32_t x = 10, int32_t y = 20,
                                   int32_t w = 300, int32_t h = 200) {
    SaoGpuOverlayWindowConfig cfg{};
    cfg.x = x;
    cfg.y = y;
    cfg.width = w;
    cfg.height = h;
    cfg.click_through = true;
    cfg.title_utf8 = title;
    cfg.vsync = false;
    cfg.z_order = 100;
    cfg.render_fn = nullptr;
    cfg.render_fn_user_data = nullptr;
    return cfg;
}

std::string last_call(sao_ui_compositor_overlay_window_handle_t h) {
    char buf[64]{};
    if (!sao_ui_adapter_test_last_call(h, buf, sizeof(buf)))
        return {};
    return std::string(buf);
}

size_t layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    if (sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) != SAO_STATUS_OK) {
        return static_cast<size_t>(-1);
    }
    return count;
}

std::vector<uint8_t> compositor_snapshot(sao_ui_compositor_handle_t compositor,
                                         uint32_t* width = nullptr, uint32_t* height = nullptr) {
    uint32_t local_width = 0;
    uint32_t local_height = 0;
    size_t required = 0;
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0, &local_width, &local_height,
                                            &required) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(required);
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(), &local_width,
                                            &local_height, &required) == SAO_STATUS_OK);
    if (width != nullptr)
        *width = local_width;
    if (height != nullptr)
        *height = local_height;
    return pixels;
}

struct BlockingButton {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool release{};
};

void SAO_UI_CALL blocking_button(int32_t, int32_t, int32_t, float, float, void* user_data) {
    auto* state = static_cast<BlockingButton*>(user_data);
    std::unique_lock lock(state->mutex);
    state->entered = true;
    state->cv.notify_all();
    state->cv.wait(lock, [state] { return state->release; });
}

struct SelfDestroyWindow {
    sao_ui_compositor_overlay_window_handle_t window{};
    size_t count{};
};

void SAO_UI_CALL destroy_window_from_button(int32_t, int32_t, int32_t, float, float,
                                            void* user_data) {
    auto* state = static_cast<SelfDestroyWindow*>(user_data);
    ++state->count;
    sao_ui_compositor_overlay_window_destroy(state->window);
}

#if defined(_WIN32)
constexpr int32_t kAdapterHostSyncRegion = 0;
constexpr int32_t kAdapterHostSyncInput = 1;
constexpr int32_t kAdapterHostSyncWindow = 2;

LRESULT host_hit_test(sao_ui_overlay_host_handle_t host, int32_t x, int32_t y) {
    const auto hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    RECT rect{};
    REQUIRE(hwnd != nullptr);
    REQUIRE(GetWindowRect(hwnd, &rect));
    const int32_t screen_x = rect.left + x;
    const int32_t screen_y = rect.top + y;
    const LPARAM point =
        static_cast<LPARAM>(static_cast<uint16_t>(screen_x)) |
        (static_cast<LPARAM>(static_cast<uint16_t>(screen_y)) << 16);
    return SendMessageW(hwnd, WM_NCHITTEST, 0, point);
}

struct HostedAdapterWindow {
    sao_ui_overlay_host_handle_t host{};
    sao_ui_compositor_handle_t compositor{};
    sao_ui_compositor_overlay_window_handle_t window{};

    explicit HostedAdapterWindow(const char* title, bool click_through = true) {
        SaoOverlayHostConfig host_config{};
        host_config.width = 96;
        host_config.height = 64;
        REQUIRE(sao_ui_overlay_host_create(&host_config, &host) == SAO_STATUS_OK);

        SaoCompositorConfig compositor_config{};
        REQUIRE(sao_ui_compositor_create(host, &compositor_config, &compositor) ==
                SAO_STATUS_OK);

        auto config = make_cfg(title, 0, 0, 32, 24);
        config.click_through = click_through;
        REQUIRE(sao_ui_compositor_overlay_window_create(compositor, &config, &window) ==
                SAO_STATUS_OK);
    }

    ~HostedAdapterWindow() {
        sao_ui_compositor_overlay_window_destroy(window);
        sao_ui_compositor_destroy(compositor);
        (void)sao_ui_overlay_host_destroy(host);
    }

    HostedAdapterWindow(const HostedAdapterWindow&) = delete;
    HostedAdapterWindow& operator=(const HostedAdapterWindow&) = delete;
};

void check_adapter_policy(sao_ui_compositor_overlay_window_handle_t window,
                          bool expected_visible, bool expected_click_through,
                          bool expected_layer_input, bool expected_degraded,
                          sao_status_t expected_degraded_status) {
    bool local_visible = false;
    bool local_click_through = false;
    REQUIRE(sao_ui_adapter_test_snapshot(window, nullptr, nullptr, nullptr, nullptr,
                                         &local_visible, nullptr, &local_click_through, nullptr,
                                         nullptr, nullptr));
    bool layer_visible = false;
    bool layer_click_through = false;
    bool layer_input_enabled = false;
    bool degraded = false;
    sao_status_t degraded_status = SAO_STATUS_ERR_UNKNOWN;
    REQUIRE(sao_ui_adapter_test_policy_snapshot(
        window, &layer_visible, &layer_click_through, &layer_input_enabled, &degraded,
        &degraded_status));
    CHECK(local_visible == expected_visible);
    CHECK(local_click_through == expected_click_through);
    CHECK(layer_visible == expected_visible);
    CHECK(layer_click_through == expected_click_through);
    CHECK(layer_input_enabled == expected_layer_input);
    CHECK(degraded == expected_degraded);
    CHECK(degraded_status == expected_degraded_status);
}
#endif

} // namespace

TEST_CASE("adapter_lifecycle_setters", "[ui][adapter][wave6]") {
    // No real compositor — pass nullptr.  The adapter's Wave 6 slice
    // records state locally and does not dereference the compositor
    // handle (Python "swallow exception on compositor sync" idiom).
    const auto cfg = make_cfg("panel");
    sao_ui_compositor_overlay_window_handle_t win = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &cfg, &win) == SAO_STATUS_OK);
    REQUIRE(win != nullptr);
    CHECK(sao_ui_compositor_overlay_window_backing_state(win) ==
          SAO_UI_COMPOSITOR_OVERLAY_BACKING_FIXTURE);
    CHECK(sao_ui_compositor_overlay_window_layer(win) == nullptr);
    CHECK(last_call(win) == "create");

    // show()
    REQUIRE(sao_ui_compositor_overlay_window_show(win) == SAO_STATUS_OK);
    CHECK(last_call(win) == "show");
    {
        bool visible = false;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, &visible,
                                             nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(visible);
    }

    // set_geometry(x, y, w, h)
    REQUIRE(sao_ui_compositor_overlay_window_set_geometry(win, 40, 50, 400, 250) == SAO_STATUS_OK);
    {
        int32_t x = 0, y = 0, w = 0, h = 0;
        REQUIRE(sao_ui_adapter_test_snapshot(win, &x, &y, &w, &h, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr));
        CHECK(x == 40);
        CHECK(y == 50);
        CHECK(w == 400);
        CHECK(h == 250);
    }

    // set_click_through(false)
    REQUIRE(sao_ui_compositor_overlay_window_set_click_through(win, false) == SAO_STATUS_OK);
    {
        bool ct = true;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, &ct, nullptr, nullptr, nullptr));
        CHECK_FALSE(ct);
    }

    // set_z / set_alpha
    REQUIRE(sao_ui_compositor_overlay_window_set_z(win, 250) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_set_alpha(win, 0.5f) == SAO_STATUS_OK);
    {
        int32_t z = 0;
        float alpha = 0.0f;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, &z, &alpha));
        CHECK(z == 250);
        CHECK(alpha == 0.5f);
    }

    // enable_input_proxy
    REQUIRE(sao_ui_compositor_overlay_window_enable_input_proxy(win) == SAO_STATUS_OK);
    {
        bool ip = false;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, &ip, nullptr, nullptr));
        CHECK(ip);
    }

    // hide()
    REQUIRE(sao_ui_compositor_overlay_window_hide(win) == SAO_STATUS_OK);
    {
        bool visible = true;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, &visible,
                                             nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK_FALSE(visible);
    }

    // Set-alpha clamp: values outside [0,1] must be clamped.
    REQUIRE(sao_ui_compositor_overlay_window_set_alpha(win, -0.5f) == SAO_STATUS_OK);
    {
        float alpha = 0.0f;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr, &alpha));
        CHECK(alpha == 0.0f);
    }
    REQUIRE(sao_ui_compositor_overlay_window_set_alpha(win, 2.0f) == SAO_STATUS_OK);
    {
        float alpha = 0.0f;
        REQUIRE(sao_ui_adapter_test_snapshot(win, nullptr, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr, &alpha));
        CHECK(alpha == 1.0f);
    }

    // Count: create + show + set_geometry + set_click_through + set_z
    //      + set_alpha + enable_input_proxy + hide + 2× set_alpha
    //   = 10 recorded calls (the current one)
    CHECK(sao_ui_adapter_test_call_count(win) == 10u);

    sao_ui_compositor_overlay_window_destroy(win);
    // No further use of `win` after destroy.
}

TEST_CASE("adapter_unique_layer_names", "[ui][adapter][wave6]") {
    // Two windows with the same title get distinct "<title>_<n>"
    // names — the sequence number is monotonic across the process,
    // so we only check that they differ.
    const auto cfg = make_cfg("shared_title");
    sao_ui_compositor_overlay_window_handle_t a = nullptr;
    sao_ui_compositor_overlay_window_handle_t b = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &cfg, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &cfg, &b) == SAO_STATUS_OK);

    char name_a[64]{}, name_b[64]{};
    REQUIRE(sao_ui_adapter_test_layer_name(a, name_a, sizeof(name_a)));
    REQUIRE(sao_ui_adapter_test_layer_name(b, name_b, sizeof(name_b)));
    CHECK(std::string(name_a).rfind("shared_title_", 0) == 0);
    CHECK(std::string(name_b).rfind("shared_title_", 0) == 0);
    CHECK(std::string(name_a) != std::string(name_b));

    // Empty title -> "layer_<n>" fallback.
    const auto cfg_empty = make_cfg("");
    sao_ui_compositor_overlay_window_handle_t c = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &cfg_empty, &c) == SAO_STATUS_OK);
    char name_c[64]{};
    REQUIRE(sao_ui_adapter_test_layer_name(c, name_c, sizeof(name_c)));
    CHECK(std::string(name_c).rfind("layer_", 0) == 0);

    sao_ui_compositor_overlay_window_destroy(a);
    sao_ui_compositor_overlay_window_destroy(b);
    sao_ui_compositor_overlay_window_destroy(c);
}

TEST_CASE("adapter_bgra_presenter_frame", "[ui][adapter][wave6]") {
    sao_ui_compositor_bgra_presenter_handle_t p = nullptr;
    REQUIRE(sao_ui_compositor_bgra_presenter_create(nullptr, &p) == SAO_STATUS_OK);
    REQUIRE(p != nullptr);

    // 3x2 opaque blue BGRA frame.
    std::vector<uint8_t> frame{
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
    };
    REQUIRE(sao_ui_compositor_bgra_presenter_set_frame(p, frame.data(), 3, 2, 0, 0) ==
            SAO_STATUS_OK);

    // Frame bytes / size mirror what set_frame accepted.
    CHECK(sao_ui_adapter_test_presenter_frame_bytes(p) == 24u);
    uint32_t w = 0, h = 0;
    REQUIRE(sao_ui_adapter_test_presenter_size(p, &w, &h));
    CHECK(w == 3u);
    CHECK(h == 2u);

    // Alpha snapshot round-trip (same as Python @alpha getter/setter).
    REQUIRE(sao_ui_compositor_bgra_presenter_set_alpha(p, 0.75f) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_bgra_presenter_get_alpha(p) == 0.75f);
    // start_fade snaps to target immediately in this Wave 6 slice.
    REQUIRE(sao_ui_compositor_bgra_presenter_start_fade(p, 0.25f, 0.3f) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_bgra_presenter_get_alpha(p) == 0.25f);

    // set_frame with zero size fails.
    REQUIRE(sao_ui_compositor_bgra_presenter_set_frame(p, frame.data(), 0, 2, 0, 0) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_ui_compositor_bgra_presenter_destroy(p);
    // Idempotent destroy on nullptr.
    sao_ui_compositor_bgra_presenter_destroy(nullptr);
}

TEST_CASE("adapter_real_layer_backing_composites_and_releases", "[ui][adapter][layer][wave6]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    REQUIRE(layer_count(compositor) == 0u);

    const auto config = make_cfg("real_adapter", 0, 0, 3, 2);
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(compositor, &config, &window) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_overlay_window_backing_state(window) ==
          SAO_UI_COMPOSITOR_OVERLAY_BACKING_LAYER);
    const sao_ui_layer_handle_t layer = sao_ui_compositor_overlay_window_layer(window);
    REQUIRE(layer != nullptr);
    CHECK(layer_count(compositor) == 1u);

    sao_ui_compositor_bgra_presenter_handle_t presenter = nullptr;
    REQUIRE(sao_ui_compositor_bgra_presenter_create(layer, &presenter) == SAO_STATUS_OK);
    const std::vector<uint8_t> opaque_blue{
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
    };
    REQUIRE(sao_ui_compositor_bgra_presenter_set_frame(presenter, opaque_blue.data(), 3, 2, 1, 1) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_set_click_through(window, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_set_input_callbacks(
                window, nullptr, nullptr, nullptr, nullptr, nullptr) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_show(window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_set_z(window, 321) == SAO_STATUS_OK);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto opaque = compositor_snapshot(compositor, &width, &height);
    CHECK(width == 4u);
    CHECK(height == 3u);
    REQUIRE(opaque.size() == 48u);
    const size_t first_pixel = (static_cast<size_t>(width) + 1u) * 4u;
    CHECK(opaque[first_pixel] == 255u);
    CHECK(opaque[first_pixel + 3u] == 255u);

    REQUIRE(sao_ui_compositor_bgra_presenter_set_alpha(presenter, 0.5f) == SAO_STATUS_OK);
    const auto translucent = compositor_snapshot(compositor, &width, &height);
    CHECK(translucent[first_pixel] >= 127u);
    CHECK(translucent[first_pixel] <= 128u);
    CHECK(translucent[first_pixel + 3u] >= 127u);
    CHECK(translucent[first_pixel + 3u] <= 128u);

    CHECK(sao_ui_compositor_bgra_presenter_start_fade(presenter, 0.25f, -1.0f) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_ui_compositor_bgra_presenter_start_fade(presenter, 0.25f, 0.1f) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_bgra_presenter_get_alpha(presenter) == 0.25f);

    REQUIRE(sao_ui_compositor_overlay_window_hide(window) == SAO_STATUS_OK);
    sao_ui_compositor_bgra_presenter_destroy(presenter);
    sao_ui_compositor_overlay_window_destroy(window);
    CHECK(layer_count(compositor) == 0u);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("adapter_input_callback_replacement_waits_for_old_generation",
          "[ui][adapter][callback]") {
    const auto config = make_cfg("adapter_callback_rundown");
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &config, &window) == SAO_STATUS_OK);
    BlockingButton state;
    REQUIRE(sao_ui_compositor_overlay_window_set_input_callbacks(
                window, nullptr, nullptr, &blocking_button, nullptr, &state) == SAO_STATUS_OK);
    std::atomic<sao_status_t> caller_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread caller([&] { caller_status.store(sao_ui_adapter_test_dispatch_button(window)); });
    {
        std::unique_lock lock(state.mutex);
        REQUIRE(state.cv.wait_for(lock, 1s, [&] { return state.entered; }));
    }
    std::atomic_bool replaced{false};
    std::atomic<sao_status_t> replacement_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread replacement([&] {
        replacement_status.store(sao_ui_compositor_overlay_window_set_input_callbacks(
            window, nullptr, nullptr, nullptr, nullptr, nullptr));
        replaced.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(replaced.load());
    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
    caller.join();
    replacement.join();
    CHECK(caller_status.load() == SAO_STATUS_OK);
    CHECK(replacement_status.load() == SAO_STATUS_OK);
    CHECK(replaced.load());
    sao_ui_compositor_overlay_window_destroy(window);
}

TEST_CASE("adapter_input_callback_can_destroy_its_window", "[ui][adapter][callback][destroy]") {
    const auto config = make_cfg("adapter_callback_self_destroy");
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &config, &window) == SAO_STATUS_OK);
    SelfDestroyWindow state{window};
    REQUIRE(sao_ui_compositor_overlay_window_set_input_callbacks(window, nullptr, nullptr,
                                                                 &destroy_window_from_button,
                                                                 nullptr, &state) == SAO_STATUS_OK);
    REQUIRE(sao_ui_adapter_test_dispatch_button(window) == SAO_STATUS_OK);
    CHECK(state.count == 1u);
    CHECK(sao_ui_compositor_overlay_window_show(window) == SAO_STATUS_ERR_HANDLE_INVALID);
}

#if defined(_WIN32)
TEST_CASE("adapter click-through toggles compositor hit-test true false true",
      "[ui][adapter][click_through][hit_test]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 96;
    host_config.height = 64;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&host_config, &host) == SAO_STATUS_OK);

    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(host, &compositor_config, &compositor) ==
        SAO_STATUS_OK);

    const auto config = make_cfg("adapter_click_policy", 0, 0, 32, 24);
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(
        compositor, &config, &window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_show(window) == SAO_STATUS_OK);

    CHECK(sao_ui_overlay_host_input_passthrough(host));
    CHECK(host_hit_test(host, 8, 8) == HTTRANSPARENT);

    REQUIRE(sao_ui_compositor_overlay_window_set_click_through(window, false) ==
        SAO_STATUS_OK);
    CHECK_FALSE(sao_ui_overlay_host_input_passthrough(host));
    CHECK(host_hit_test(host, 8, 8) == HTCLIENT);

    REQUIRE(sao_ui_compositor_overlay_window_set_click_through(window, true) ==
        SAO_STATUS_OK);
    CHECK(sao_ui_overlay_host_input_passthrough(host));
    CHECK(host_hit_test(host, 8, 8) == HTTRANSPARENT);

    sao_ui_compositor_overlay_window_destroy(window);
    sao_ui_compositor_destroy(compositor);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}

    TEST_CASE("adapter show rolls layer and local state back when host region sync fails",
            "[ui][adapter][transaction][rollback][region]") {
        HostedAdapterWindow fixture("adapter_show_region_rollback");
        REQUIRE(sao_ui_adapter_test_fail_next_host_sync(
                fixture.window, kAdapterHostSyncRegion, SAO_STATUS_ERR_OS_CALL_FAILED) ==
            SAO_STATUS_OK);

        CHECK(sao_ui_compositor_overlay_window_show(fixture.window) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
        check_adapter_policy(fixture.window, false, true, false, false, SAO_STATUS_OK);
        CHECK(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(host_hit_test(fixture.host, 8, 8) == HTTRANSPARENT);

        REQUIRE(sao_ui_compositor_overlay_window_show(fixture.window) == SAO_STATUS_OK);
        check_adapter_policy(fixture.window, true, true, false, false, SAO_STATUS_OK);
    }

    TEST_CASE("adapter hide restores interactive host state when input sync fails",
            "[ui][adapter][transaction][rollback][input]") {
        HostedAdapterWindow fixture("adapter_hide_input_rollback", false);
        REQUIRE(sao_ui_compositor_overlay_window_show(fixture.window) == SAO_STATUS_OK);
        REQUIRE_FALSE(sao_ui_overlay_host_input_passthrough(fixture.host));
        REQUIRE(host_hit_test(fixture.host, 8, 8) == HTCLIENT);
        REQUIRE(sao_ui_adapter_test_fail_next_host_sync(
                fixture.window, kAdapterHostSyncInput, SAO_STATUS_ERR_OS_CALL_FAILED) ==
            SAO_STATUS_OK);

        CHECK(sao_ui_compositor_overlay_window_hide(fixture.window) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
        check_adapter_policy(fixture.window, true, false, true, false, SAO_STATUS_OK);
        CHECK_FALSE(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(host_hit_test(fixture.host, 8, 8) == HTCLIENT);
    }

    TEST_CASE("adapter click-through restores policy when host window sync fails",
            "[ui][adapter][transaction][rollback][window]") {
        HostedAdapterWindow fixture("adapter_click_window_rollback");
        REQUIRE(sao_ui_compositor_overlay_window_show(fixture.window) == SAO_STATUS_OK);
        REQUIRE(sao_ui_compositor_overlay_window_enable_input_proxy(fixture.window) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_adapter_test_fail_next_host_sync(
                fixture.window, kAdapterHostSyncWindow, SAO_STATUS_ERR_OS_CALL_FAILED) ==
            SAO_STATUS_OK);

        CHECK(sao_ui_compositor_overlay_window_set_click_through(fixture.window, false) ==
            SAO_STATUS_ERR_OS_CALL_FAILED);
        check_adapter_policy(fixture.window, true, true, true, false, SAO_STATUS_OK);
        CHECK(sao_ui_overlay_host_input_passthrough(fixture.host));
        CHECK(host_hit_test(fixture.host, 8, 8) == HTTRANSPARENT);
    }

    TEST_CASE("adapter exposes degraded state when host rollback sync also fails",
            "[ui][adapter][transaction][rollback][degraded]") {
        HostedAdapterWindow fixture("adapter_degraded_rollback");
        REQUIRE(sao_ui_adapter_test_fail_next_host_sync(
                fixture.window, kAdapterHostSyncRegion, SAO_STATUS_ERR_OS_CALL_FAILED) ==
            SAO_STATUS_OK);
        REQUIRE(sao_ui_adapter_test_fail_next_host_sync(
                fixture.window, kAdapterHostSyncRegion, SAO_STATUS_ERR_ACCESS_DENIED) ==
            SAO_STATUS_OK);

        CHECK(sao_ui_compositor_overlay_window_show(fixture.window) ==
            SAO_STATUS_ERR_SURFACE_INVALID);
        check_adapter_policy(fixture.window, false, true, false, true,
                     SAO_STATUS_ERR_ACCESS_DENIED);

        REQUIRE(sao_ui_compositor_overlay_window_show(fixture.window) == SAO_STATUS_OK);
        check_adapter_policy(fixture.window, true, true, false, false, SAO_STATUS_OK);
    }

TEST_CASE("adapter host synchronization enforces the real owner thread",
          "[ui][adapter][owner_thread]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 96;
    host_config.height = 64;
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&host_config, &host) == SAO_STATUS_OK);
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(host, &compositor_config, &compositor) == SAO_STATUS_OK);
    const auto config = make_cfg("adapter_owner_thread", 0, 0, 32, 24);
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(compositor, &config, &window) ==
            SAO_STATUS_OK);

    std::atomic<sao_status_t> worker_status{SAO_STATUS_OK};
    std::thread worker(
        [&] { worker_status.store(sao_ui_compositor_overlay_window_show(window)); });
    worker.join();
    CHECK(worker_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    bool visible = true;
    REQUIRE(sao_ui_adapter_test_snapshot(window, nullptr, nullptr, nullptr, nullptr, &visible,
                                         nullptr, nullptr, nullptr, nullptr, nullptr));
    CHECK_FALSE(visible);
    CHECK(sao_ui_compositor_overlay_window_show(window) == SAO_STATUS_OK);

    sao_ui_compositor_overlay_window_destroy(window);
    sao_ui_compositor_destroy(compositor);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}
#endif

TEST_CASE("adapter_window_and_presenter_stale_handles_are_stable", "[ui][adapter][lifetime]") {
    const auto config = make_cfg("adapter_stable_handle");
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(nullptr, &config, &window) == SAO_STATUS_OK);
    std::atomic_bool stop{false};
    std::atomic<sao_status_t> unexpected_status{SAO_STATUS_OK};
    std::thread mover([&] {
        int32_t position = 0;
        while (!stop.load()) {
            const sao_status_t status =
                sao_ui_compositor_overlay_window_move(window, position, position);
            if (status == SAO_STATUS_ERR_HANDLE_INVALID)
                break;
            if (status != SAO_STATUS_OK) {
                unexpected_status.store(status);
                break;
            }
            ++position;
        }
    });
    std::this_thread::sleep_for(20ms);
    sao_ui_compositor_overlay_window_destroy(window);
    stop.store(true);
    mover.join();
    CHECK(unexpected_status.load() == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_overlay_window_move(window, 1, 1) == SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_compositor_overlay_window_destroy(window);

    sao_ui_compositor_bgra_presenter_handle_t presenter = nullptr;
    REQUIRE(sao_ui_compositor_bgra_presenter_create(nullptr, &presenter) == SAO_STATUS_OK);
    sao_ui_compositor_bgra_presenter_destroy(presenter);
    CHECK(sao_ui_compositor_bgra_presenter_set_alpha(presenter, 0.5F) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    sao_ui_compositor_bgra_presenter_destroy(presenter);
}

TEST_CASE("adapter_raise_is_monotonic_and_vsync_sets_target", "[ui][adapter][z][vsync]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    auto config = make_cfg("adapter_vsync", 0, 0, 2, 2);
    config.vsync = true;
    sao_ui_compositor_overlay_window_handle_t first = nullptr;
    sao_ui_compositor_overlay_window_handle_t second = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(compositor, &config, &first) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_create(compositor, &config, &second) == SAO_STATUS_OK);
    CHECK(sao_ui_adapter_test_target_fps(first) > 0);
    REQUIRE(sao_ui_compositor_overlay_window_raise_to_top(first) == SAO_STATUS_OK);
    int32_t first_z = 0;
    REQUIRE(sao_ui_adapter_test_snapshot(first, nullptr, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, &first_z, nullptr));
    REQUIRE(sao_ui_compositor_overlay_window_raise_to_top(second) == SAO_STATUS_OK);
    int32_t second_z = 0;
    REQUIRE(sao_ui_adapter_test_snapshot(second, nullptr, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, &second_z, nullptr));
    CHECK(second_z > first_z);
    sao_ui_compositor_overlay_window_destroy(first);
    sao_ui_compositor_overlay_window_destroy(second);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("adapter_direct_alpha_cancels_running_fade", "[ui][adapter][fade]") {
    SaoCompositorConfig compositor_config{};
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &compositor_config, &compositor) == SAO_STATUS_OK);
    auto config = make_cfg("adapter_fade_cancel", 0, 0, 1, 1);
    sao_ui_compositor_overlay_window_handle_t window = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(compositor, &config, &window) == SAO_STATUS_OK);
    sao_ui_compositor_bgra_presenter_handle_t presenter = nullptr;
    REQUIRE(sao_ui_compositor_bgra_presenter_create(sao_ui_compositor_overlay_window_layer(window),
                                                    &presenter) == SAO_STATUS_OK);
    const std::vector<uint8_t> pixel{255, 255, 255, 255};
    REQUIRE(sao_ui_compositor_bgra_presenter_set_frame(presenter, pixel.data(), 1, 1, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_show(window) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_bgra_presenter_start_fade(presenter, 0.0F, 2.0F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_bgra_presenter_set_alpha(presenter, 0.75F) == SAO_STATUS_OK);
    std::this_thread::sleep_for(20ms);
    const auto snapshot = compositor_snapshot(compositor);
    REQUIRE(snapshot.size() == 4u);
    CHECK(snapshot[3] >= 190u);
    CHECK(snapshot[3] <= 192u);
    sao_ui_compositor_bgra_presenter_destroy(presenter);
    sao_ui_compositor_overlay_window_destroy(window);
    sao_ui_compositor_destroy(compositor);
}
