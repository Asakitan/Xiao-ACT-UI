// Catch2 tests for apply_stealth_window (window_hardening.h).
//
// The production write-path routes through sao_security_anti_screencap
// helper functions (dynamic-loaded from the DLL that sao_platform_ui
// already brings into the process).  In the isolated test process
// sao_security_anti_screencap.dll is not resident, so
// apply_stealth_window degrades gracefully to `false` without touching
// the window at all -- proving:
//   * null / graceful-degradation contract
//   * no direct SetWindowDisplayAffinity call (would otherwise succeed
//     regardless of the helper DLL presence and let the test read back a
//     non-NONE affinity via user32)
//
// Full end-to-end verification of the anti-screencap write+read lives in
// sao_security_anti_screencap's own Catch2 suite (see
// security/anti_screencap/tests/test_overlay_host_capture.cpp) which
// links the helper library directly and can exercise real DWM affinity.

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "window_hardening.h"

namespace {

struct DummyWindow {
    HWND hwnd = nullptr;
    DummyWindow() {
        hwnd = ::CreateWindowExW(
            WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr,
            ::GetModuleHandleW(nullptr), nullptr);
    }
    ~DummyWindow() {
        if (hwnd != nullptr) {
            ::DestroyWindow(hwnd);
        }
    }
};

}  // namespace

TEST_CASE("apply_stealth_window rejects null hwnd",
          "[ai_editor][stealth]") {
    REQUIRE_FALSE(sao::ai_editor::apply_stealth_window(nullptr));
}

TEST_CASE("apply_stealth_window gracefully returns false when the "
          "sao_security helper DLL is not loaded",
          "[ai_editor][stealth]") {
    DummyWindow w;
    REQUIRE(w.hwnd != nullptr);
    // The test binary does not link sao_security_anti_screencap, and no
    // sibling library pulls it in either, so GetModuleHandleW inside
    // apply_stealth_window returns NULL.  The helper must return false
    // without touching the window (proving it never falls back to a
    // direct SetWindowDisplayAffinity call bypassing the helper).
    if (::GetModuleHandleW(L"sao_security_anti_screencap.dll") == nullptr) {
        REQUIRE_FALSE(sao::ai_editor::apply_stealth_window(w.hwnd));
        // Verify no side effect on the window's affinity: still WDA_NONE.
        DWORD affinity = 0xFFFFFFFFu;
        REQUIRE(::GetWindowDisplayAffinity(w.hwnd, &affinity) != FALSE);
        REQUIRE(affinity == 0u);
    } else {
        // Rare CI environment where the helper DLL happens to be
        // pre-loaded; in that case apply_stealth_window may succeed.
        // Either outcome is contract-compliant; only ensure no crash.
        (void)sao::ai_editor::apply_stealth_window(w.hwnd);
    }
}

TEST_CASE("apply_stealth_window handles a null-hwnd repeatedly",
          "[ai_editor][stealth]") {
    for (int i = 0; i < 16; ++i) {
        REQUIRE_FALSE(sao::ai_editor::apply_stealth_window(nullptr));
    }
}
