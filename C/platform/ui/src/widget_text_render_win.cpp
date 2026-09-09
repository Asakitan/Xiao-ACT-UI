// DirectWrite-backed true-font renderer for the software paint context.
//
// Replaces the 5x5 procedural placeholder glyphs (d2d_widgets.cpp draw_text)
// with real DirectWrite text rendered into a WIC bitmap via D2D1, then
// alpha-blended into the shared BGRA raster honoring clip rects and the
// opacity stack.  Falls back (returns false) when DWrite/D2D/WIC init fails
// or COM is unavailable, so the caller can keep the procedural path as a
// safety net — text rendering must never crash a paint pass.

#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000003
#include "widget_raster_internal.h"
#include "classic_text_roles.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma pop_macro("NTDDI_VERSION")

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sao::ui::detail {

namespace {

using Microsoft::WRL::ComPtr;
using namespace sao::ui::raster;

// Lazy, process-wide COM + factory set.  Guarded by a function-local static
// (thread-safe init in C++11).  All factory creation failure paths leave
// ready()==false so the caller falls back.
// The Windows 10 loader owns its own copy of the embedded bytes. Collection
// teardown precedes unregistering the loader, and no process-wide font install occurs.
struct EmbeddedDisplayFont {
    ComPtr<IDWriteFactory5> factory;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ComPtr<IDWriteFontCollection1> collection;
    bool registered{};
    explicit EmbeddedDisplayFont(IDWriteFactory* source) noexcept {
        if (!source || FAILED(source->QueryInterface(IID_PPV_ARGS(&factory))))
            return;
        if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)))
            return;
        if (FAILED(factory->RegisterFontFileLoader(loader.Get())))
            return;
        registered = true;
        static const int resource_anchor = 0;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&resource_anchor), &module))
            return;
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(701), MAKEINTRESOURCEW(10));
        const DWORD size = resource ? SizeofResource(module, resource) : 0;
        HGLOBAL memory = resource ? LoadResource(module, resource) : nullptr;
        const void* bytes = memory ? LockResource(memory) : nullptr;
        if (!bytes || !size)
            return;
        ComPtr<IDWriteFontFile> file;
        ComPtr<IDWriteFontSetBuilder1> builder;
        ComPtr<IDWriteFontSet> set;
        if (FAILED(loader->CreateInMemoryFontFileReference(factory.Get(), bytes, size, nullptr,
                                                           &file)) ||
            FAILED(factory->CreateFontSetBuilder(&builder)) ||
            FAILED(builder->AddFontFile(file.Get())) || FAILED(builder->CreateFontSet(&set)))
            return;
        (void)factory->CreateFontCollectionFromFontSet(set.Get(), &collection);
    }
    ~EmbeddedDisplayFont() {
        collection.Reset();
        if (registered)
            (void)factory->UnregisterFontFileLoader(loader.Get());
    }
};

struct DwriteBackend {
    HRESULT com_result{E_UNEXPECTED};
    bool owns_com{};
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    std::shared_ptr<EmbeddedDisplayFont> display_font;

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
            try {
                b.display_font = std::make_shared<EmbeddedDisplayFont>(b.dwrite.Get());
            } catch (...) {
            }
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
    float size_px{0.0F};
    std::vector<uint8_t> pixels;
    uint32_t width{0};
    uint32_t height{0};
};

uint32_t text_size_bits(float size_px) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &size_px, sizeof(bits));
    return bits;
}

struct TextStyleKey {
    ClassicTextStyle style{};

    bool operator==(const TextStyleKey& other) const noexcept {
        return style.role == other.style.role && style.weight == other.style.weight;
    }
};

struct GlyphMaskKey {
    std::wstring text;
    uint32_t size_bits{};
    TextStyleKey style{};

    bool operator==(const GlyphMaskKey& other) const noexcept {
        return size_bits == other.size_bits && style == other.style && text == other.text;
    }
};

struct GlyphMaskKeyHash {
    size_t operator()(const GlyphMaskKey& key) const noexcept {
        size_t hash = std::hash<std::wstring>{}(key.text);
        hash ^= std::hash<uint32_t>{}(key.size_bits) + static_cast<size_t>(0x9e3779b9U) +
                (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<size_t>(key.style.style.role) +
                (static_cast<size_t>(key.style.style.weight) << 8U) +
                static_cast<size_t>(0x85ebca6bU) + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

struct GlyphMaskCache {
    static constexpr size_t kMaxEntries = 256;
    std::mutex mtx;
    std::unordered_map<GlyphMaskKey, std::shared_ptr<const GlyphMaskEntry>, GlyphMaskKeyHash>
        entries;

    // Keep the immutable entry alive in the caller while eviction releases the
    // cache's reference.
    std::shared_ptr<const GlyphMaskEntry> find(const std::wstring& text, float size_px,
                                               ClassicTextStyle style) {
        std::lock_guard<std::mutex> lock(mtx);
        GlyphMaskKey key{text, text_size_bits(size_px), {style}};
        auto it = entries.find(key);
        if (it == entries.end() || std::abs(it->second->size_px - size_px) > 0.25F)
            return {};
        return it->second;
    }

    std::shared_ptr<const GlyphMaskEntry>
    store(std::wstring text, float size_px, ClassicTextStyle style,
          std::shared_ptr<const GlyphMaskEntry> entry) noexcept {
        if (entry == nullptr || size_px < 4.0F || size_px > 512.0F || entry->width == 0 ||
            entry->height == 0 || text.size() > 64)
            return entry; // transient one-off sizes pollute the cache
        try {
            std::lock_guard<std::mutex> lock(mtx);
            if (entries.size() >= kMaxEntries)
                entries.clear();
            entries[GlyphMaskKey{std::move(text), text_size_bits(size_px), {style}}] = entry;
        } catch (...) {
        }
        return entry;
    }
};
GlyphMaskCache& glyph_mask_cache() noexcept {
    static GlyphMaskCache cache;
    return cache;
}

struct TextMetricsKey {
    std::wstring text;
    uint32_t size_bits{0};
    TextStyleKey style{};

    bool operator==(const TextMetricsKey& other) const noexcept {
        return size_bits == other.size_bits && style == other.style && text == other.text;
    }
};

struct TextMetricsKeyHash {
    size_t operator()(const TextMetricsKey& key) const noexcept {
        size_t hash = std::hash<std::wstring>{}(key.text);
        hash ^= std::hash<uint32_t>{}(key.size_bits) + static_cast<size_t>(0x9e3779b9U) +
                (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<size_t>(key.style.style.role) +
                (static_cast<size_t>(key.style.style.weight) << 8U) +
                static_cast<size_t>(0x85ebca6bU) + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

struct TextMetricsValue {
    float width{0.0F};
    float height{0.0F};
};

struct TextMetricsCache {
    static constexpr size_t kMaxEntries = 256;
    std::mutex mtx;
    std::unordered_map<TextMetricsKey, TextMetricsValue, TextMetricsKeyHash> entries;

    bool find(const std::wstring& text, float size_px, ClassicTextStyle style, float* out_width,
              float* out_height) noexcept {
        try {
            TextMetricsKey key;
            key.text = text;
            key.size_bits = text_size_bits(size_px);
            key.style = {style};
            std::lock_guard<std::mutex> lock(mtx);
            const auto it = entries.find(key);
            if (it == entries.end())
                return false;
            *out_width = it->second.width;
            *out_height = it->second.height;
            return true;
        } catch (...) {
            return false;
        }
    }

    void store(std::wstring text, float size_px, ClassicTextStyle style, float width,
               float height) noexcept {
        if (text.empty() || text.size() > 64 || !std::isfinite(width) || !std::isfinite(height) ||
            width < 0.0F || height < 0.0F || width > 65536.0F || height > 65536.0F)
            return;
        try {
            TextMetricsKey key;
            key.text = std::move(text);
            key.size_bits = text_size_bits(size_px);
            key.style = {style};
            std::lock_guard<std::mutex> lock(mtx);
            if (entries.size() >= kMaxEntries && entries.find(key) == entries.end())
                entries.clear();
            entries.insert_or_assign(std::move(key), TextMetricsValue{width, height});
        } catch (...) {
        }
    }
};

TextMetricsCache& text_metrics_cache() noexcept {
    static TextMetricsCache cache;
    return cache;
}

ClassicTextStyle resolved_text_style(const std::wstring& text) noexcept {
    ClassicTextStyle style = active_classic_text_style();
    if (style.role == ClassicTextRole::Display &&
        std::any_of(text.begin(), text.end(), [](wchar_t c) { return c > 127; }))
        style.role = ClassicTextRole::Body;
    if (style.role == ClassicTextRole::Auto)
        style.role = ClassicTextRole::Body;
    return style;
}

const wchar_t* family_for_role(ClassicTextRole role) noexcept {
    switch (role) {
    case ClassicTextRole::Display:
        return L"SAO UI";
    case ClassicTextRole::Monospace:
        return L"Consolas";
    case ClassicTextRole::Body:
    case ClassicTextRole::Auto:
    default:
        return L"Microsoft YaHei UI";
    }
}

DWRITE_FONT_WEIGHT weight_for_role(ClassicTextWeight weight) noexcept {
    switch (weight) {
    case ClassicTextWeight::SemiBold:
        return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case ClassicTextWeight::Bold:
        return DWRITE_FONT_WEIGHT_BOLD;
    case ClassicTextWeight::Normal:
    default:
        return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

HRESULT create_text_format(DwriteBackend& backend, float size_px, ClassicTextStyle style,
                           IDWriteTextFormat** out_format) noexcept {
    if (out_format == nullptr)
        return E_INVALIDARG;
    IDWriteFontCollection* collection =
        style.role == ClassicTextRole::Display && backend.display_font
            ? backend.display_font->collection.Get()
            : nullptr;
    const wchar_t* family = style.role == ClassicTextRole::Display && !collection
                                ? L"Segoe UI"
                                : family_for_role(style.role);
    return backend.dwrite->CreateTextFormat(family, collection, weight_for_role(style.weight),
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            size_px, L"", out_format);
}

// Render `text` at `size_px` into a tight WIC bitmap (premultiplied BGRA)
// filled with a white mask: every channel equals the coverage alpha.
// Tinting happens at blend time.  On success returns true + pixels/w/h.
bool render_text_bitmap(DwriteBackend& backend, const std::wstring& text, float size_px,
                        ClassicTextStyle style, std::vector<uint8_t>* out_pixels, uint32_t* out_w,
                        uint32_t* out_h) noexcept {
    if (out_pixels == nullptr || out_w == nullptr || out_h == nullptr || text.empty() ||
        !std::isfinite(size_px) || size_px <= 0.0F ||
        text.size() > std::numeric_limits<UINT32>::max())
        return false;
    const uint32_t kMaskColor = 0xFFFFFFFFu;
    try {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(create_text_format(backend, size_px, style, &format)))
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

        const uint64_t row_bytes = static_cast<uint64_t>(width) * 4u;
        if (row_bytes > std::numeric_limits<UINT>::max() ||
            static_cast<uint64_t>(height) >
                std::numeric_limits<size_t>::max() / row_bytes)
            return false;
        const size_t total_bytes = static_cast<size_t>(row_bytes) * height;
        if (total_bytes > std::numeric_limits<UINT>::max()) return false;
        const UINT stride = static_cast<UINT>(row_bytes);
        out_pixels->resize(total_bytes);
        if (FAILED(bitmap->CopyPixels(nullptr, stride,
                                      static_cast<UINT>(total_bytes),
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
    const ClassicTextStyle style = resolved_text_style(wide);
    // The embedded SAO display face is designed for capitals. Keep its
    // lowercase x-height out of compact titles; measure the exact same text.
    if (style.role == ClassicTextRole::Display) {
        for (wchar_t& character : wide)
            if (character >= L'a' && character <= L'z')
                character -= L'a' - L'A';
    }

    // Shared raster per-pixel premultiplied blending.
    const uint32_t effective = apply_opacity(argb, current_opacity(context));

    // Reuse cached white masks when possible; the tint multiplies per
    // channel so color/opacity changes stay cheap.
    GlyphMaskCache& cache = glyph_mask_cache();
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<const GlyphMaskEntry> cached = cache.find(wide, size_px, style);
    if (cached == nullptr) {
        std::vector<uint8_t> local_pixels;
        if (!render_text_bitmap(backend, wide, size_px, style, &local_pixels, &width, &height))
            return false;
        try {
            auto rendered = std::make_shared<GlyphMaskEntry>();
            rendered->size_px = size_px;
            rendered->pixels = std::move(local_pixels);
            rendered->width = width;
            rendered->height = height;
            cached = cache.store(std::move(wide), size_px, style, std::move(rendered));
        } catch (...) {
            return false;
        }
    }
    width = cached->width;
    height = cached->height;
    const uint8_t* pixels = cached->pixels.data();

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
    const ClassicTextStyle style = resolved_text_style(wide);
    // The embedded SAO display face is designed for capitals. Keep its
    // lowercase x-height out of compact titles; measure the exact same text.
    if (style.role == ClassicTextRole::Display) {
        for (wchar_t& character : wide)
            if (character >= L'a' && character <= L'z')
                character -= L'a' - L'A';
    }
    auto& cache = text_metrics_cache();
    if (cache.find(wide, size_px, style, out_width, out_height))
        return true;
    try {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(create_text_format(backend, size_px, style, &format)))
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
        cache.store(std::move(wide), size_px, style, metrics.width, metrics.height);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sao::ui::detail
