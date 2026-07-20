// Overlay-host creation, style, hit-test, and singleton tests.
//
// Coverage:
//   * overlay_host_create_returns_valid_handle
//   * overlay_host_hwnd_has_layered_style
//   * overlay_host_nchit_returns_HTTRANSPARENT
//   * overlay_host_single_instance_guard
//
// The window is created hidden (create() never calls ShowWindow, per
// header line 114 -- the launcher owns visibility).  Every CASE tears
// its host down cleanly so the process-wide single-instance mutex is
// released before the next CASE runs.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/overlay_host.h"
#include "sao/core/status.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

SaoOverlayHostConfig make_default_config() {
    SaoOverlayHostConfig cfg{};
    // Small explicit rect keeps the hidden window off any real monitor
    // configuration when a CI runs on a low-resolution VM.  Non-zero
    // size + zero origin.
    cfg.width       = 640;
    cfg.height      = 480;
    cfg.origin_x    = 0;
    cfg.origin_y    = 0;
    cfg.title_utf16 = L"SAO Overlay Host [lifecycle test]";
    cfg.diagnostics = false;
    cfg.tagwnd_dump = false;
    cfg.dc_mutation_coordinator = nullptr;
    return cfg;
}

}  // namespace

TEST_CASE("overlay_host_create_returns_valid_handle",
          "[ui][overlay_host][host]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;

    const sao_status_t rc = sao_ui_overlay_host_create(&cfg, &host);
    REQUIRE(rc == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    void* hwnd = sao_ui_overlay_host_hwnd(host);
    REQUIRE(hwnd != nullptr);
    REQUIRE(::IsWindow(reinterpret_cast<HWND>(hwnd)) != 0);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_hwnd_has_dcomp_compatible_style",
          "[ui][overlay_host][host]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;

    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // DComp owns alpha composition: layered-window and direct TOPMOST styles
    // must stay absent.  The z_order manager is the only z-order authority.
    CHECK((ex & WS_EX_LAYERED)     == 0);
    CHECK((ex & WS_EX_TRANSPARENT) != 0);
    CHECK((ex & WS_EX_TOPMOST)     == 0);
    CHECK((ex & WS_EX_NOACTIVATE)  != 0);
    CHECK((ex & WS_EX_TOOLWINDOW)  != 0);

    // Not shown (header line 114): never activates on create.
    CHECK(::IsWindowVisible(hwnd) == 0);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_nchit_returns_HTTRANSPARENT",
          "[ui][overlay_host][host]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;

    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    // Hit-test a pixel inside the window rect.  Screen coords packed
    // as documented by MSDN WM_NCHITTEST (LOWORD=x, HIWORD=y with
    // signed cast semantics).
    RECT rect{};
    ::GetWindowRect(hwnd, &rect);
    const int cx = rect.left + 100;
    const int cy = rect.top  + 100;
    const LPARAM pos = MAKELPARAM(static_cast<WORD>(cx),
                                  static_cast<WORD>(cy));

    // SendMessageW dispatches to the WndProc directly (same-thread
    // path).  The WndProc must return HTTRANSPARENT so the OS treats
    // the click as passing through -- the "click-through" contract.
    const LRESULT hit = ::SendMessageW(hwnd, WM_NCHITTEST, 0, pos);
    CHECK(hit == HTTRANSPARENT);

    // WM_MOUSEACTIVATE test: header rule #4 says the WndProc answer
    // is what actually holds, above and beyond WS_EX_NOACTIVATE.
    const LRESULT act = ::SendMessageW(hwnd, WM_MOUSEACTIVATE, 0, 0);
    CHECK(act == MA_NOACTIVATE);

    // WM_ERASEBKGND must return 1 to short-circuit the default black
    // erase pass (prevents the black-frame flash on show).
    HDC any_dc = ::GetDC(hwnd);
    const LRESULT erase = ::SendMessageW(hwnd, WM_ERASEBKGND,
                                         reinterpret_cast<WPARAM>(any_dc),
                                         0);
    if (any_dc) ::ReleaseDC(hwnd, any_dc);
    CHECK(erase == 1);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_single_instance_guard",
          "[ui][overlay_host][host]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host_a = nullptr;
    sao_ui_overlay_host_handle_t host_b = nullptr;

    REQUIRE(sao_ui_overlay_host_create(&cfg, &host_a) == SAO_STATUS_OK);
    REQUIRE(host_a != nullptr);

    // Second create must fail with ALREADY_EXISTS while host_a is alive.
    const sao_status_t rc = sao_ui_overlay_host_create(&cfg, &host_b);
    CHECK(rc == SAO_STATUS_ERR_ALREADY_EXISTS);
    CHECK(host_b == nullptr);

    // Tear host_a down; a subsequent create must succeed.
    REQUIRE(sao_ui_overlay_host_destroy(host_a));

    sao_ui_overlay_host_handle_t host_c = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host_c) == SAO_STATUS_OK);
    REQUIRE(host_c != nullptr);
    REQUIRE(sao_ui_overlay_host_destroy(host_c));
}
