#pragma once

// Capture an arbitrary HWND (typically off-screen WebView2 host) to a
// GDI framebuffer via PrintWindow(PW_RENDERFULLCONTENT), then commit
// the BGRA bytes to an MmfFrameWriter slot.  Used by webview_bridge
// (Phase C of AI Editor migration to compositor) so the WebView2
// pixel output lands on a compositor layer instead of a visible HWND.
//
// PrintWindow with PW_RENDERFULLCONTENT is the only reliable way to
// snapshot WebView2 content that is composited via DirectComposition
// (its DWM presentation is not visible to BitBlt on a hidden window).
// Frame rate ceiling is ~30 fps because PrintWindow blocks on the
// target thread's message pump; for higher rates use Windows Graphics
// Capture (WGC) API instead -- deferred to future work if 30 fps is
// insufficient.
//
// Ownership: this helper does NOT own the target HWND.  Caller must
// keep the HWND alive for the lifetime of capture calls.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#include "mmf_frame_writer.h"
#include "self_render.h"

namespace sao::ai_editor {

// PrintWindow flag constant.  Some SDK headers gate it on
// _WIN32_WINNT >= 0x0602; define locally to stay portable.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

class WindowCaptureToMmf {
public:
    WindowCaptureToMmf() = default;
    ~WindowCaptureToMmf() { shutdown(); }

    WindowCaptureToMmf(const WindowCaptureToMmf&) = delete;
    WindowCaptureToMmf& operator=(const WindowCaptureToMmf&) = delete;

    bool init(const wchar_t* mmf_name, HWND target, int width, int height) {
        shutdown();
        if (mmf_name == nullptr || target == nullptr || !::IsWindow(target) ||
            width <= 0 || height <= 0) {
            return false;
        }
        if (!fb_.init(width, height)) {
            return false;
        }
        if (!writer_.init(mmf_name, static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height))) {
            fb_.shutdown();
            return false;
        }
        mmf_name_ = mmf_name;
        target_ = target;
        return true;
    }

    void shutdown() {
        writer_.shutdown();
        fb_.shutdown();
        target_ = nullptr;
        mmf_name_ = nullptr;
    }

    // Capture the current HWND contents and publish a new MMF frame.
    // Returns false if HWND is dead or PrintWindow failed; MMF is not
    // updated on failure so the consumer keeps the last good frame.
    bool capture_and_publish() {
        if (target_ == nullptr || !::IsWindow(target_) ||
            !fb_.is_initialized() || !writer_.is_initialized()) {
            return false;
        }
        if (::PrintWindow(target_, fb_.dc(), PW_RENDERFULLCONTENT) == FALSE) {
            return false;
        }
        ::GdiFlush();
        // GDI writes 0x00 for the alpha channel of the target HDC because
        // it is not alpha-aware.  Force opaque so SOPF-consuming layer
        // does not blend to transparent.  Cheaper than a full
        // premultiply pass since we know WebView2 output is opaque.
        force_opaque_alpha_();
        uint8_t* slot_bytes = writer_.current_slot_pixels();
        ::memcpy(slot_bytes, fb_.pixels(), writer_.current_slot_bytes());
        writer_.commit_frame();
        return true;
    }

    // Rebuild both DIB and MMF at a new size.  Caller is responsible
    // for resizing the target HWND itself (e.g. via SetWindowPos and
    // WebView2 put_Bounds).
    bool resize(int new_width, int new_height) {
        if (mmf_name_ == nullptr || target_ == nullptr) {
            return false;
        }
        const wchar_t* name = mmf_name_;
        HWND t = target_;
        shutdown();
        return init(name, t, new_width, new_height);
    }

    bool is_initialized() const { return writer_.is_initialized(); }
    HWND target() const { return target_; }
    int width() const { return fb_.width(); }
    int height() const { return fb_.height(); }

private:
    void force_opaque_alpha_() {
        uint8_t* p = fb_.pixels_mutable();
        if (p == nullptr) {
            return;
        }
        const size_t pixel_count =
            static_cast<size_t>(fb_.width()) *
            static_cast<size_t>(fb_.height());
        for (size_t i = 0; i < pixel_count; ++i) {
            p[i * 4 + 3] = 0xFFu;
        }
    }

    GdiFramebuffer fb_;
    MmfFrameWriter writer_;
    const wchar_t* mmf_name_ = nullptr;
    HWND target_ = nullptr;
};

}  // namespace sao::ai_editor

#endif  // _WIN32
