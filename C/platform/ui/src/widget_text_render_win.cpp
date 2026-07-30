// DirectWrite-backed true-font renderer for the software paint context.
//
// Replaces the 5x5 procedural placeholder glyphs (d2d_widgets.cpp draw_text)
// with real DirectWrite text rendered into a WIC bitmap via D2D1, then
// alpha-blended into the shared BGRA raster honoring clip rects and the
// opacity stack.  Falls back (returns false) when DWrite/D2D/WIC init fails
// or COM is unavailable, so the caller can keep the procedural path as a
// safety net — text rendering must never crash a paint pass.

#include "widget_raster_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sao::ui::detail {

namespace {

using Microsoft::WRL::ComPtr;
using namespace sao::ui::raster;

// Lazy, process-wide COM + factory set.  Guarded by a function-local static
// (thread-safe init in C++11).  All factory creation failure paths leave
// ready()==false so the caller falls back.
struct DwriteBackend {
    HRESULT com_result{E_UNEXPECTED};
    bool owns_com{};
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;

    bool ready() const noexcept {
        return dwrite != nullptr && d2d != nullptr && wic != nullptr;
    }

    static DwriteBackend& instance() noexcept {
        static DwriteBackend backend = [] {
            DwriteBackend b{};
            b.com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            b.owns_com = b.com_result == S_OK || b.com_result == S_FALSE;
            if (FAILED(b.com_result) && b.com_result != RPC_E_CHANGED_MODE)
                return b;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&b.wic))))
                return b;
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                                         __uuidof(ID2D1Factory), &b.d2d)))
                return b;
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                           __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(b.dwrite.GetAddressOf()))))
                return b;
            return b;
        }();
        return backend;
    }
};

bool utf8_to_utf16(const char* text, std::wstring* output) noexcept {
    if (output == nullptr)
        return false;
    output->clear();
    if (text == nullptr || *text == '\0')
        return false;
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring converted(static_cast<size_t>(length - 1), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, converted.data(), length) != length)
        return false;
    *output = std::move(converted);
    return true;
}

// Render `text` at `size_px` into a tight WIC bitmap (premultiplied BGRA),
// drawn in `argb`.  On success returns true and fills out pixels/w/h.
bool render_text_bitmap(DwriteBackend& backend, const std::wstring& text,
                        float size_px, uint32_t argb, std::vector<uint8_t>* out_pixels,
                        uint32_t* out_w, uint32_t* out_h) noexcept {
    if (out_pixels == nullptr || out_w == nullptr || out_h == nullptr || text.empty() ||
        size_px <= 0.0F)
        return false;
    try {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(backend.dwrite->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size_px, L"",
                &format)))
            return false;
        if (FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)))
            return false;

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(backend.dwrite->CreateTextLayout(
                text.data(), static_cast<UINT32>(text.size()), format.Get(),
                std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                &layout)))
            return false;

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)))
            return false;
        const uint32_t width = std::max(1U, static_cast<uint32_t>(std::ceil(metrics.width)));
        const uint32_t height =
            std::max(1U, static_cast<uint32_t>(std::ceil(metrics.height)));
        if (width > 4096U || height > 4096U)
            return false;

        ComPtr<IWICBitmap> bitmap;
        if (FAILED(backend.wic->CreateBitmap(
                width, height, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnDemand, &bitmap)))
            return false;

        D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1RenderTarget> target;
        if (FAILED(backend.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target)))
            return false;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        const float a = static_cast<float>((argb >> 24U) & 0xffU) / 255.0F;
        const float r = static_cast<float>((argb >> 16U) & 0xffU) / 255.0F;
        const float g = static_cast<float>((argb >> 8U) & 0xffU) / 255.0F;
        const float b = static_cast<float>(argb & 0xffU) / 255.0F;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &brush)))
            return false;

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
        target->DrawTextLayout(D2D1::Point2F(0.0F, 0.0F), layout.Get(), brush.Get());
        if (FAILED(target->EndDraw()))
            return false;

        const UINT stride = width * 4U;
        out_pixels->resize(static_cast<size_t>(stride) * height);
        if (FAILED(bitmap->CopyPixels(nullptr, stride,
                                      static_cast<UINT>(out_pixels->size()),
                                      out_pixels->data())))
            return false;
        *out_w = width;
        *out_h = height;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

// Render true-font text into the shared raster.  Returns false when the
// DirectWrite path is unavailable so the caller can use the procedural
// fallback.  Honors clip rects and the opacity stack.
bool draw_text_dwrite(sao_ui_paint_ctx_s& context, float x, float y,
                      const char* text_utf8, float size_px, uint32_t argb) noexcept {
    if (context.raster == nullptr || text_utf8 == nullptr || *text_utf8 == '\0' ||
        !std::isfinite(size_px) || size_px <= 0.0F)
        return false;
    DwriteBackend& backend = DwriteBackend::instance();
    if (!backend.ready())
        return false;

    std::wstring wide;
    if (!utf8_to_utf16(text_utf8, &wide))
        return false;

    // Apply the current opacity to the requested color before rendering.
    const uint32_t effective = apply_opacity(argb, current_opacity(context));

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!render_text_bitmap(backend, wide, size_px, effective, &pixels, &width, &height))
        return false;

    const Rect destination = intersect({x, y, static_cast<float>(width), static_cast<float>(height)},
                                       clip_bounds(context));
    if (!valid_rect(destination.width, destination.height))
        return true; // Fully clipped — nothing to do, but the render succeeded.

    const int32_t left = static_cast<int32_t>(std::floor(destination.x));
    const int32_t top = static_cast<int32_t>(std::floor(destination.y));
    const int32_t right = static_cast<int32_t>(std::ceil(destination.x + destination.width));
    const int32_t bottom = static_cast<int32_t>(std::ceil(destination.y + destination.height));
    for (int32_t py = top; py < bottom; ++py) {
        for (int32_t px = left; px < right; ++px) {
            const int32_t sx = px - static_cast<int32_t>(std::floor(x));
            const int32_t sy = py - static_cast<int32_t>(std::floor(y));
            if (sx < 0 || sy < 0 || sx >= static_cast<int32_t>(width) ||
                sy >= static_cast<int32_t>(height))
                continue;
            BgraPixel source{};
            std::memcpy(&source,
                        pixels.data() + static_cast<size_t>(sy) * width * 4U +
                            static_cast<size_t>(sx) * 4U,
                        sizeof(source));
            blend_pixel(*context.raster, px, py, source);
        }
    }
    return true;
}

} // namespace sao::ui::detail
