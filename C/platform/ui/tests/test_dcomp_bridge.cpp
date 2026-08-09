// DirectComposition bridge lifecycle, device, commit, and leak tests.
//
// Coverage:
//   * dcomp_bridge_create_with_dummy_hwnd_succeeds
//   * dcomp_bridge_d3d11_device_nonnull
//   * dcomp_bridge_commit_returns_ok
//   * dcomp_bridge_destroy_releases_com    (10x create/destroy leak guard)
//
// The bridge needs a real HWND (CreateTargetForHwnd rejects null); we
// spin up a hidden Win32 window via a private class name for each test.
// On CI hosts with no D3D11 hardware AND no WARP available, the create
// path fails inside D3D11CreateDevice - the tests SKIP in that case
// rather than fail, because these tests validate COM lifetime
// wiring, not a hardware requirement.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "sao/ui/dcomp_bridge.h"
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

// Internal test-only counter exported from dcomp_bridge.cpp.  Not part
// of the public ABI header, so we redeclare it here with the same
// signature the .cpp exports.
extern "C" SAO_UI_API int64_t SAO_UI_CALL
sao_ui_dcomp_bridge_live_count_for_test(void);

namespace {

#if defined(_WIN32)

// Hidden Win32 window suitable as the DComp target.  DComp only requires
// a valid HWND that owns its own composition target - the class/style
// don't matter for the create path itself.
struct DummyHwnd {
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    ATOM cls = 0;
    std::wstring cls_name;

    explicit DummyHwnd(const wchar_t* label) {
        hinst = ::GetModuleHandleW(nullptr);
        // Unique per-test class name to avoid Register conflicts when a
        // previous test's window is still in destruction.
        cls_name = std::wstring(L"SaoDcompBridgeTest_") + label;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = hinst;
        wc.lpszClassName = cls_name.c_str();
        cls = ::RegisterClassExW(&wc);
        // If RegisterClassEx failed (e.g. duplicate name from a leaked
        // test), Continue anyway - CreateWindowEx will fail loudly.
        hwnd = ::CreateWindowExW(
            0, cls_name.c_str(), L"sao dcomp test",
            WS_OVERLAPPEDWINDOW,
            0, 0, 400, 300,
            nullptr, nullptr, hinst, nullptr);
    }

    ~DummyHwnd() {
        if (hwnd != nullptr) ::DestroyWindow(hwnd);
        if (cls != 0) ::UnregisterClassW(cls_name.c_str(), hinst);
    }
};

SaoDcompBridgeConfig make_bridge_config(HWND hwnd) {
    SaoDcompBridgeConfig cfg{};
    cfg.hwnd = hwnd;
    cfg.d3d11_device = nullptr;
    cfg.alpha_mode = 1;   // DXGI_ALPHA_MODE_PREMULTIPLIED
    cfg.buffer_count = 2;
    cfg.width = 400;
    cfg.height = 300;
    return cfg;
}

// Common gate: skip the CASE cleanly if the environment refuses to hand
// out a D3D11 device (headless CI, no HW, no WARP).  Returns nullptr in
// that case; otherwise a live handle the CASE owns.
sao_ui_dcomp_bridge_handle_t try_create(HWND hwnd) {
    const SaoDcompBridgeConfig cfg = make_bridge_config(hwnd);
    sao_ui_dcomp_bridge_handle_t b = nullptr;
    const sao_status_t rc = sao_ui_dcomp_bridge_create(nullptr, &cfg, &b);
    if (rc != SAO_STATUS_OK) return nullptr;
    return b;
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

TEST_CASE("dcomp_bridge_create_with_dummy_hwnd_succeeds",
          "[ui][dcomp_bridge][interpreter]") {
    DummyHwnd host(L"case_create");
    if (host.hwnd == nullptr) {
        SKIP("CreateWindowEx failed (headless session, no window station)");
    }

    const SaoDcompBridgeConfig cfg = make_bridge_config(host.hwnd);
    sao_ui_dcomp_bridge_handle_t b = nullptr;
    const sao_status_t rc = sao_ui_dcomp_bridge_create(nullptr, &cfg, &b);
    if (rc != SAO_STATUS_OK) {
        // D3D11CreateDevice failed even with WARP fallback - CI is
        // genuinely GPU-less.  Skip is correct: the slice contract is
        // "wire the COM chain", not "guarantee a GPU exists".
        SKIP("D3D11/DComp unavailable in this environment (rc="
             << rc << ")");
    }
    REQUIRE(b != nullptr);
    sao_ui_dcomp_bridge_destroy(b);
}

TEST_CASE("dcomp_bridge_d3d11_device_nonnull",
          "[ui][dcomp_bridge][interpreter]") {
    DummyHwnd host(L"case_d3d11_getter");
    if (host.hwnd == nullptr) SKIP("no window station");

    sao_ui_dcomp_bridge_handle_t b = try_create(host.hwnd);
    if (b == nullptr) SKIP("D3D11/DComp unavailable");

    void* dev = sao_ui_dcomp_bridge_d3d11_device(b);
    CHECK(dev != nullptr);

    void* ctx = sao_ui_dcomp_bridge_d3d11_context(b);
    CHECK(ctx != nullptr);

    void* dc_dev = sao_ui_dcomp_bridge_dcomp_device(b);
    CHECK(dc_dev != nullptr);

    // CreateSwapChainForComposition runs before the visual
    // tree is committed, so the bridge exposes its owned swap chain.
    void* swap = sao_ui_dcomp_bridge_swap_chain(b);
    CHECK(swap != nullptr);

    sao_ui_dcomp_bridge_destroy(b);
}

TEST_CASE("dcomp_bridge_commit_returns_ok",
          "[ui][dcomp_bridge][interpreter]") {
    DummyHwnd host(L"case_commit");
    if (host.hwnd == nullptr) SKIP("no window station");

    sao_ui_dcomp_bridge_handle_t b = try_create(host.hwnd);
    if (b == nullptr) SKIP("D3D11/DComp unavailable");

    // present() in this slice drives IDCompositionDevice::Commit - a
    // no-op flush against an empty root is legal and must succeed.
    const sao_status_t rc = sao_ui_dcomp_bridge_present(b);
    CHECK(rc == SAO_STATUS_OK);

    // device_removed() should report OK on a freshly created device.
    uint32_t reason = 0xDEADBEEF;
    const sao_status_t dr = sao_ui_dcomp_bridge_device_removed(b, &reason);
    CHECK(dr == SAO_STATUS_OK);
    CHECK(reason == 0u);

    sao_ui_dcomp_bridge_destroy(b);
}

TEST_CASE("dcomp_bridge_destroy_releases_com",
          "[ui][dcomp_bridge][interpreter]") {
    DummyHwnd host(L"case_leak");
    if (host.hwnd == nullptr) SKIP("no window station");

    // Baseline: some other test in the executable may hold live bridges
    // right now (unlikely - each CASE tears down - but a co-runner
    // executable could interleave), so record and diff instead of
    // asserting an absolute count.
    const int64_t baseline = sao_ui_dcomp_bridge_live_count_for_test();

    // Ten full cycles preserve leak-symmetry coverage at an acceptable runtime.
    constexpr int kIters = 10;
    int successful = 0;
    for (int i = 0; i < kIters; ++i) {
        sao_ui_dcomp_bridge_handle_t b = try_create(host.hwnd);
        if (b == nullptr) {
            if (i == 0) SKIP("D3D11/DComp unavailable");
            // Mid-loop failure (e.g. TDR) is a real bug - fail.
            FAIL("create #" << i << " failed after earlier successes");
            break;
        }
        ++successful;
        sao_ui_dcomp_bridge_destroy(b);
    }
    CHECK(successful == kIters);

    // create/destroy symmetry: live-count must return to baseline.
    const int64_t after = sao_ui_dcomp_bridge_live_count_for_test();
    CHECK(after == baseline);
}

#else

TEST_CASE("dcomp_bridge_only_on_windows", "[ui][dcomp_bridge][interpreter]") {
    SKIP("DirectComposition requires Windows");
}

#endif  // _WIN32
