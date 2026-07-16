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

#include "sao/ui/adapter.h"
#include "sao/ui/gpu_overlay_window.h"
#include "sao/core/status.h"

#include <cstring>
#include <string>
#include <vector>

// Wave 6 test-only helpers.  SAO_UI_API forward-decls match the DLL
// import macro used by SAO_UI_USING_DLL callers so the symbols
// resolve against the shared library's export table.
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_adapter_test_call_count(
    sao_ui_compositor_overlay_window_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_last_call(
    sao_ui_compositor_overlay_window_handle_t handle,
    char* out_call, size_t out_call_cap);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_layer_name(
    sao_ui_compositor_overlay_window_handle_t handle,
    char* out_name, size_t out_name_cap);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_snapshot(
    sao_ui_compositor_overlay_window_handle_t handle,
    int32_t* x, int32_t* y, int32_t* w, int32_t* h,
    bool* visible, bool* destroyed,
    bool* click_through, bool* input_proxy_attached,
    int32_t* z_order, float* alpha);
extern "C" SAO_UI_API size_t SAO_UI_CALL sao_ui_adapter_test_presenter_frame_bytes(
    sao_ui_compositor_bgra_presenter_handle_t handle);
extern "C" SAO_UI_API bool SAO_UI_CALL sao_ui_adapter_test_presenter_size(
    sao_ui_compositor_bgra_presenter_handle_t handle,
    uint32_t* w, uint32_t* h);

namespace {

SaoGpuOverlayWindowConfig make_cfg(const char* title,
                                   int32_t x = 10, int32_t y = 20,
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
    if (!sao_ui_adapter_test_last_call(h, buf, sizeof(buf))) return {};
    return std::string(buf);
}

}  // namespace

TEST_CASE("adapter_lifecycle_setters", "[ui][adapter][wave6]") {
    // No real compositor — pass nullptr.  The adapter's Wave 6 slice
    // records state locally and does not dereference the compositor
    // handle (Python "swallow exception on compositor sync" idiom).
    const auto cfg = make_cfg("panel");
    sao_ui_compositor_overlay_window_handle_t win = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(
        nullptr, &cfg, &win) == SAO_STATUS_OK);
    REQUIRE(win != nullptr);
    CHECK(last_call(win) == "create");

    // show()
    REQUIRE(sao_ui_compositor_overlay_window_show(win) == SAO_STATUS_OK);
    CHECK(last_call(win) == "show");
    {
        bool visible = false;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            &visible, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(visible);
    }

    // set_geometry(x, y, w, h)
    REQUIRE(sao_ui_compositor_overlay_window_set_geometry(
        win, 40, 50, 400, 250) == SAO_STATUS_OK);
    {
        int32_t x = 0, y = 0, w = 0, h = 0;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, &x, &y, &w, &h, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr));
        CHECK(x == 40);
        CHECK(y == 50);
        CHECK(w == 400);
        CHECK(h == 250);
    }

    // set_click_through(false)
    REQUIRE(sao_ui_compositor_overlay_window_set_click_through(
        win, false) == SAO_STATUS_OK);
    {
        bool ct = true;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, &ct, nullptr, nullptr, nullptr));
        CHECK_FALSE(ct);
    }

    // set_z / set_alpha
    REQUIRE(sao_ui_compositor_overlay_window_set_z(win, 250) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_set_alpha(win, 0.5f) ==
            SAO_STATUS_OK);
    {
        int32_t z = 0;
        float alpha = 0.0f;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, &z, &alpha));
        CHECK(z == 250);
        CHECK(alpha == 0.5f);
    }

    // enable_input_proxy
    REQUIRE(sao_ui_compositor_overlay_window_enable_input_proxy(win) ==
            SAO_STATUS_OK);
    {
        bool ip = false;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, &ip, nullptr, nullptr));
        CHECK(ip);
    }

    // hide()
    REQUIRE(sao_ui_compositor_overlay_window_hide(win) == SAO_STATUS_OK);
    {
        bool visible = true;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            &visible, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK_FALSE(visible);
    }

    // Set-alpha clamp: values outside [0,1] must be clamped.
    REQUIRE(sao_ui_compositor_overlay_window_set_alpha(win, -0.5f) ==
            SAO_STATUS_OK);
    {
        float alpha = 0.0f;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, &alpha));
        CHECK(alpha == 0.0f);
    }
    REQUIRE(sao_ui_compositor_overlay_window_set_alpha(win, 2.0f) ==
            SAO_STATUS_OK);
    {
        float alpha = 0.0f;
        REQUIRE(sao_ui_adapter_test_snapshot(
            win, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, &alpha));
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
    REQUIRE(sao_ui_compositor_overlay_window_create(
        nullptr, &cfg, &a) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_overlay_window_create(
        nullptr, &cfg, &b) == SAO_STATUS_OK);

    char name_a[64]{}, name_b[64]{};
    REQUIRE(sao_ui_adapter_test_layer_name(
        a, name_a, sizeof(name_a)));
    REQUIRE(sao_ui_adapter_test_layer_name(
        b, name_b, sizeof(name_b)));
    CHECK(std::string(name_a).rfind("shared_title_", 0) == 0);
    CHECK(std::string(name_b).rfind("shared_title_", 0) == 0);
    CHECK(std::string(name_a) != std::string(name_b));

    // Empty title -> "layer_<n>" fallback.
    const auto cfg_empty = make_cfg("");
    sao_ui_compositor_overlay_window_handle_t c = nullptr;
    REQUIRE(sao_ui_compositor_overlay_window_create(
        nullptr, &cfg_empty, &c) == SAO_STATUS_OK);
    char name_c[64]{};
    REQUIRE(sao_ui_adapter_test_layer_name(
        c, name_c, sizeof(name_c)));
    CHECK(std::string(name_c).rfind("layer_", 0) == 0);

    sao_ui_compositor_overlay_window_destroy(a);
    sao_ui_compositor_overlay_window_destroy(b);
    sao_ui_compositor_overlay_window_destroy(c);
}

TEST_CASE("adapter_bgra_presenter_frame", "[ui][adapter][wave6]") {
    sao_ui_compositor_bgra_presenter_handle_t p = nullptr;
    REQUIRE(sao_ui_compositor_bgra_presenter_create(nullptr, &p) ==
            SAO_STATUS_OK);
    REQUIRE(p != nullptr);

    // 3x2 opaque blue BGRA frame.
    std::vector<uint8_t> frame{
        255, 0, 0, 255,  255, 0, 0, 255,  255, 0, 0, 255,
        255, 0, 0, 255,  255, 0, 0, 255,  255, 0, 0, 255,
    };
    REQUIRE(sao_ui_compositor_bgra_presenter_set_frame(
        p, frame.data(), 3, 2, 0, 0) == SAO_STATUS_OK);

    // Frame bytes / size mirror what set_frame accepted.
    CHECK(sao_ui_adapter_test_presenter_frame_bytes(p) == 24u);
    uint32_t w = 0, h = 0;
    REQUIRE(sao_ui_adapter_test_presenter_size(p, &w, &h));
    CHECK(w == 3u);
    CHECK(h == 2u);

    // Alpha snapshot round-trip (same as Python @alpha getter/setter).
    REQUIRE(sao_ui_compositor_bgra_presenter_set_alpha(p, 0.75f) ==
            SAO_STATUS_OK);
    CHECK(sao_ui_compositor_bgra_presenter_get_alpha(p) == 0.75f);
    // start_fade snaps to target immediately in this Wave 6 slice.
    REQUIRE(sao_ui_compositor_bgra_presenter_start_fade(
        p, 0.25f, 0.3f) == SAO_STATUS_OK);
    CHECK(sao_ui_compositor_bgra_presenter_get_alpha(p) == 0.25f);

    // set_frame with zero size fails.
    REQUIRE(sao_ui_compositor_bgra_presenter_set_frame(
        p, frame.data(), 0, 2, 0, 0) ==
            SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_ui_compositor_bgra_presenter_destroy(p);
    // Idempotent destroy on nullptr.
    sao_ui_compositor_bgra_presenter_destroy(nullptr);
}
