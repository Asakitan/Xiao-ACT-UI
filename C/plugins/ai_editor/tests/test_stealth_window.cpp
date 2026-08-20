// Catch2 tests for apply_stealth_window (window_hardening.h).
//
// The production write-path routes through sao_security_anti_screencap
// helper functions from the one application-local DLL.  If that DLL is
// absent from the application directory, apply_stealth_window degrades
// gracefully to `false` without touching the window at all -- proving:
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
          "sao_security helper DLL is unavailable locally",
          "[ai_editor][stealth]") {
    DummyWindow w;
    REQUIRE(w.hwnd != nullptr);
    const int32_t status =
        sao::ai_editor::apply_stealth_window_status(w.hwnd);
    if (status == SAO_ERR_NOT_FOUND) {
        DWORD affinity = 0xFFFFFFFFu;
        REQUIRE(::GetWindowDisplayAffinity(w.hwnd, &affinity) != FALSE);
        REQUIRE(affinity == 0u);
    } else {
        CHECK(status != SAO_ERR_HANDLE_INVALID);
    }
}

TEST_CASE("webview hardening requires affinity and coordinator registration",
          "[ai_editor][stealth][webview]") {
    using sao::ai_editor::WebviewHardeningStatus;
    CHECK(sao::ai_editor::classify_webview_hardening(true, true) ==
          WebviewHardeningStatus::kApplied);
    CHECK(sao::ai_editor::classify_webview_hardening(false, true) ==
          WebviewHardeningStatus::kAffinityApplyFailed);
    CHECK(sao::ai_editor::classify_webview_hardening(true, false) ==
          WebviewHardeningStatus::kRegistrationFailed);
    CHECK(sao::ai_editor::webview_hardening_registered(
        WebviewHardeningStatus::kApplied));
    CHECK(sao::ai_editor::classify_webview_hardening_status(
              SAO_ASC_APPLY_POLICY_DISABLED, SAO_OK) ==
          WebviewHardeningStatus::kDeferredByPolicy);
    CHECK(sao::ai_editor::webview_hardening_registered(
        WebviewHardeningStatus::kDeferredByPolicy));
    CHECK(sao::ai_editor::webview_hardening_registered(
        WebviewHardeningStatus::kAffinityApplyFailed));
    CHECK_FALSE(sao::ai_editor::webview_hardening_registered(
        WebviewHardeningStatus::kRegistrationFailed));
}


TEST_CASE("main-window hardening classification is fail-closed",
          "[ai_editor][stealth][main-window]") {
    using sao::ai_editor::WebviewHardeningStatus;
    CHECK(sao::ai_editor::classify_webview_hardening(false, false) ==
          WebviewHardeningStatus::kRegistrationFailed);
    CHECK(sao::ai_editor::classify_webview_hardening(false, true) ==
          WebviewHardeningStatus::kAffinityApplyFailed);
    CHECK(sao::ai_editor::webview_hardening_registered(
        WebviewHardeningStatus::kApplied));
}
