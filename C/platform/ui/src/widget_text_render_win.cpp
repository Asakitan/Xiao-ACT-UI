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

#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
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
    // MB_ERR_INVALID_CHARS fails the conversion for malformed UTF-8 so the
    // caller can use the procedural fallback instead of rendering garbage.
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring converted(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                            converted.data(), length) != length)
        return false;
    converted.resize(static_cast<size_t>(length - 1));
    *output = std::move(converted);
    return true;
}

// Bounded glyph-mask cache.  Entries store a white (alpha-only) bitmap so
// the tint color and opacity can change per frame without re-rasterizing.
struct GlyphMaskEntry {
    std::wstring text;
    float size_px{0.0F};
    std::vector<uint8_t> pixels;
    uint32_t width{0};
    uint32_t height{0};
    uint64_t last_used{0};
};

struct GlyphMaskCache {
    static constexpr size_t kMaxEntries = 256;
    std::mutex mtx;
    std::unordered_map<std::wstring, GlyphMaskEntry> entries;
    uint64_t tick{0};

    // Copies the entry out under the lock so the caller never touches
    // map-owned storage after release (eviction-safe).
    bool find(const std::wstring& text, float size_px, GlyphMaskEntry* out) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = entries.find(text);
        if (it == entries.end() ||
            std::abs(it->second.size_px - size_px) > 0.25F)
            return false;
        it->second.last_used = ++tick;
        *out = it->second;
        return true;
    }

    void store(std::wstring text, float size_px,
               const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
        if (size_px < 4.0F || size_px > 512.0F || width == 0 || height == 0)
            return;   // transient one-off sizes pollute the cache
        if (text.size() > 64)
            return;   // long labels are near-unique; don't cache them
        std::lock_guard<std::mutex> lock(mtx);
        if (entries.size() >= kMaxEntries) {
            // Evict everything in one pass; simple and bounded.
            entries.clear();
            tick = 0;
        }
        GlyphMaskEntry entry;
        entry.text = text;
        entry.size_px = size_px;
        entry.pixels = pixels;
        entry.width = width;
        entry.height = height;
        entry.last_used = ++tick;
        entries[std::move(text)] = std::move(entry);
    }
};

GlyphMaskCache& glyph_mask_cache() noexcept {
    static GlyphMaskCache cache;
    return cache;
}

// Render `text` at `size_px` into a tight WIC bitmap (premultiplied BGRA)
// filled with a white mask: every channel equals the coverage alpha.
// Tinting happens at blend time.  On success returns true + pixels/w/h.
bool render_text_bitmap(DwriteBackend& backend, const std::wstring& text,
                        float size_px, std::vector<uint8_t>* out_pixels,
                        uint32_t* out_w, uint32_t* out_h) noexcept {
    if (out_pixels == nullptr || out_w == nullptr || out_h == nullptr || text.empty() ||
        !std::isfinite(size_px) || size_px <= 0.0F ||
        text.size() > std::numeric_limits<UINT32>::max())
        return false;
    const uint32_t kMaskColor = 0xFFFFFFFFu;
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
        if (FAILED(layout->GetMetrics(&metrics)) || !std::isfinite(metrics.width) ||
            !std::isfinite(metrics.height) || metrics.width < 0.0F || metrics.height < 0.0F)
            return false;
        const double width_value = std::ceil(static_cast<double>(metrics.width));
        const double height_value = std::ceil(static_cast<double>(metrics.height));
        if (!std::isfinite(width_value) || !std::isfinite(height_value) ||
            width_value > 4096.0 || height_value > 4096.0)
            return false;
        const uint32_t width = std::max(1U, static_cast<uint32_t>(width_value));
        const uint32_t height = std::max(1U, static_cast<uint32_t>(height_value));
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

        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0F, 1.0F, 1.0F, 1.0F), &brush)))
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
        (void)kMaskColor;
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
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(size_px) || size_px <= 0.0F)
        return false;
    DwriteBackend& backend = DwriteBackend::instance();
    if (!backend.ready())
        return false;

    std::wstring wide;
    if (!utf8_to_utf16(text_utf8, &wide))
        return false;

    // Shared raster per-pixel premultiplied blending.
    const uint32_t effective = apply_opacity(argb, current_opacity(context));

    // Reuse cached white masks when possible; the tint multiplies per
    // channel so color/opacity changes stay cheap.
    GlyphMaskCache& cache = glyph_mask_cache();
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> local_pixels;
    GlyphMaskEntry cached{};
    if (!cache.find(wide, size_px, &cached)) {
        if (!render_text_bitmap(backend, wide, size_px,
                                &local_pixels, &width, &height))
            return false;
        cache.store(std::move(wide), size_px, local_pixels, width, height);
    } else {
        width = cached.width;
        height = cached.height;
        local_pixels = std::move(cached.pixels);
    }
    const uint8_t* pixels = local_pixels.data();

    const Rect destination = intersect({x, y, static_cast<float>(width), static_cast<float>(height)},
                                       clip_bounds(context));
    if (!valid_rect(destination.width, destination.height))
        return true; // Fully clipped — nothing to do, but the render succeeded.

    const uint32_t tr = (effective >> 16U) & 0xffU;
    const uint32_t tg = (effective >> 8U) & 0xffU;
    const uint32_t tb = effective & 0xffU;
    const uint32_t ta = (effective >> 24U) & 0xffU;

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
                        pixels + static_cast<size_t>(sy) * width * 4U +
                            static_cast<size_t>(sx) * 4U,
                        sizeof(source));
            // Mask channel = coverage; tint with the effective color.
            const uint32_t m = source.b;
            source.b = static_cast<uint8_t>((m * tb) / 255u);
            source.g = static_cast<uint8_t>((m * tg) / 255u);
            source.r = static_cast<uint8_t>((m * tr) / 255u);
            source.a = static_cast<uint8_t>((m * ta) / 255u);
            blend_pixel(*context.raster, px, py, source);
        }
    }
    return true;
}

// DirectWrite text measurement without rasterization.  Mirrors the
// render path's format/layout setup so popup and menu metrics agree
// with the glyphs that actually get drawn.  Returns false when the
// backend is unavailable (callers keep their codepoint fallback).
bool measure_text_dwrite(const char* text_utf8, float size_px, float* out_width,
                         float* out_height) noexcept {
    if (out_width == nullptr || out_height == nullptr || text_utf8 == nullptr ||
        *text_utf8 == '\0' || !std::isfinite(size_px) || size_px <= 0.0F)
        return false;
    DwriteBackend& backend = DwriteBackend::instance();
    if (!backend.ready())
        return false;
    std::wstring wide;
    if (!utf8_to_utf16(text_utf8, &wide) || wide.size() > std::numeric_limits<UINT32>::max())
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
                wide.data(), static_cast<UINT32>(wide.size()), format.Get(),
                std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                &layout)))
            return false;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)) || !std::isfinite(metrics.width) ||
            !std::isfinite(metrics.height) || metrics.width < 0.0F || metrics.height < 0.0F)
            return false;
        if (metrics.width > 65536.0F || metrics.height > 65536.0F)
            return false;
        *out_width = metrics.width;
        *out_height = metrics.height;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sao::ui::detail
