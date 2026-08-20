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

#include <atomic>
#include <chrono>
#include <thread>

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

struct ReentrantDestroyContext {
    sao_ui_overlay_host_handle_t host = nullptr;
    size_t callback_count = 0;
    bool destroy_results[2]{};
};

void SAO_UI_CALL attempt_reentrant_destroy(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                           void* user_data) {
    auto* context = static_cast<ReentrantDestroyContext*>(user_data);
    if (context == nullptr || context->callback_count >= 2)
        return;
    context->destroy_results[context->callback_count++] =
        sao_ui_overlay_host_destroy(context->host);
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

TEST_CASE("overlay_host_reentrant_destroy_from_callback_is_immediate",
          "[ui][overlay_host][lifetime][reentrant]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);

    struct Cleanup {
        sao_ui_overlay_host_handle_t host;
        ~Cleanup() {
            if (host != nullptr)
                (void)sao_ui_overlay_host_destroy(host);
        }
    } cleanup{host};

    REQUIRE(host != nullptr);

    ReentrantDestroyContext context{host};
    REQUIRE(sao_ui_overlay_host_set_mouse(host, &attempt_reentrant_destroy, &context) ==
            SAO_STATUS_OK);
    const HWND hwnd = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);

    (void)::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
    CHECK(context.callback_count == 1);
    CHECK_FALSE(context.destroy_results[0]);

    REQUIRE(::PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1)) != FALSE);
    CHECK(sao_ui_overlay_host_pump_messages(host) == SAO_STATUS_OK);
    CHECK(context.callback_count == 2);
    CHECK_FALSE(context.destroy_results[1]);

    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_stale_handle_is_benign_during_concurrent_destroy",
          "[ui][overlay_host][lifetime][concurrency]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    REQUIRE(host != nullptr);

    struct Cleanup {
        sao_ui_overlay_host_handle_t host;
        ~Cleanup() {
            if (host != nullptr)
                (void)sao_ui_overlay_host_destroy(host);
        }
    } cleanup{host};

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> fail_fast{false};
    std::thread reader([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!start.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (!start.load(std::memory_order_acquire)) {
            fail_fast.store(true, std::memory_order_release);
            entered.store(true, std::memory_order_release);
            reader_done.store(true, std::memory_order_release);
            return;
        }
        entered.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)sao_ui_overlay_host_hwnd(host);
            (void)sao_ui_overlay_host_control_hwnd(host);
            (void)sao_ui_overlay_host_owner_hwnd(host);
            (void)sao_ui_overlay_host_visible(host);
            (void)sao_ui_overlay_host_input_passthrough(host);
            (void)sao_ui_overlay_host_capture_excluded(host);
            (void)sao_ui_overlay_host_current_dpi(host);
        }
        if (!stop.load(std::memory_order_acquire))
            fail_fast.store(true, std::memory_order_release);
        reader_done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    if (!entered.load(std::memory_order_acquire))
        fail_fast.store(true, std::memory_order_release);

    bool destroyed = false;
    if (!fail_fast.load(std::memory_order_acquire))
        destroyed = sao_ui_overlay_host_destroy(host);
    stop.store(true, std::memory_order_release);
    const auto reader_done_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
    while (!reader_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < reader_done_deadline)
        std::this_thread::yield();
    if (!reader_done.load(std::memory_order_acquire))
        fail_fast.store(true, std::memory_order_release);
    reader.join();
    if (!destroyed)
        destroyed = sao_ui_overlay_host_destroy(host);

    CHECK_FALSE(fail_fast.load(std::memory_order_acquire));
    REQUIRE(destroyed);
    SaoOverlayHostState state{};
    CHECK(sao_ui_overlay_host_get_state(host, &state) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_overlay_host_destroy(host));
    CHECK(sao_ui_overlay_host_hwnd(host) == nullptr);
    CHECK(sao_ui_overlay_host_control_hwnd(host) == nullptr);
    CHECK(sao_ui_overlay_host_owner_hwnd(host) == nullptr);
    CHECK_FALSE(sao_ui_overlay_host_visible(host));
    CHECK_FALSE(sao_ui_overlay_host_input_passthrough(host));
    CHECK_FALSE(sao_ui_overlay_host_capture_excluded(host));
    CHECK(sao_ui_overlay_host_current_dpi(host) == 0u);

    SaoOverlayHostClientRect rect{};
    CHECK(sao_ui_overlay_host_get_client_rect(host, &rect) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_overlay_host_set_visible(host, true) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_overlay_host_require_owner_thread(host) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_overlay_host_make_current(host) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_overlay_host_release_current(host) == SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(sao_ui_overlay_host_swap_buffers(host) == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("overlay_host_destroy_rejects_non_owner_thread_without_retiring",
          "[ui][overlay_host][lifetime][owner_thread]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
    std::atomic<bool> cross_thread_result{true};
    std::thread destroyer([&] {
        cross_thread_result.store(sao_ui_overlay_host_destroy(host),
                                  std::memory_order_release);
    });
    destroyer.join();
    CHECK_FALSE(cross_thread_result.load(std::memory_order_acquire));
    CHECK(sao_ui_overlay_host_hwnd(host) != nullptr);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("overlay_host_external_hwnd_destruction_clears_tombstones",
          "[ui][overlay_host][lifetime][external_hwnd]") {
    const SaoOverlayHostConfig cfg = make_default_config();
    for (int role = 0; role < 3; ++role) {
        sao_ui_overlay_host_handle_t host = nullptr;
        REQUIRE(sao_ui_overlay_host_create(&cfg, &host) == SAO_STATUS_OK);
        HWND target = nullptr;
        if (role == 0)
            target = reinterpret_cast<HWND>(sao_ui_overlay_host_hwnd(host));
        else if (role == 1)
            target = reinterpret_cast<HWND>(sao_ui_overlay_host_control_hwnd(host));
        else
            target = reinterpret_cast<HWND>(sao_ui_overlay_host_owner_hwnd(host));
        REQUIRE(target != nullptr);
        REQUIRE(::DestroyWindow(target) != FALSE);
        if (role == 0)
            CHECK(sao_ui_overlay_host_hwnd(host) == nullptr);
        else if (role == 1)
            CHECK(sao_ui_overlay_host_control_hwnd(host) == nullptr);
        else
            CHECK(sao_ui_overlay_host_owner_hwnd(host) == nullptr);
        REQUIRE(sao_ui_overlay_host_destroy(host));
    }
}
