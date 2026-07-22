// Catch2 tests for apply_stealth_window (window_hardening.h).
//
// Real dummy STATIC windows verify the helper reaches DWM:
//   * empty title is set (GetWindowTextW returns 0 chars)
//   * display affinity flips off WDA_NONE (WDA_EXCLUDEFROMCAPTURE on
//     Win10 2004+, WDA_MONITOR fallback otherwise)
//   * null / non-window inputs are rejected without crashing

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "window_hardening.h"

namespace {

constexpr DWORD kWdaNone = 0x00000000;

struct DummyWindow {
    HWND hwnd = nullptr;
    DummyWindow() {
        hwnd = ::CreateWindowExW(
            WS_EX_TOOLWINDOW, L"STATIC", L"stealth_window_test_title",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
            ::GetModuleHandleW(nullptr), nullptr);
    }
    ~DummyWindow() {
        if (hwnd != nullptr)
            ::DestroyWindow(hwnd);
    }
};

}  // namespace

TEST_CASE("apply_stealth_window clears window title",
          "[ai_editor][stealth]") {
    DummyWindow w;
    REQUIRE(w.hwnd != nullptr);
    wchar_t before[64]{};
    ::GetWindowTextW(w.hwnd, before, 64);
    REQUIRE(::wcslen(before) > 0);

    REQUIRE(sao::ai_editor::apply_stealth_window(w.hwnd));

    wchar_t after[64]{};
    const int chars = ::GetWindowTextW(w.hwnd, after, 64);
    REQUIRE(chars == 0);
}

TEST_CASE("apply_stealth_window flips display affinity off WDA_NONE",
          "[ai_editor][stealth]") {
    DummyWindow w;
    REQUIRE(w.hwnd != nullptr);

    DWORD before = 0xFFFFFFFFu;
    REQUIRE(::GetWindowDisplayAffinity(w.hwnd, &before) != FALSE);
    REQUIRE(before == kWdaNone);

    REQUIRE(sao::ai_editor::apply_stealth_window(w.hwnd));

    DWORD after = 0xFFFFFFFFu;
    REQUIRE(::GetWindowDisplayAffinity(w.hwnd, &after) != FALSE);
    REQUIRE(after != kWdaNone);
}

TEST_CASE("apply_stealth_window rejects null hwnd",
          "[ai_editor][stealth]") {
    REQUIRE_FALSE(sao::ai_editor::apply_stealth_window(nullptr));
}

TEST_CASE("apply_stealth_window rejects destroyed hwnd",
          "[ai_editor][stealth]") {
    HWND stale = nullptr;
    {
        DummyWindow w;
        stale = w.hwnd;
    }
    REQUIRE_FALSE(::IsWindow(stale));
    REQUIRE_FALSE(sao::ai_editor::apply_stealth_window(stale));
}
