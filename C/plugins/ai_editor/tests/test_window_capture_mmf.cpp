// Catch2 tests for WindowCaptureToMmf (window_capture_mmf.h).
//
// Uses a real short-lived STATIC window as the capture target so
// PrintWindow round-trips through the actual OS path.  Verifies:
//   * init succeeds with a valid HWND
//   * init rejects invalid inputs
//   * capture_and_publish bumps generation (proving it wrote a slot)
//   * capture fails gracefully on destroyed HWND
//   * resize rebuilds under the same MMF name

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#include "window_capture_mmf.h"
#include "sao/ui/compositor.h"

namespace {

std::wstring unique_capture_mmf(const wchar_t* prefix) {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    wchar_t buf[128]{};
    ::swprintf_s(buf, L"Local\\%ls_%08lX_%08lX", prefix,
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long>(counter.LowPart ^ counter.HighPart));
    return buf;
}

struct DummyTarget {
    HWND hwnd = nullptr;
    DummyTarget(int w, int h) {
        hwnd = ::CreateWindowExW(
            WS_EX_TOOLWINDOW, L"STATIC", L"cap_target", WS_POPUP,
            0, 0, w, h, nullptr, nullptr,
            ::GetModuleHandleW(nullptr), nullptr);
    }
    ~DummyTarget() {
        if (hwnd != nullptr) {
            ::DestroyWindow(hwnd);
        }
    }
};

struct MmfReader {
    HANDLE handle = nullptr;
    uint8_t* view = nullptr;
    ~MmfReader() {
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
        }
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
    }
    bool open(const std::wstring& name, size_t total_bytes) {
        handle = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (handle == nullptr) {
            return false;
        }
        view = static_cast<uint8_t*>(
            ::MapViewOfFile(handle, FILE_MAP_READ, 0, 0, total_bytes));
        return view != nullptr;
    }
    const SaoUiSopfMmfHeaderV1* header() const {
        return reinterpret_cast<const SaoUiSopfMmfHeaderV1*>(view);
    }
};

}  // namespace

TEST_CASE("WindowCaptureToMmf init succeeds with a live HWND",
          "[ai_editor][window_capture]") {
    DummyTarget target(64, 48);
    REQUIRE(target.hwnd != nullptr);
    const auto name = unique_capture_mmf(L"CapInitTest");

    sao::ai_editor::WindowCaptureToMmf cap;
    REQUIRE(cap.init(name.c_str(), target.hwnd, 64, 48));
    REQUIRE(cap.is_initialized());
    REQUIRE(cap.target() == target.hwnd);
    REQUIRE(cap.width() == 64);
    REQUIRE(cap.height() == 48);
}

TEST_CASE("WindowCaptureToMmf init rejects invalid inputs",
          "[ai_editor][window_capture]") {
    DummyTarget target(64, 48);
    REQUIRE(target.hwnd != nullptr);
    sao::ai_editor::WindowCaptureToMmf cap;
    REQUIRE_FALSE(cap.init(nullptr, target.hwnd, 64, 48));
    REQUIRE_FALSE(cap.init(L"Local\\bad", nullptr, 64, 48));
    REQUIRE_FALSE(cap.init(L"Local\\bad", target.hwnd, 0, 48));
    REQUIRE_FALSE(cap.init(L"Local\\bad", target.hwnd, 64, 0));
    REQUIRE_FALSE(cap.is_initialized());
}

TEST_CASE("WindowCaptureToMmf capture_and_publish bumps generation",
          "[ai_editor][window_capture]") {
    DummyTarget target(32, 24);
    REQUIRE(target.hwnd != nullptr);
    const auto name = unique_capture_mmf(L"CapPublishTest");

    sao::ai_editor::WindowCaptureToMmf cap;
    REQUIRE(cap.init(name.c_str(), target.hwnd, 32, 24));

    MmfReader r;
    const size_t total = SAO_UI_SOPF_MMF_HEADER_BYTES + 3u * 4096u;
    REQUIRE(r.open(name, total));
    REQUIRE(r.header()->published_generation == 0ull);

    REQUIRE(cap.capture_and_publish());
    REQUIRE(r.header()->published_generation == 1ull);

    REQUIRE(cap.capture_and_publish());
    REQUIRE(r.header()->published_generation == 2ull);
}

TEST_CASE("WindowCaptureToMmf capture fails on destroyed HWND",
          "[ai_editor][window_capture]") {
    HWND stale = nullptr;
    {
        DummyTarget target(16, 16);
        stale = target.hwnd;
    }
    REQUIRE_FALSE(::IsWindow(stale));

    const auto name = unique_capture_mmf(L"CapStaleTest");
    sao::ai_editor::WindowCaptureToMmf cap;
    REQUIRE_FALSE(cap.init(name.c_str(), stale, 16, 16));
    REQUIRE_FALSE(cap.capture_and_publish());
}

TEST_CASE("WindowCaptureToMmf resize rebuilds at new dimensions",
          "[ai_editor][window_capture]") {
    DummyTarget target(64, 48);
    REQUIRE(target.hwnd != nullptr);
    const auto name = unique_capture_mmf(L"CapResizeTest");
    sao::ai_editor::WindowCaptureToMmf cap;
    REQUIRE(cap.init(name.c_str(), target.hwnd, 64, 48));
    REQUIRE(cap.width() == 64);

    REQUIRE(cap.resize(128, 96));
    REQUIRE(cap.width() == 128);
    REQUIRE(cap.height() == 96);
    REQUIRE(cap.is_initialized());
}
