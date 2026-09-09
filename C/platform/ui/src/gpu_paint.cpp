#include "classic_text_roles.h"
#include "gpu_paint_internal.h"
#include "widget_paint_internal.h"
#include "widget_raster_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace sao::ui::detail {
bool prepare_gpu_text_dwrite(const char* text_utf8, float size_px, std::wstring* out_text,
                             void** out_format) noexcept;
}

namespace {
using Microsoft::WRL::ComPtr;
using sao::ui::raster::GpuPaintDispatch;
using sao::ui::raster::Rect;

constexpr size_t kMaxGpuClipDepth = 1024U;

struct GpuPaintState {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> device_context;
    ComPtr<ID2D1Bitmap1> target_bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> borrowed_dwrite;
    DWORD owner_thread{};
    uint32_t width{};
    uint32_t height{};
    size_t clip_depth{};
    bool drawing{};
};

sao_status_t map_hresult(HRESULT value) noexcept {
    if (SUCCEEDED(value))
        return SAO_STATUS_OK;
    if (value == D2DERR_RECREATE_TARGET || value == DXGI_ERROR_DEVICE_REMOVED ||
        value == DXGI_ERROR_DEVICE_RESET || value == DXGI_ERROR_DEVICE_HUNG)
        return SAO_STATUS_ERR_DEVICE_LOST;
    return SAO_STATUS_ERR_OS_CALL_FAILED;
}

bool owner(const GpuPaintState* state) noexcept {
    return state != nullptr && state->owner_thread == GetCurrentThreadId();
}

D2D1_COLOR_F color(uint32_t argb, float opacity) noexcept {
    const float a =
        static_cast<float>((argb >> 24U) & 0xffU) / 255.0F * std::clamp(opacity, 0.0F, 1.0F);
    return D2D1::ColorF(static_cast<float>((argb >> 16U) & 0xffU) / 255.0F,
                        static_cast<float>((argb >> 8U) & 0xffU) / 255.0F,
                        static_cast<float>(argb & 0xffU) / 255.0F, a);
}

bool brush(GpuPaintState* state, uint32_t argb, float opacity,
           ID2D1SolidColorBrush** out) noexcept {
    return owner(state) && state->target && out &&
           SUCCEEDED(state->target->CreateSolidColorBrush(color(argb, opacity), out));
}

void destroy(void* opaque) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (state != nullptr && state->drawing && owner(state) && state->target) {
        while (state->clip_depth != 0) {
            state->target->PopAxisAlignedClip();
            --state->clip_depth;
        }
        (void)state->target->EndDraw();
    }
    delete state;
}

sao_status_t begin_frame(void* opaque) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!owner(state) || !state->target || state->drawing)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    const D2D1_MATRIX_3X2_F identity = D2D1::Matrix3x2F::Identity();
    state->target->SetTransform(&identity);
    state->target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    state->target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (state->device_context && state->target_bitmap)
        state->device_context->SetTarget(state->target_bitmap.Get());
    state->target->BeginDraw();
    const D2D1_COLOR_F clear = D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F);
    state->target->Clear(&clear);
    state->clip_depth = 0;
    state->drawing = true;
    return SAO_STATUS_OK;
}

sao_status_t end_frame(void* opaque) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!owner(state) || !state->target || !state->drawing)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    while (state->clip_depth != 0) {
        state->target->PopAxisAlignedClip();
        --state->clip_depth;
    }
    state->drawing = false;
    return map_hresult(state->target->EndDraw());
}

sao_status_t push_clip(void* opaque, Rect rect) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!owner(state) || !state->drawing)
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (state->clip_depth >= kMaxGpuClipDepth)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const D2D1_RECT_F geometry =
        D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
    state->target->PushAxisAlignedClip(&geometry, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ++state->clip_depth;
    return SAO_STATUS_OK;
}

sao_status_t pop_clip(void* opaque) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!owner(state) || !state->drawing || state->clip_depth == 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    state->target->PopAxisAlignedClip();
    --state->clip_depth;
    return SAO_STATUS_OK;
}

bool fill_rect(void* opaque, Rect rect, uint32_t argb, float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    ComPtr<ID2D1SolidColorBrush> value;
    if (!state || !state->drawing || !brush(state, argb, opacity, &value))
        return false;
    const D2D1_RECT_F geometry =
        D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
    state->target->FillRectangle(&geometry, value.Get());
    return true;
}

bool fill_rounded(void* opaque, Rect rect, float radius, uint32_t argb, float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    ComPtr<ID2D1SolidColorBrush> value;
    if (!state || !state->drawing || !brush(state, argb, opacity, &value))
        return false;
    const D2D1_ROUNDED_RECT geometry = D2D1::RoundedRect(
        D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), radius, radius);
    state->target->FillRoundedRectangle(&geometry, value.Get());
    return true;
}

bool stroke_rounded(void* opaque, Rect rect, float radius, float width, uint32_t argb,
                    float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    ComPtr<ID2D1SolidColorBrush> value;
    if (!state || !state->drawing || !brush(state, argb, opacity, &value))
        return false;
    const D2D1_ROUNDED_RECT geometry = D2D1::RoundedRect(
        D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), radius, radius);
    state->target->DrawRoundedRectangle(&geometry, value.Get(), width);
    return true;
}

bool fill_ellipse(void* opaque, Rect rect, uint32_t argb, float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    ComPtr<ID2D1SolidColorBrush> value;
    if (!state || !state->drawing || !brush(state, argb, opacity, &value))
        return false;
    const D2D1_ELLIPSE geometry =
        D2D1::Ellipse(D2D1::Point2F(rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F),
                      rect.width * 0.5F, rect.height * 0.5F);
    state->target->FillEllipse(&geometry, value.Get());
    return true;
}

bool stroke_line(void* opaque, float x1, float y1, float x2, float y2, float width, uint32_t argb,
                 float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    ComPtr<ID2D1SolidColorBrush> value;
    if (!state || !state->drawing || !brush(state, argb, opacity, &value))
        return false;
    state->target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), value.Get(), width);
    return true;
}

bool fill_polygon(void* opaque, const int32_t* points, size_t count, uint32_t argb,
                  float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!state || !state->drawing || !state->factory || !points || count < 3 ||
        count > std::numeric_limits<UINT32>::max())
        return false;
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    ComPtr<ID2D1SolidColorBrush> value;
    if (FAILED(state->factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink)) ||
        !brush(state, argb, opacity, &value))
        return false;
    sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
    sink->BeginFigure(D2D1::Point2F(static_cast<float>(points[0]), static_cast<float>(points[1])),
                      D2D1_FIGURE_BEGIN_FILLED);
    for (size_t index = 1; index < count; ++index)
        sink->AddLine(D2D1::Point2F(static_cast<float>(points[index * 2]),
                                    static_cast<float>(points[index * 2 + 1])));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return false;
    state->target->FillGeometry(geometry.Get(), value.Get());
    return true;
}

bool draw_utf8(void* opaque, float x, float y, const char* text, float size, uint32_t argb,
               float opacity, uint8_t role, uint8_t weight) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!state || !state->drawing || !text || !*text)
        return false;
    std::wstring wide;
    void* raw_format = nullptr;
    const sao::ui::detail::ScopedTextRole text_role(
        static_cast<sao::ui::detail::ClassicTextRole>(role),
        static_cast<sao::ui::detail::ClassicTextWeight>(weight));
    if (!sao::ui::detail::prepare_gpu_text_dwrite(text, size, &wide, &raw_format) ||
        raw_format == nullptr || wide.empty())
        return false;
    ComPtr<IDWriteTextFormat> format;
    format.Attach(static_cast<IDWriteTextFormat*>(raw_format));
    ComPtr<ID2D1SolidColorBrush> value;
    if (!brush(state, argb, opacity, &value))
        return false;
    const float right = static_cast<float>(state->width);
    const float bottom = static_cast<float>(state->height);
    const D2D1_RECT_F layout = D2D1::RectF(x, y, right, bottom);
    state->target->DrawText(wide.data(), static_cast<UINT32>(wide.size()), format.Get(), &layout,
                            value.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL);
    return true;
}

bool blit_bgra(void* opaque, const uint8_t* pixels, uint32_t width, uint32_t height,
               uint32_t stride, Rect destination, float opacity) noexcept {
    auto* state = static_cast<GpuPaintState*>(opaque);
    if (!state || !state->drawing || !pixels || !width || !height ||
        width > std::numeric_limits<uint32_t>::max() / 4U || stride < width * 4U)
        return false;
    ComPtr<ID2D1Bitmap> bitmap;
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    if (FAILED(state->target->CreateBitmap(D2D1::SizeU(width, height), pixels, stride, properties,
                                           &bitmap)))
        return false;
    const D2D1_RECT_F target =
        D2D1::RectF(destination.x, destination.y, destination.x + destination.width,
                    destination.y + destination.height);
    state->target->DrawBitmap(bitmap.Get(), &target, std::clamp(opacity, 0.0F, 1.0F),
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

const GpuPaintDispatch kDispatch{
    destroy,        begin_frame,  end_frame,   push_clip,    pop_clip,  fill_rect, fill_rounded,
    stroke_rounded, fill_ellipse, stroke_line, fill_polygon, draw_utf8, blit_bgra};

sao_ui_paint_ctx_s* allocate_context(std::unique_ptr<GpuPaintState> state) noexcept {
    if (!state)
        return nullptr;
    try {
        auto context = std::make_unique<sao_ui_paint_ctx_s>();
        context->gpu_state = state.get();
        context->gpu_dispatch = &kDispatch;
        context->target_width = state->width;
        context->target_height = state->height;
        state.release();
        return context.release();
    } catch (...) {
        return nullptr;
    }
}
} // namespace

namespace sao::ui::detail {
sao_status_t create_gpu_paint_context(void* raw_device, void* raw_texture, uint32_t width,
                                      uint32_t height, sao_ui_paint_ctx_handle_t* out) noexcept {
    if (!raw_device || !raw_texture || !out || !width || !height)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    try {
        auto state = std::make_unique<GpuPaintState>();
        state->owner_thread = GetCurrentThreadId();
        state->width = width;
        state->height = height;
        auto* d3d = static_cast<ID3D11Device*>(raw_device);
        auto* texture = static_cast<ID3D11Texture2D*>(raw_texture);
        const auto device_failure = [d3d]() noexcept {
            return FAILED(d3d->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST
                                                         : SAO_STATUS_ERR_OS_CALL_FAILED;
        };
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Device> texture_device;
        texture->GetDevice(&texture_device);
        if (desc.Width != width || desc.Height != height ||
            (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
            (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || texture_device.Get() != d3d) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGISurface> surface;
        const D2D1_FACTORY_OPTIONS factory_options{};
        if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
            FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface))) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                     &factory_options,
                                     reinterpret_cast<void**>(state->factory.GetAddressOf()))) ||
            FAILED(state->factory->CreateDevice(dxgi_device.Get(), &state->device)) ||
            FAILED(state->device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                      &state->device_context))) {
            return device_failure();
        }
        if (FAILED(state->device_context.As(&state->target))) {
            return device_failure();
        }
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
        if (FAILED(state->device_context->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                                      &state->target_bitmap))) {
            return device_failure();
        }
        state->device_context->SetTarget(state->target_bitmap.Get());
        auto* context = allocate_context(std::move(state));
        if (!context)
            return SAO_STATUS_ERR_UNKNOWN;
        *out = context;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t create_borrowed_d2d_paint_context(void* raw_target, void* raw_dwrite,
                                               sao_ui_paint_ctx_handle_t* out) noexcept {
    if (!raw_target || !out)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    try {
        auto state = std::make_unique<GpuPaintState>();
        state->owner_thread = GetCurrentThreadId();
        state->target = static_cast<ID2D1RenderTarget*>(raw_target);
        if (raw_dwrite)
            state->borrowed_dwrite = static_cast<IDWriteFactory*>(raw_dwrite);
        ComPtr<ID2D1Factory> base_factory;
        state->target->GetFactory(&base_factory);
        if (base_factory)
            (void)base_factory.As(&state->factory);
        const D2D1_SIZE_U size = state->target->GetPixelSize();
        state->width = size.width;
        state->height = size.height;
        if (!state->width || !state->height)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        auto* context = allocate_context(std::move(state));
        if (!context)
            return SAO_STATUS_ERR_UNKNOWN;
        *out = context;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
} // namespace sao::ui::detail

#else
namespace sao::ui::detail {
sao_status_t create_gpu_paint_context(void*, void*, uint32_t, uint32_t,
                                      sao_ui_paint_ctx_handle_t* out) noexcept {
    if (out)
        *out = nullptr;
    return out ? SAO_STATUS_ERR_NOT_IMPLEMENTED : SAO_STATUS_ERR_INVALID_ARGUMENT;
}
sao_status_t create_borrowed_d2d_paint_context(void*, void*,
                                               sao_ui_paint_ctx_handle_t* out) noexcept {
    if (out)
        *out = nullptr;
    return out ? SAO_STATUS_ERR_NOT_IMPLEMENTED : SAO_STATUS_ERR_INVALID_ARGUMENT;
}
} // namespace sao::ui::detail
#endif
