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
#include <limits>

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
    ~WindowCaptureToMmf() {
        shutdown();
    }

    WindowCaptureToMmf(const WindowCaptureToMmf&) = delete;
    WindowCaptureToMmf& operator=(const WindowCaptureToMmf&) = delete;

    bool init(const wchar_t* mmf_name, HWND target, int width, int height) {
        shutdown();
        if (mmf_name == nullptr || target == nullptr || !::IsWindow(target) ||
            !dimensions_within_budget(width, height)) {
            return false;
        }
        if (!fb_.init(width, height)) {
            return false;
        }
        if (!writer_.init(mmf_name, static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
            fb_.shutdown();
            return false;
        }
        target_ = target;
        fb_.clear(bgra_rgba(255u, 255u, 255u));
        if (!publish_framebuffer_()) {
            shutdown();
            return false;
        }
        return true;
    }

    void shutdown() {
        writer_.shutdown();
        fb_.shutdown();
        target_ = nullptr;
    }

    // Capture the current HWND contents and publish a new MMF frame.
    // Returns false if HWND is dead or PrintWindow failed; MMF is not
    // updated on failure so the consumer keeps the last good frame.
    bool capture_and_publish() {
        if (target_ == nullptr || !::IsWindow(target_) || !fb_.is_initialized() ||
            !writer_.is_initialized()) {
            return false;
        }
        RECT client{};
        if (!::GetClientRect(target_, &client) || client.left != 0 || client.top != 0 ||
            client.right != fb_.width() || client.bottom != fb_.height()) {
            return false;
        }
        if (::PrintWindow(target_, fb_.dc(), PW_RENDERFULLCONTENT) == FALSE) {
            return false;
        }
        if (::GdiFlush() == FALSE) {
            return false;
        }
        // GDI writes 0x00 for the alpha channel of the target HDC because
        // it is not alpha-aware.  Force opaque so SOPF-consuming layer
        // does not blend to transparent.  Cheaper than a full
        // premultiply pass since we know WebView2 output is opaque.
        force_opaque_alpha_();
        return publish_framebuffer_();
    }

    static bool dimensions_within_budget(int width, int height) {
        if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4) {
            return false;
        }
        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if (pixels > std::numeric_limits<uint64_t>::max() / 4ull) {
            return false;
        }
        const uint64_t pixel_bytes = pixels * 4ull;
        if (pixel_bytes > std::numeric_limits<uint64_t>::max() - 4095ull) {
            return false;
        }
        const uint64_t slot_stride = ((pixel_bytes + 4095ull) / 4096ull) * 4096ull;
        if (slot_stride > (std::numeric_limits<uint64_t>::max() -
                           static_cast<uint64_t>(SAO_UI_SOPF_MMF_HEADER_BYTES)) /
                              3ull) {
            return false;
        }
        const uint64_t total =
            static_cast<uint64_t>(SAO_UI_SOPF_MMF_HEADER_BYTES) + 3ull * slot_stride;
        return total <= SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES &&
               total <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
    }

    bool is_initialized() const {
        return writer_.is_initialized();
    }
    HWND target() const {
        return target_;
    }
    int width() const {
        return fb_.width();
    }
    int height() const {
        return fb_.height();
    }

  private:
    bool publish_framebuffer_() {
        uint8_t* slot_bytes = writer_.current_slot_pixels();
        if (slot_bytes == nullptr || writer_.current_slot_bytes() != fb_.byte_size()) {
            return false;
        }
        ::memcpy(slot_bytes, fb_.pixels(), writer_.current_slot_bytes());
        return writer_.commit_frame();
    }

    void force_opaque_alpha_() {
        uint8_t* p = fb_.pixels_mutable();
        if (p == nullptr) {
            return;
        }
        const size_t pixel_count =
            static_cast<size_t>(fb_.width()) * static_cast<size_t>(fb_.height());
        for (size_t i = 0; i < pixel_count; ++i) {
            p[i * 4 + 3] = 0xFFu;
        }
    }

    GdiFramebuffer fb_;
    MmfFrameWriter writer_;
    HWND target_ = nullptr;
};

} // namespace sao::ai_editor

#endif // _WIN32
