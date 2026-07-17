#include "entity_text_renderer_win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace sao::ui::entity_text {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
  public:
    ComApartment() noexcept { initialize(); }

    ~ComApartment() {
        if (owns_initialization_)
            CoUninitialize();
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool ensure_ready() noexcept {
        if (!ready())
            initialize();
        return ready();
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

  private:
    [[nodiscard]] bool ready() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    void initialize() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_initialization_ = result_ == S_OK || result_ == S_FALSE;
    }

    HRESULT result_{E_UNEXPECTED};
    bool owns_initialization_{};
};

struct PreparedCommand {
    const TextCommand* source{};
    std::wstring text;
};

struct FormatEntry {
    FontRole role{FontRole::Label};
    float pixel_size{};
    bool ellipsis{};
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteInlineObject> trimming_sign;
};

bool valid_surface(BgraSurface surface) noexcept {
    if (surface.pixels == nullptr || surface.width == 0 || surface.height == 0)
        return false;
    if (surface.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        surface.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    const uint64_t row_bytes = static_cast<uint64_t>(surface.width) * 4U;
    return row_bytes <= surface.stride;
}

bool utf8_to_utf16(const std::string& utf8, std::wstring* output) {
    if (output == nullptr)
        return false;
    output->clear();
    if (utf8.empty())
        return true;
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring converted(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), converted.data(), length) != length) {
        return false;
    }
    *output = std::move(converted);
    return true;
}

bool prepare_commands(const std::vector<TextCommand>& commands,
                      std::vector<PreparedCommand>* prepared) {
    if (prepared == nullptr)
        return false;
    prepared->clear();
    prepared->reserve(commands.size());
    for (const auto& command : commands) {
        if (!std::isfinite(command.pixel_size) || command.pixel_size <= 0.0F)
            return false;
        PreparedCommand item{&command, {}};
        if (!utf8_to_utf16(command.utf8, &item.text))
            return false;
        prepared->push_back(std::move(item));
    }
    return true;
}

HRESULT create_text_format(IDWriteFactory* factory, FontRole role, float pixel_size, bool ellipsis,
                           FormatEntry* entry) {
    if (factory == nullptr || entry == nullptr)
        return E_INVALIDARG;
    const wchar_t* family = role == FontRole::Icon ? L"Segoe UI Symbol" : L"Microsoft YaHei UI";
    ComPtr<IDWriteTextFormat> format;
    HRESULT result = factory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               pixel_size, L"", &format);
    if (FAILED(result))
        return result;
    result = format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(result))
        return result;
    result = format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (FAILED(result))
        return result;
    result = format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (FAILED(result))
        return result;

    ComPtr<IDWriteInlineObject> trimming_sign;
    if (ellipsis) {
        result = factory->CreateEllipsisTrimmingSign(format.Get(), &trimming_sign);
        if (FAILED(result))
            return result;
        constexpr DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        result = format->SetTrimming(&trimming, trimming_sign.Get());
        if (FAILED(result))
            return result;
    }

    entry->role = role;
    entry->pixel_size = pixel_size;
    entry->ellipsis = ellipsis;
    entry->format = std::move(format);
    entry->trimming_sign = std::move(trimming_sign);
    return S_OK;
}

HRESULT get_text_format(IDWriteFactory* factory, const TextCommand& command,
                        std::vector<FormatEntry>* formats, IDWriteTextFormat** output) {
    if (formats == nullptr || output == nullptr)
        return E_INVALIDARG;
    *output = nullptr;
    const auto found = std::find_if(formats->begin(), formats->end(), [&command](const auto& item) {
        return item.role == command.font_role && item.pixel_size == command.pixel_size &&
               item.ellipsis == command.ellipsis;
    });
    if (found != formats->end()) {
        *output = found->format.Get();
        return S_OK;
    }
    FormatEntry entry{};
    const HRESULT result = create_text_format(factory, command.font_role, command.pixel_size,
                                              command.ellipsis, &entry);
    if (FAILED(result))
        return result;
    formats->push_back(std::move(entry));
    *output = formats->back().format.Get();
    return S_OK;
}

void source_over_floor(BgraSurface target, const uint8_t* source, uint32_t source_stride) {
    for (uint32_t y = 0; y < target.height; ++y) {
        uint8_t* destination_row = target.pixels + static_cast<size_t>(y) * target.stride;
        const uint8_t* source_row = source + static_cast<size_t>(y) * source_stride;
        for (uint32_t x = 0; x < target.width; ++x) {
            uint8_t* destination = destination_row + static_cast<size_t>(x) * 4U;
            const uint8_t* input = source_row + static_cast<size_t>(x) * 4U;
            const uint32_t inverse = 255U - input[3];
            destination[0] = static_cast<uint8_t>(input[0] + static_cast<uint32_t>(destination[0]) *
                                                                 inverse / 255U);
            destination[1] = static_cast<uint8_t>(input[1] + static_cast<uint32_t>(destination[1]) *
                                                                 inverse / 255U);
            destination[2] = static_cast<uint8_t>(input[2] + static_cast<uint32_t>(destination[2]) *
                                                                 inverse / 255U);
            destination[3] = static_cast<uint8_t>(input[3] + static_cast<uint32_t>(destination[3]) *
                                                                 inverse / 255U);
        }
    }
}

D2D1_DRAW_TEXT_OPTIONS text_draw_options() noexcept {
#if defined(NTDDI_WINBLUE)
    return static_cast<D2D1_DRAW_TEXT_OPTIONS>(
        static_cast<uint32_t>(D2D1_DRAW_TEXT_OPTIONS_CLIP) |
        static_cast<uint32_t>(D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT));
#else
    return D2D1_DRAW_TEXT_OPTIONS_CLIP;
#endif
}

class RendererContext final {
  public:
    RendererContext() = default;
    RendererContext(const RendererContext&) = delete;
    RendererContext& operator=(const RendererContext&) = delete;

    bool render(BgraSurface target, const std::vector<PreparedCommand>& prepared) {
        HRESULT result = ensure_initialized();
        if (FAILED(result)) {
            discard_surface();
            return false;
        }
        result = ensure_surface(target.width, target.height);
        if (FAILED(result)) {
            discard_surface();
            return false;
        }

        render_target_->BeginDraw();
        render_target_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
        for (const auto& item : prepared) {
            const TextCommand& command = *item.source;
            if (item.text.empty() || command.max_width <= 0 || command.max_height <= 0)
                continue;
            IDWriteTextFormat* format = nullptr;
            result = get_text_format(dwrite_factory_.Get(), command, &formats_, &format);
            if (FAILED(result))
                break;

            ComPtr<IDWriteTextLayout> layout;
            result = dwrite_factory_->CreateTextLayout(
                item.text.data(), static_cast<uint32_t>(item.text.size()), format,
                static_cast<float>(command.max_width), static_cast<float>(command.max_height),
                &layout);
            if (FAILED(result))
                break;

            const float alpha = static_cast<float>(command.color.a) / 255.0F;
            ComPtr<ID2D1SolidColorBrush> brush;
            result = render_target_->CreateSolidColorBrush(
                D2D1::ColorF(static_cast<float>(command.color.r) / 255.0F,
                             static_cast<float>(command.color.g) / 255.0F,
                             static_cast<float>(command.color.b) / 255.0F, alpha),
                &brush);
            if (FAILED(result))
                break;
            render_target_->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(command.x), static_cast<float>(command.y)),
                layout.Get(), brush.Get(), text_draw_options());
        }

        const HRESULT draw_result = render_target_->EndDraw();
        if (draw_result == D2DERR_RECREATE_TARGET) {
            discard_surface();
            return false;
        }
        if (FAILED(draw_result)) {
            discard_surface();
            return false;
        }
        if (FAILED(result))
            return false;

        const WICRect lock_rect{0, 0, static_cast<int>(target.width),
                                static_cast<int>(target.height)};
        ComPtr<IWICBitmapLock> lock;
        result = bitmap_->Lock(&lock_rect, WICBitmapLockRead, &lock);
        if (FAILED(result))
            return false;
        UINT source_stride = 0;
        UINT source_size = 0;
        BYTE* source_pixels = nullptr;
        result = lock->GetStride(&source_stride);
        if (SUCCEEDED(result))
            result = lock->GetDataPointer(&source_size, &source_pixels);
        const uint64_t required_size =
            static_cast<uint64_t>(source_stride) * static_cast<uint64_t>(target.height);
        if (FAILED(result) || source_pixels == nullptr || required_size > source_size)
            return false;

        source_over_floor(target, source_pixels, source_stride);
        return true;
    }

    void discard_surface() noexcept {
        render_target_.Reset();
        bitmap_.Reset();
        surface_width_ = 0;
        surface_height_ = 0;
    }

  private:
    HRESULT ensure_initialized() {
        if (!apartment_.ensure_ready())
            return apartment_.result();
        HRESULT result = S_OK;
        if (wic_factory_ == nullptr) {
            result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic_factory_));
            if (FAILED(result))
                return result;
        }
        if (d2d_factory_ == nullptr) {
            result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       d2d_factory_.GetAddressOf());
            if (FAILED(result))
                return result;
        }
        if (dwrite_factory_ == nullptr) {
            result = DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
            if (FAILED(result))
                return result;
        }
        return S_OK;
    }

    HRESULT ensure_surface(uint32_t width, uint32_t height) {
        if (bitmap_ != nullptr && render_target_ != nullptr && surface_width_ == width &&
            surface_height_ == height) {
            return S_OK;
        }
        discard_surface();

        ComPtr<IWICBitmap> bitmap;
        HRESULT result = wic_factory_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                                    WICBitmapCacheOnLoad, &bitmap);
        if (FAILED(result))
            return result;
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F,
            96.0F);
        ComPtr<ID2D1RenderTarget> render_target;
        result =
            d2d_factory_->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &render_target);
        if (FAILED(result))
            return result;
        render_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        bitmap_ = std::move(bitmap);
        render_target_ = std::move(render_target);
        surface_width_ = width;
        surface_height_ = height;
        return S_OK;
    }

    ComApartment apartment_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<IDWriteFactory> dwrite_factory_;
    std::vector<FormatEntry> formats_;
    ComPtr<IWICBitmap> bitmap_;
    ComPtr<ID2D1RenderTarget> render_target_;
    uint32_t surface_width_{};
    uint32_t surface_height_{};
};

} // namespace

bool render_text(BgraSurface target, const std::vector<TextCommand>& commands) noexcept {
    try {
        if (!valid_surface(target))
            return false;
        std::vector<PreparedCommand> prepared;
        if (!prepare_commands(commands, &prepared))
            return false;
        if (prepared.empty())
            return true;
        thread_local RendererContext context;
        try {
            return context.render(target, prepared);
        } catch (...) {
            context.discard_surface();
            return false;
        }
    } catch (...) {
        return false;
    }
}

} // namespace sao::ui::entity_text
