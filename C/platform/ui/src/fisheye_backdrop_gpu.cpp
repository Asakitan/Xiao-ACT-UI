#include "fisheye_backdrop_gpu.h"
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <new>

#if defined(_WIN32)
#include <d3d11.h>
#include "sao_ui_linkstart_fullscreen_vs.h"
#include "sao_ui_fisheye_backdrop_ps.h"
#endif

namespace sao::ui::fisheye_gpu {
struct Renderer {
    std::mutex mutex;
    float seconds{};
    float openness{};
    bool reduced{};
    float darkness{};
    float theme_direction{};
    Scene scene{};
    bool live{};
    bool live_dirty{};
    std::vector<uint8_t> live_bgra;
    uint32_t live_width{}, live_height{};
#if defined(_WIN32)
    ID3D11Device* device{};
    ID3D11VertexShader* vertex{};
    ID3D11PixelShader* pixel{};
    ID3D11Buffer* constants{};
    ID3D11SamplerState* sampler{};
    ID3D11DepthStencilState* no_depth{};
    ID3D11Texture2D* field{};
    ID3D11RenderTargetView* field_target{};
    ID3D11ShaderResourceView* field_source{};
    ID3D11Texture2D* live_texture{};
    ID3D11ShaderResourceView* live_source{};
    UINT live_texture_width{}, live_texture_height{};
    UINT width{}, height{};
#endif
};

#if defined(_WIN32)
namespace {
struct alignas(16) Constants {
    float resolution[2], time, openness, reduced, pass, darkness, theme_direction;
    Scene scene;
};
static_assert(sizeof(Scene) == 48);
static_assert(sizeof(Constants) == 80 && alignof(Constants) == 16);
static_assert(offsetof(Constants, reduced) == 16 && offsetof(Constants, scene) == 32);
static_assert(offsetof(Scene, host_uv) == 16 && offsetof(Scene, menu_visibility) == 32);
template<class T> void release(T*& object) noexcept {
    if (object) { object->Release(); object = nullptr; }
}
void release_field(Renderer& r) noexcept {
    release(r.field_source); release(r.field_target); release(r.field);
    r.width = r.height = 0;
}
void release_live(Renderer& r) noexcept {
    release(r.live_source); release(r.live_texture);
    r.live_texture_width = r.live_texture_height = 0;
    r.live_dirty = !r.live_bgra.empty();
}
void release_device(Renderer& r) noexcept {
    release_field(r);
    release_live(r);
    release(r.no_depth); release(r.sampler); release(r.constants);
    release(r.pixel); release(r.vertex); r.device = nullptr;
}
bool ensure_device(Renderer& r, ID3D11Device* device) noexcept {
    if (r.device == device && r.vertex && r.pixel && r.constants && r.sampler && r.no_depth)
        return true;
    release_device(r);
    r.device = device;
    if (FAILED(device->CreateVertexShader(g_sao_ui_linkstart_fullscreen_vs,
            sizeof(g_sao_ui_linkstart_fullscreen_vs), nullptr, &r.vertex)) ||
        FAILED(device->CreatePixelShader(g_sao_ui_fisheye_backdrop_ps,
            sizeof(g_sao_ui_fisheye_backdrop_ps), nullptr, &r.pixel))) {
        release_device(r); return false;
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = static_cast<UINT>(sizeof(Constants)); buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &r.constants)) ||
        FAILED(device->CreateSamplerState(&sampler, &r.sampler)) ||
        FAILED(device->CreateDepthStencilState(&depth, &r.no_depth))) {
        release_device(r); return false;
    }
    return true;
}
bool ensure_field(Renderer& r, UINT width, UINT height) noexcept {
    if (r.field && r.width == width && r.height == height) return true;
    release_field(r);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(r.device->CreateTexture2D(&desc, nullptr, &r.field)) ||
        FAILED(r.device->CreateRenderTargetView(r.field, nullptr, &r.field_target)) ||
        FAILED(r.device->CreateShaderResourceView(r.field, nullptr, &r.field_source))) {
        release_field(r); return false;
    }
    r.width = width; r.height = height; return true;
}
bool ensure_live(Renderer& r, ID3D11DeviceContext* context) noexcept {
    if (!r.live_texture || r.live_texture_width != r.live_width ||
        r.live_texture_height != r.live_height) {
        release_live(r);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = r.live_width; desc.Height = r.live_height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(r.device->CreateTexture2D(&desc, nullptr, &r.live_texture)) ||
            FAILED(r.device->CreateShaderResourceView(r.live_texture, nullptr, &r.live_source))) {
            release_live(r); return false;
        }
        r.live_texture_width = r.live_width; r.live_texture_height = r.live_height;
    }
    if (r.live_dirty) {
        context->UpdateSubresource(r.live_texture, 0, nullptr, r.live_bgra.data(), r.live_width * 4u, 0);
        r.live_dirty = false;
    }
    return true;
}
}
#endif

sao_status_t create(Renderer** output) noexcept {
    if (!output) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *output = new (std::nothrow) Renderer;
    return *output ? SAO_STATUS_OK : SAO_STATUS_ERR_UNKNOWN;
}
void destroy(Renderer* renderer) noexcept {
    if (!renderer) return;
#if defined(_WIN32)
    release_device(*renderer);
#endif
    delete renderer;
}
void set_source(Renderer* r, bool live, bool clear_frame) noexcept {
    if (!r) return;
    std::lock_guard lock(r->mutex);
    if (r->live != live || clear_frame) {
        r->live_bgra.clear();
        r->live_width = r->live_height = 0;
        r->live_dirty = false;
#if defined(_WIN32)
        release_live(*r);
#endif
    }
    r->live = live;
}
bool has_live_frame(Renderer* r) noexcept {
    if (!r) return false;
    std::lock_guard lock(r->mutex);
    return r->live && !r->live_bgra.empty();
}
sao_status_t publish_live(Renderer* r, std::vector<uint8_t>& frame,
                          uint32_t width, uint32_t height) noexcept {
    if (!r || !width || !height || width > UINT32_MAX / 4u ||
        static_cast<uint64_t>(width) * height * 4u != frame.size())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(r->mutex);
        if (!r->live) return SAO_STATUS_ERR_CANCELLED;
        r->live_bgra.swap(frame);
        r->live_width = width; r->live_height = height; r->live_dirty = true;
        return SAO_STATUS_OK;
    } catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
}
void update(Renderer* r, float seconds, float openness, bool reduced_motion,
            float darkness, float theme_direction, Scene scene) noexcept {
    if (!r) return;
    std::lock_guard lock(r->mutex);
    r->seconds = seconds; r->openness = std::clamp(openness, 0.0F, 1.0F); r->reduced = reduced_motion;
    r->darkness = std::clamp(darkness, 0.0F, 1.0F);
    r->theme_direction = reduced_motion ? 0.0F : std::clamp(theme_direction, -1.0F, 1.0F);
    r->scene = scene;
}
sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* frame, void* user) noexcept {
#if defined(_WIN32)
    auto* r = static_cast<Renderer*>(user);
    if (!r || !frame || frame->struct_size < SAO_UI_D3D11_LAYER_RENDER_CONTEXT_V1_SIZE ||
        !frame->width_px || !frame->height_px) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(r->mutex);
        auto* device = static_cast<ID3D11Device*>(frame->d3d11_device);
        auto* context = static_cast<ID3D11DeviceContext*>(frame->d3d11_context);
        auto* output = static_cast<ID3D11RenderTargetView*>(frame->render_target_view);
        if (!device || !context || !output) return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto failure = [&] { return FAILED(device->GetDeviceRemovedReason())
            ? SAO_STATUS_ERR_DEVICE_LOST : SAO_STATUS_ERR_OS_CALL_FAILED; };
        const UINT width = std::max(1u, frame->width_px * 85u / 100u);
        const UINT height = std::max(1u, frame->height_px * 85u / 100u);
        if (!ensure_device(*r, device)) return failure();
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
        context->OMSetRenderTargets(1, &output, nullptr);
        if (r->openness <= 0.0F || (r->live && r->live_bgra.empty())) {
            const float transparent[4]{};
            context->ClearRenderTargetView(output, transparent);
            return SAO_STATUS_OK;
        }
        if (r->live ? !ensure_live(*r, context) : !ensure_field(*r, width, height)) return failure();
        const auto uniforms = [&](UINT w, UINT h, float pass) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(r->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
            *static_cast<Constants*>(mapped.pData) = {{static_cast<float>(w), static_cast<float>(h)},
                r->seconds, r->openness, r->reduced ? 1.0F : 0.0F, pass, r->darkness, r->theme_direction,
                r->scene};
            context->Unmap(r->constants, 0); return true;
        };
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(r->vertex, nullptr, 0); context->PSSetShader(r->pixel, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0); context->SetPredication(nullptr, FALSE);
        context->VSSetConstantBuffers(0, 1, &r->constants); context->PSSetConstantBuffers(0, 1, &r->constants);
        context->PSSetSamplers(0, 1, &r->sampler); context->OMSetDepthStencilState(r->no_depth, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu); context->RSSetState(nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        if (!r->live) {
            context->RSSetViewports(1, &viewport);
            context->OMSetRenderTargets(1, &r->field_target, nullptr);
            if (!uniforms(width, height, 0.0F)) {
                context->OMSetRenderTargets(1, &output, nullptr); return failure();
            }
            context->Draw(6, 0);
        }
        viewport.Width = static_cast<float>(frame->width_px); viewport.Height = static_cast<float>(frame->height_px);
        context->RSSetViewports(1, &viewport); context->OMSetRenderTargets(1, &output, nullptr);
        ID3D11ShaderResourceView* source = r->live ? r->live_source : r->field_source;
        context->PSSetShaderResources(0, 1, &source);
        if (!uniforms(frame->width_px, frame->height_px, r->live ? 2.0F : 1.0F)) {
            context->PSSetShaderResources(0, 1, &empty); return failure();
        }
        context->Draw(6, 0); context->PSSetShaderResources(0, 1, &empty);
        return FAILED(device->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST : SAO_STATUS_OK;
    } catch (...) { return SAO_STATUS_ERR_UNKNOWN; }
#else
    (void)frame; (void)user; return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
}
