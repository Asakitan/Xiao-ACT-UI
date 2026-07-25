#pragma once

// GDI-backed BGRA framebuffer for subprocess self-rendering.
//
// Owns a top-down DIB section (BGRA, 4-byte aligned stride), and
// exposes primitives (clear / fill_rect / rounded_rect / draw_text)
// implemented with direct BGRA fills plus GDI RoundRect / DrawTextW.
// After drawing a frame the caller memcpys pixels() into
// MmfFrameWriter::current_slot_pixels() and calls commit_frame().
//
// GDI is chosen over Direct2D for spike simplicity: no D3D device
// dependency, no COM lifetime, and DrawTextW handles IME composition
// / font fallback well enough for a debug-tier text UI.  Cost is
// GDI text quality is slightly below D2D; acceptable for the
// AI Editor / GPU Hunt panels' internal-tool audience.
//
// Threading: single-threaded per instance.  Caller must serialize
// draw calls and the ensuing MMF write.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace sao::ai_editor {

// BGRA colour helper: input R,G,B,A → 0xAARRGGBB packing.  The DIB
// section stores bytes as B,G,R,A in memory (little-endian 0xAARRGGBB),
// which matches SOPF BGRA premultiplied-alpha contract when alpha is 255.
inline uint32_t bgra_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFFu) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

// GDI COLORREF is 0x00BBGGRR; convert from packed BGRA.
inline COLORREF bgra_to_colorref(uint32_t bgra) {
    const uint8_t r = static_cast<uint8_t>((bgra >> 16) & 0xFFu);
    const uint8_t g = static_cast<uint8_t>((bgra >> 8) & 0xFFu);
    const uint8_t b = static_cast<uint8_t>(bgra & 0xFFu);
    return RGB(r, g, b);
}

class GdiFramebuffer {
public:
    GdiFramebuffer() = default;
    ~GdiFramebuffer() { shutdown(); }

    GdiFramebuffer(const GdiFramebuffer&) = delete;
    GdiFramebuffer& operator=(const GdiFramebuffer&) = delete;

    bool init(int width, int height) {
        shutdown();
        if (width <= 0 || height <= 0) {
            return false;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC screen = ::GetDC(nullptr);
        if (screen == nullptr) {
            return false;
        }
        dc_ = ::CreateCompatibleDC(screen);
        ::ReleaseDC(nullptr, screen);
        if (dc_ == nullptr) {
            return false;
        }
        void* pixels = nullptr;
        bitmap_ = ::CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &pixels,
                                      nullptr, 0);
        if (bitmap_ == nullptr || pixels == nullptr) {
            ::DeleteDC(dc_);
            dc_ = nullptr;
            return false;
        }
        old_bitmap_ = static_cast<HBITMAP>(::SelectObject(dc_, bitmap_));
        pixels_ = static_cast<uint8_t*>(pixels);
        width_ = width;
        height_ = height;
        stride_ = width * 4;
        ::SetGraphicsMode(dc_, GM_ADVANCED);
        ::SetBkMode(dc_, TRANSPARENT);
        return true;
    }

    void shutdown() {
        if (dc_ != nullptr) {
            if (old_bitmap_ != nullptr) {
                ::SelectObject(dc_, old_bitmap_);
                old_bitmap_ = nullptr;
            }
            ::DeleteDC(dc_);
            dc_ = nullptr;
        }
        if (bitmap_ != nullptr) {
            ::DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        pixels_ = nullptr;
        width_ = 0;
        height_ = 0;
        stride_ = 0;
    }

    bool is_initialized() const { return dc_ != nullptr; }
    HDC dc() const { return dc_; }
    const uint8_t* pixels() const { return pixels_; }
    uint8_t* pixels_mutable() { return pixels_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    size_t byte_size() const {
        return static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u;
    }

    // Fill the entire buffer with a single BGRA colour.
    void clear(uint32_t bgra) {
        if (pixels_ == nullptr) {
            return;
        }
        const size_t pixel_count =
            static_cast<size_t>(width_) * static_cast<size_t>(height_);
        uint32_t* p = reinterpret_cast<uint32_t*>(pixels_);
        for (size_t i = 0; i < pixel_count; ++i) {
            p[i] = bgra;
        }
    }

    void fill_rect(int x, int y, int w, int h, uint32_t bgra) {
        if (pixels_ == nullptr || w <= 0 || h <= 0) {
            return;
        }
        RECT requested{x, y, x + w, y + h};
        RECT bounds{0, 0, width_, height_};
        RECT clipped{};
        if (::IntersectRect(&clipped, &requested, &bounds) == FALSE) {
            return;
        }
        ::GdiFlush();
        for (LONG row = clipped.top; row < clipped.bottom; ++row) {
            uint32_t* row_pixels = reinterpret_cast<uint32_t*>(
                pixels_ + static_cast<size_t>(row) * static_cast<size_t>(stride_));
            for (LONG column = clipped.left; column < clipped.right; ++column) {
                row_pixels[column] = bgra;
            }
        }
    }

    void rounded_rect(int x, int y, int w, int h, int radius, uint32_t fill_bgra,
                      uint32_t border_bgra = 0u, int border_width = 0) {
        if (dc_ == nullptr || w <= 0 || h <= 0) {
            return;
        }
        HBRUSH brush = ::CreateSolidBrush(bgra_to_colorref(fill_bgra));
        HPEN pen = border_width > 0
                       ? ::CreatePen(PS_SOLID, border_width,
                                     bgra_to_colorref(border_bgra))
                       : static_cast<HPEN>(::GetStockObject(NULL_PEN));
        HGDIOBJ old_brush = ::SelectObject(dc_, brush);
        HGDIOBJ old_pen = ::SelectObject(dc_, pen);
        ::RoundRect(dc_, x, y, x + w, y + h, radius * 2, radius * 2);
        ::SelectObject(dc_, old_brush);
        ::SelectObject(dc_, old_pen);
        ::DeleteObject(brush);
        if (border_width > 0) {
            ::DeleteObject(pen);
        }
    }

    // Draw wide-char text into a rectangle using the currently-selected
    // font (or a newly-selected one).  align uses DrawText DT_* flags.
    void draw_text(const wchar_t* text, int x, int y, int w, int h,
                   uint32_t bgra, HFONT font, UINT align) {
        if (dc_ == nullptr || text == nullptr) {
            return;
        }
        HGDIOBJ old_font = nullptr;
        if (font != nullptr) {
            old_font = ::SelectObject(dc_, font);
        }
        ::SetTextColor(dc_, bgra_to_colorref(bgra));
        RECT r{x, y, x + w, y + h};
        ::DrawTextW(dc_, text, -1, &r, align);
        if (old_font != nullptr) {
            ::SelectObject(dc_, old_font);
        }
    }

    static HFONT create_font(const wchar_t* face, int pixel_size,
                             bool bold = false) {
        return ::CreateFontW(pixel_size, 0, 0, 0,
                             bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, face);
    }

    // BGRA output is not premultiplied by GDI; the SOPF contract wants
    // premultiplied alpha.  For fully-opaque UI (alpha=255) this is a
    // no-op.  Callers that produce translucent regions should call
    // premultiply_alpha() before copying to MMF.
    void premultiply_alpha() {
        if (pixels_ == nullptr) {
            return;
        }
        const size_t pixel_count =
            static_cast<size_t>(width_) * static_cast<size_t>(height_);
        uint8_t* p = pixels_;
        for (size_t i = 0; i < pixel_count; ++i, p += 4) {
            const uint32_t a = p[3];
            if (a == 0xFFu) {
                continue;
            }
            if (a == 0u) {
                p[0] = p[1] = p[2] = 0u;
                continue;
            }
            p[0] = static_cast<uint8_t>((p[0] * a + 127u) / 255u);
            p[1] = static_cast<uint8_t>((p[1] * a + 127u) / 255u);
            p[2] = static_cast<uint8_t>((p[2] * a + 127u) / 255u);
        }
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP old_bitmap_ = nullptr;
    uint8_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
};

}  // namespace sao::ai_editor

#endif  // _WIN32
