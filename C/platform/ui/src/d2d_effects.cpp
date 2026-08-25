#include "sao/ui/d2d_effects.h"
#include "sao/ui/theme.h"

#include "d2d_effects_internal.h"
#include "panel_theme_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

constexpr uint32_t kKnownFlags = SAO_UI_LAYER_EFFECT_BACKDROP_BLUR | SAO_UI_LAYER_EFFECT_SHADOW |
                                 SAO_UI_LAYER_EFFECT_COLOR_MATRIX |
                                 SAO_UI_LAYER_EFFECT_MODAL_BACKDROP;

struct Image {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> pixels;
};

uint8_t clamp_byte(float value) {
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

uint32_t shadow_argb_for_elevation(size_t elevation) noexcept {
    if (sao::ui::detail::panel_theme_high_contrast())
        return 0x00000000U;
    SaoUiThemeId theme_id = SAO_UI_THEME_DARK;
    (void)sao_ui_theme_get_active_id(&theme_id);
    const uint32_t shadow = sao_ui_theme_resolve_color(theme_id, SAO_UI_TOKEN_ALERT_SHADOW);
    const auto& preset = sao::ui::kSaoThemeElevationPresets[std::min(elevation, size_t{3})];
    return (static_cast<uint32_t>(preset.alpha) << 24u) | (shadow & 0x00ffffffu);
}

uint8_t scale_byte(uint8_t value, uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * alpha + 127u) / 255u);
}

void blend_pixel(uint8_t* destination, const uint8_t* source) {
    const uint32_t inverse_alpha = 255u - source[3];
    destination[0] = static_cast<uint8_t>(
        source[0] + (static_cast<uint32_t>(destination[0]) * inverse_alpha + 127u) / 255u);
    destination[1] = static_cast<uint8_t>(
        source[1] + (static_cast<uint32_t>(destination[1]) * inverse_alpha + 127u) / 255u);
    destination[2] = static_cast<uint8_t>(
        source[2] + (static_cast<uint32_t>(destination[2]) * inverse_alpha + 127u) / 255u);
    destination[3] = static_cast<uint8_t>(
        source[3] + (static_cast<uint32_t>(destination[3]) * inverse_alpha + 127u) / 255u);
}

Image extract_image(const std::vector<uint8_t>& canvas, uint32_t canvas_width,
                    uint32_t canvas_height, int32_t left, int32_t top, int32_t right,
                    int32_t bottom) {
    Image image{};
    left = std::clamp(left, 0, static_cast<int32_t>(canvas_width));
    top = std::clamp(top, 0, static_cast<int32_t>(canvas_height));
    right = std::clamp(right, left, static_cast<int32_t>(canvas_width));
    bottom = std::clamp(bottom, top, static_cast<int32_t>(canvas_height));
    image.width = static_cast<uint32_t>(right - left);
    image.height = static_cast<uint32_t>(bottom - top);
    if (image.width == 0u || image.height == 0u)
        return image;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4u);
    for (uint32_t row = 0; row < image.height; ++row) {
        const uint8_t* source =
            canvas.data() + (static_cast<size_t>(top + static_cast<int32_t>(row)) * canvas_width +
                             static_cast<uint32_t>(left)) *
                                4u;
        uint8_t* destination = image.pixels.data() + static_cast<size_t>(row) * image.width * 4u;
        std::memcpy(destination, source, static_cast<size_t>(image.width) * 4u);
    }
    return image;
}

void copy_image(std::vector<uint8_t>& canvas, uint32_t canvas_width, uint32_t canvas_height,
                const Image& image, int32_t left, int32_t top) {
    for (uint32_t row = 0; row < image.height; ++row) {
        const int32_t destination_y = top + static_cast<int32_t>(row);
        if (destination_y < 0 || destination_y >= static_cast<int32_t>(canvas_height))
            continue;
        for (uint32_t column = 0; column < image.width; ++column) {
            const int32_t destination_x = left + static_cast<int32_t>(column);
            if (destination_x < 0 || destination_x >= static_cast<int32_t>(canvas_width))
                continue;
            const uint8_t* source =
                image.pixels.data() + (static_cast<size_t>(row) * image.width + column) * 4u;
            uint8_t* destination =
                canvas.data() + (static_cast<size_t>(destination_y) * canvas_width +
                                 static_cast<uint32_t>(destination_x)) *
                                    4u;
            std::memcpy(destination, source, 4u);
        }
    }
}

void composite_image(std::vector<uint8_t>& canvas, uint32_t canvas_width, uint32_t canvas_height,
                     const Image& image, int32_t left, int32_t top) {
    for (uint32_t row = 0; row < image.height; ++row) {
        const int32_t destination_y = top + static_cast<int32_t>(row);
        if (destination_y < 0 || destination_y >= static_cast<int32_t>(canvas_height))
            continue;
        for (uint32_t column = 0; column < image.width; ++column) {
            const int32_t destination_x = left + static_cast<int32_t>(column);
            if (destination_x < 0 || destination_x >= static_cast<int32_t>(canvas_width))
                continue;
            const uint8_t* source =
                image.pixels.data() + (static_cast<size_t>(row) * image.width + column) * 4u;
            if (source[3] == 0u)
                continue;
            uint8_t* destination =
                canvas.data() + (static_cast<size_t>(destination_y) * canvas_width +
                                 static_cast<uint32_t>(destination_x)) *
                                    4u;
            blend_pixel(destination, source);
        }
    }
}

void box_blur(Image* image, int32_t radius) {
    if (image == nullptr || image->width == 0u || image->height == 0u || radius <= 0)
        return;
    radius = std::min(radius, 64);
    std::vector<uint8_t> horizontal(image->pixels.size());
    std::vector<uint8_t> vertical(image->pixels.size());
    const int32_t width = static_cast<int32_t>(image->width);
    const int32_t height = static_cast<int32_t>(image->height);
    const int32_t span = radius * 2 + 1;

    for (int32_t y = 0; y < height; ++y) {
        for (int32_t channel = 0; channel < 4; ++channel) {
            uint64_t sum = 0;
            for (int32_t offset = -radius; offset <= radius; ++offset) {
                const int32_t x = std::clamp(offset, 0, width - 1);
                sum += image->pixels[(static_cast<size_t>(y) * image->width +
                                      static_cast<uint32_t>(x)) *
                                         4u +
                                     static_cast<uint32_t>(channel)];
            }
            for (int32_t x = 0; x < width; ++x) {
                horizontal[(static_cast<size_t>(y) * image->width + static_cast<uint32_t>(x)) * 4u +
                           static_cast<uint32_t>(channel)] =
                    static_cast<uint8_t>((sum + static_cast<uint64_t>(span / 2)) /
                                         static_cast<uint64_t>(span));
                const int32_t remove_x = std::clamp(x - radius, 0, width - 1);
                const int32_t add_x = std::clamp(x + radius + 1, 0, width - 1);
                sum -= image->pixels[(static_cast<size_t>(y) * image->width +
                                      static_cast<uint32_t>(remove_x)) *
                                         4u +
                                     static_cast<uint32_t>(channel)];
                sum += image->pixels[(static_cast<size_t>(y) * image->width +
                                      static_cast<uint32_t>(add_x)) *
                                         4u +
                                     static_cast<uint32_t>(channel)];
            }
        }
    }

    for (int32_t x = 0; x < width; ++x) {
        for (int32_t channel = 0; channel < 4; ++channel) {
            uint64_t sum = 0;
            for (int32_t offset = -radius; offset <= radius; ++offset) {
                const int32_t y = std::clamp(offset, 0, height - 1);
                sum +=
                    horizontal[(static_cast<size_t>(y) * image->width + static_cast<uint32_t>(x)) *
                                   4u +
                               static_cast<uint32_t>(channel)];
            }
            for (int32_t y = 0; y < height; ++y) {
                vertical[(static_cast<size_t>(y) * image->width + static_cast<uint32_t>(x)) * 4u +
                         static_cast<uint32_t>(channel)] =
                    static_cast<uint8_t>((sum + static_cast<uint64_t>(span / 2)) /
                                         static_cast<uint64_t>(span));
                const int32_t remove_y = std::clamp(y - radius, 0, height - 1);
                const int32_t add_y = std::clamp(y + radius + 1, 0, height - 1);
                sum -= horizontal[(static_cast<size_t>(remove_y) * image->width +
                                   static_cast<uint32_t>(x)) *
                                      4u +
                                  static_cast<uint32_t>(channel)];
                sum += horizontal[(static_cast<size_t>(add_y) * image->width +
                                   static_cast<uint32_t>(x)) *
                                      4u +
                                  static_cast<uint32_t>(channel)];
            }
        }
    }
    image->pixels.swap(vertical);
}

void apply_color_matrix(Image* image, const float* matrix) {
    if (image == nullptr || matrix == nullptr)
        return;
    for (size_t offset = 0; offset < image->pixels.size(); offset += 4u) {
        const float blue = static_cast<float>(image->pixels[offset + 0u]) / 255.0F;
        const float green = static_cast<float>(image->pixels[offset + 1u]) / 255.0F;
        const float red = static_cast<float>(image->pixels[offset + 2u]) / 255.0F;
        const float alpha = static_cast<float>(image->pixels[offset + 3u]) / 255.0F;
        const float output_red = red * matrix[0] + green * matrix[4] + blue * matrix[8] +
                                 alpha * matrix[12] + matrix[16];
        const float output_green = red * matrix[1] + green * matrix[5] + blue * matrix[9] +
                                   alpha * matrix[13] + matrix[17];
        const float output_blue = red * matrix[2] + green * matrix[6] + blue * matrix[10] +
                                  alpha * matrix[14] + matrix[18];
        const float output_alpha = red * matrix[3] + green * matrix[7] + blue * matrix[11] +
                                   alpha * matrix[15] + matrix[19];
        const uint8_t a = clamp_byte(output_alpha * 255.0F);
        image->pixels[offset + 0u] = std::min(a, clamp_byte(output_blue * 255.0F));
        image->pixels[offset + 1u] = std::min(a, clamp_byte(output_green * 255.0F));
        image->pixels[offset + 2u] = std::min(a, clamp_byte(output_red * 255.0F));
        image->pixels[offset + 3u] = a;
    }
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

enum class NativeGraphKind { backdrop, shadow };

class NativeEffectBackend {
  public:
    bool available(void* device_pointer) {
        std::lock_guard lock(mutex_);
        return ensure(static_cast<ID3D11Device*>(device_pointer));
    }

    bool render(void* device_pointer, const Image& input, const SaoUiLayerEffects& effects,
                NativeGraphKind kind, Image* output) {
        if (output == nullptr || input.width == 0u || input.height == 0u)
            return false;
        std::lock_guard lock(mutex_);
        if (!ensure(static_cast<ID3D11Device*>(device_pointer)))
            return false;

        D2D1_BITMAP_PROPERTIES1 source_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F,
            96.0F);
        ComPtr<ID2D1Bitmap1> source;
        if (FAILED(context_->CreateBitmap(D2D1::SizeU(input.width, input.height),
                                          input.pixels.data(), input.width * 4u, source_properties,
                                          &source))) {
            return false;
        }

        D2D1_BITMAP_PROPERTIES1 target_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F,
            96.0F);
        ComPtr<ID2D1Bitmap1> target;
        if (FAILED(context_->CreateBitmap(D2D1::SizeU(input.width, input.height), nullptr, 0u,
                                          target_properties, &target))) {
            return false;
        }

        ComPtr<ID2D1Effect> first;
        ComPtr<ID2D1Effect> second;
        ComPtr<ID2D1Image> first_output;
        ComPtr<ID2D1Image> second_output;
        ID2D1Image* output_image = nullptr;
        if (kind == NativeGraphKind::backdrop) {
            if ((effects.flags & SAO_UI_LAYER_EFFECT_BACKDROP_BLUR) != 0u) {
                if (FAILED(context_->CreateEffect(CLSID_D2D1GaussianBlur, &first)))
                    return false;
                first->SetInput(0u, source.Get());
                if (FAILED(first->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                                           effects.blur_sigma)) ||
                    FAILED(first->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
                                           D2D1_BORDER_MODE_HARD)) ||
                    FAILED(first->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                                           D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED))) {
                    return false;
                }
                first->GetOutput(&first_output);
                output_image = first_output.Get();
            } else {
                output_image = source.Get();
            }
            if ((effects.flags & SAO_UI_LAYER_EFFECT_COLOR_MATRIX) != 0u) {
                if (FAILED(context_->CreateEffect(CLSID_D2D1ColorMatrix, &second)))
                    return false;
                second->SetInput(0u, output_image);
                D2D1_MATRIX_5X4_F matrix{};
                std::memcpy(&matrix, effects.color_matrix, sizeof(matrix));
                if (FAILED(second->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix)) ||
                    FAILED(second->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE,
                                            D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED))) {
                    return false;
                }
                second->GetOutput(&second_output);
                output_image = second_output.Get();
            }
        } else {
            if (FAILED(context_->CreateEffect(CLSID_D2D1Shadow, &first)))
                return false;
            first->SetInput(0u, source.Get());
            const float alpha = static_cast<float>((effects.shadow_argb >> 24u) & 0xffu) / 255.0F;
            const float red = static_cast<float>((effects.shadow_argb >> 16u) & 0xffu) / 255.0F;
            const float green = static_cast<float>((effects.shadow_argb >> 8u) & 0xffu) / 255.0F;
            const float blue = static_cast<float>(effects.shadow_argb & 0xffu) / 255.0F;
            const D2D1_VECTOR_4F color{red, green, blue, alpha};
            if (FAILED(first->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                                       effects.shadow_sigma)) ||
                FAILED(first->SetValue(D2D1_SHADOW_PROP_COLOR, color)) ||
                FAILED(first->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION,
                                       D2D1_SHADOW_OPTIMIZATION_SPEED))) {
                return false;
            }
            first->GetOutput(&first_output);
            output_image = first_output.Get();
        }

        context_->SetTarget(target.Get());
        context_->BeginDraw();
        context_->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));
        context_->DrawImage(output_image);
        const HRESULT draw_status = context_->EndDraw();
        context_->SetTarget(nullptr);
        if (FAILED(draw_status))
            return false;

        D2D1_BITMAP_PROPERTIES1 read_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F,
            96.0F);
        ComPtr<ID2D1Bitmap1> readable;
        if (FAILED(context_->CreateBitmap(D2D1::SizeU(input.width, input.height), nullptr, 0u,
                                          read_properties, &readable)) ||
            FAILED(readable->CopyFromBitmap(nullptr, target.Get(), nullptr))) {
            return false;
        }
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
            return false;
        output->width = input.width;
        output->height = input.height;
        output->pixels.resize(static_cast<size_t>(input.width) * input.height * 4u);
        for (uint32_t row = 0; row < input.height; ++row) {
            std::memcpy(output->pixels.data() + static_cast<size_t>(row) * input.width * 4u,
                        mapped.bits + static_cast<size_t>(row) * mapped.pitch,
                        static_cast<size_t>(input.width) * 4u);
        }
        readable->Unmap();
        return true;
    }

  private:
    void reset() {
        context_.Reset();
        d2d_device_.Reset();
        factory_.Reset();
        dxgi_device_.Reset();
        d3d_device_.Reset();
    }

    bool ensure(ID3D11Device* device) {
        if (device == nullptr)
            return false;
        if (device == d3d_device_.Get() && context_ != nullptr)
            return true;
        reset();
        device->AddRef();
        d3d_device_.Attach(device);
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device_)))) {
            reset();
            return false;
        }
        D2D1_FACTORY_OPTIONS options{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                                     &options,
                                     reinterpret_cast<void**>(factory_.GetAddressOf()))) ||
            FAILED(factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_)) ||
            FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_))) {
            reset();
            return false;
        }
        return true;
    }

    std::mutex mutex_;
    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<IDXGIDevice> dxgi_device_;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> context_;
};

NativeEffectBackend& native_backend() {
    static NativeEffectBackend backend;
    return backend;
}

bool render_native(void* device_pointer, const Image& input, const SaoUiLayerEffects& effects,
                   NativeGraphKind kind, Image* output) {
    return native_backend().render(device_pointer, input, effects, kind, output);
}

#else

enum class NativeGraphKind { backdrop, shadow };

bool render_native(void*, const Image&, const SaoUiLayerEffects&, NativeGraphKind, Image*) {
    return false;
}

#endif

void apply_backdrop(std::vector<uint8_t>& canvas, uint32_t canvas_width, uint32_t canvas_height,
                    uint32_t layer_width, uint32_t layer_height, int32_t layer_x, int32_t layer_y,
                    const SaoUiLayerEffects& effects, void* device_pointer) {
    if ((effects.flags & (SAO_UI_LAYER_EFFECT_BACKDROP_BLUR | SAO_UI_LAYER_EFFECT_COLOR_MATRIX)) ==
        0u) {
        return;
    }
    const bool modal = (effects.flags & SAO_UI_LAYER_EFFECT_MODAL_BACKDROP) != 0u;
    const int32_t left = modal ? 0 : std::max(0, layer_x);
    const int32_t top = modal ? 0 : std::max(0, layer_y);
    const int32_t right = modal ? static_cast<int32_t>(canvas_width)
                                : std::min(static_cast<int32_t>(canvas_width),
                                           layer_x + static_cast<int32_t>(layer_width));
    const int32_t bottom = modal ? static_cast<int32_t>(canvas_height)
                                 : std::min(static_cast<int32_t>(canvas_height),
                                            layer_y + static_cast<int32_t>(layer_height));
    Image source = extract_image(canvas, canvas_width, canvas_height, left, top, right, bottom);
    if (source.width == 0u || source.height == 0u)
        return;
    Image output;
    if (!render_native(device_pointer, source, effects, NativeGraphKind::backdrop, &output)) {
        output = source;
        if ((effects.flags & SAO_UI_LAYER_EFFECT_BACKDROP_BLUR) != 0u)
            box_blur(&output, static_cast<int32_t>(std::ceil(effects.blur_sigma * 1.5F)));
        if ((effects.flags & SAO_UI_LAYER_EFFECT_COLOR_MATRIX) != 0u)
            apply_color_matrix(&output, effects.color_matrix);
    }
    copy_image(canvas, canvas_width, canvas_height, output, left, top);
}

void apply_shadow(std::vector<uint8_t>& canvas, uint32_t canvas_width, uint32_t canvas_height,
                  const uint8_t* layer_pixels, uint32_t layer_width, uint32_t layer_height,
                  uint32_t layer_stride, int32_t layer_x, int32_t layer_y, float layer_alpha,
                  const SaoUiLayerEffects& effects, void* device_pointer) {
    if ((effects.flags & SAO_UI_LAYER_EFFECT_SHADOW) == 0u || layer_pixels == nullptr ||
        layer_width == 0u || layer_height == 0u || layer_alpha <= 0.0F) {
        return;
    }
    const int32_t margin =
        std::clamp(static_cast<int32_t>(std::ceil(effects.shadow_sigma * 3.0F)), 1, 192);
    Image source{};
    source.width = layer_width + static_cast<uint32_t>(margin * 2);
    source.height = layer_height + static_cast<uint32_t>(margin * 2);
    source.pixels.assign(static_cast<size_t>(source.width) * source.height * 4u, 0u);
    const uint8_t layer_alpha_byte =
        static_cast<uint8_t>(std::clamp(std::lround(layer_alpha * 255.0F), 0L, 255L));
    for (uint32_t row = 0; row < layer_height; ++row) {
        const uint8_t* input = layer_pixels + static_cast<size_t>(row) * layer_stride;
        uint8_t* output = source.pixels.data() +
                          (static_cast<size_t>(row + static_cast<uint32_t>(margin)) * source.width +
                           static_cast<uint32_t>(margin)) *
                              4u;
        for (uint32_t column = 0; column < layer_width; ++column) {
            output[column * 4u + 0u] = scale_byte(input[column * 4u + 0u], layer_alpha_byte);
            output[column * 4u + 1u] = scale_byte(input[column * 4u + 1u], layer_alpha_byte);
            output[column * 4u + 2u] = scale_byte(input[column * 4u + 2u], layer_alpha_byte);
            output[column * 4u + 3u] = scale_byte(input[column * 4u + 3u], layer_alpha_byte);
        }
    }

    Image shadow;
    if (!render_native(device_pointer, source, effects, NativeGraphKind::shadow, &shadow)) {
        shadow = source;
        box_blur(&shadow, static_cast<int32_t>(std::ceil(effects.shadow_sigma * 1.5F)));
        const uint8_t color_alpha = static_cast<uint8_t>((effects.shadow_argb >> 24u) & 0xffu);
        const uint8_t red = static_cast<uint8_t>((effects.shadow_argb >> 16u) & 0xffu);
        const uint8_t green = static_cast<uint8_t>((effects.shadow_argb >> 8u) & 0xffu);
        const uint8_t blue = static_cast<uint8_t>(effects.shadow_argb & 0xffu);
        for (size_t offset = 0; offset < shadow.pixels.size(); offset += 4u) {
            const uint8_t alpha = scale_byte(shadow.pixels[offset + 3u], color_alpha);
            shadow.pixels[offset + 0u] = scale_byte(blue, alpha);
            shadow.pixels[offset + 1u] = scale_byte(green, alpha);
            shadow.pixels[offset + 2u] = scale_byte(red, alpha);
            shadow.pixels[offset + 3u] = alpha;
        }
    }

    const int32_t destination_x =
        layer_x - margin + static_cast<int32_t>(std::lround(effects.shadow_offset_x));
    const int32_t destination_y =
        layer_y - margin + static_cast<int32_t>(std::lround(effects.shadow_offset_y));
    composite_image(canvas, canvas_width, canvas_height, shadow, destination_x, destination_y);
}

void set_identity_matrix(float* matrix) {
    std::fill(matrix, matrix + 20, 0.0F);
    matrix[0] = 1.0F;
    matrix[5] = 1.0F;
    matrix[10] = 1.0F;
    matrix[15] = 1.0F;
}

} // namespace

bool sao::ui::effects::validate(const SaoUiLayerEffects& effects) noexcept {
    const uint32_t declared =
        effects.struct_size == 0u ? SAO_UI_LAYER_EFFECTS_V1_SIZE : effects.struct_size;
    if (declared != SAO_UI_LAYER_EFFECTS_V1_SIZE || (effects.flags & ~kKnownFlags) != 0u ||
        effects.reserved != 0u || !std::isfinite(effects.blur_sigma) ||
        !std::isfinite(effects.shadow_sigma) || !std::isfinite(effects.shadow_offset_x) ||
        !std::isfinite(effects.shadow_offset_y) || effects.blur_sigma < 0.0F ||
        effects.blur_sigma > 64.0F || effects.shadow_sigma < 0.0F || effects.shadow_sigma > 64.0F ||
        std::fabs(effects.shadow_offset_x) > 512.0F ||
        std::fabs(effects.shadow_offset_y) > 512.0F) {
        return false;
    }
    for (float value : effects.color_matrix) {
        if (!std::isfinite(value) || std::fabs(value) > 16.0F)
            return false;
    }
    return true;
}

void sao::ui::effects::apply_precompose(std::vector<uint8_t>& canvas, uint32_t canvas_width,
                                        uint32_t canvas_height, const uint8_t* layer_pixels,
                                        uint32_t layer_width, uint32_t layer_height,
                                        uint32_t layer_stride, int32_t layer_x, int32_t layer_y,
                                        float layer_alpha, const SaoUiLayerEffects& effects,
                                        void* d3d11_device_ptr) noexcept {
    if (effects.flags == SAO_UI_LAYER_EFFECT_NONE || canvas.empty() || canvas_width == 0u ||
        canvas_height == 0u || !validate(effects)) {
        return;
    }
    try {
        apply_backdrop(canvas, canvas_width, canvas_height, layer_width, layer_height, layer_x,
                       layer_y, effects, d3d11_device_ptr);
        apply_shadow(canvas, canvas_width, canvas_height, layer_pixels, layer_width, layer_height,
                     layer_stride, layer_x, layer_y, layer_alpha, effects, d3d11_device_ptr);
    } catch (...) {
    }
}

bool sao::ui::effects::native_available(void* d3d11_device_ptr) noexcept {
#if defined(_WIN32)
    try {
        return native_backend().available(d3d11_device_ptr);
    } catch (...) {
        return false;
    }
#else
    (void)d3d11_device_ptr;
    return false;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_layer_effects_init(SaoUiLayerEffectPreset preset,
                                                              SaoUiLayerEffects* out_effects) {
    if (out_effects == nullptr || preset < SAO_UI_LAYER_EFFECT_PRESET_MENU ||
        preset > SAO_UI_LAYER_EFFECT_PRESET_MODAL) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoUiLayerEffects effects{};
    effects.struct_size = sizeof(SaoUiLayerEffects);
    effects.flags = SAO_UI_LAYER_EFFECT_BACKDROP_BLUR | SAO_UI_LAYER_EFFECT_SHADOW |
                    SAO_UI_LAYER_EFFECT_COLOR_MATRIX;
    set_identity_matrix(effects.color_matrix);
    switch (preset) {
    case SAO_UI_LAYER_EFFECT_PRESET_MENU:
        effects.blur_sigma = 6.0F;
        effects.shadow_sigma = 8.0F;
        effects.shadow_offset_y = 3.0F;
        effects.shadow_argb = shadow_argb_for_elevation(2);
        effects.color_matrix[0] = 0.94F;
        effects.color_matrix[5] = 0.98F;
        effects.color_matrix[10] = 1.02F;
        break;
    case SAO_UI_LAYER_EFFECT_PRESET_POPUP:
        effects.blur_sigma = 5.0F;
        effects.shadow_sigma = 7.0F;
        effects.shadow_offset_y = 4.0F;
        effects.shadow_argb = shadow_argb_for_elevation(2);
        effects.color_matrix[0] = 0.95F;
        effects.color_matrix[5] = 0.99F;
        effects.color_matrix[10] = 1.01F;
        break;
    case SAO_UI_LAYER_EFFECT_PRESET_MODAL:
        effects.flags |= SAO_UI_LAYER_EFFECT_MODAL_BACKDROP;
        effects.blur_sigma = 10.0F;
        effects.shadow_sigma = 12.0F;
        effects.shadow_offset_y = 6.0F;
        effects.shadow_argb = shadow_argb_for_elevation(3);
        effects.color_matrix[0] = 0.55F;
        effects.color_matrix[5] = 0.55F;
        effects.color_matrix[10] = 0.55F;
        break;
    default:
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_effects = effects;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_d2d_effects_native_available(void* d3d11_device_ptr,
                                                                        bool* out_available) {
    if (out_available == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_available = sao::ui::effects::native_available(d3d11_device_ptr);
    return SAO_STATUS_OK;
}
