#include "fisheye_backdrop_gpu.h"
#include <algorithm>
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
    UINT width{}, height{};
#endif
};

#if defined(_WIN32)
namespace {
template<class T> void release(T*& object) noexcept {
    if (object) { object->Release(); object = nullptr; }
}
void release_field(Renderer& r) noexcept {
    release(r.field_source); release(r.field_target); release(r.field);
    r.width = r.height = 0;
}
void release_device(Renderer& r) noexcept {
    release_field(r);
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
    buffer.ByteWidth = 32u; buffer.Usage = D3D11_USAGE_DYNAMIC;
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
void update(Renderer* r, float seconds, float openness, bool reduced_motion) noexcept {
    if (!r) return;
    std::lock_guard lock(r->mutex);
    r->seconds = seconds; r->openness = std::clamp(openness, 0.0F, 1.0F); r->reduced = reduced_motion;
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
        if (!ensure_device(*r, device) || !ensure_field(*r, width, height)) return failure();
        struct Constants { float resolution[2], time, openness, reduced, pass, padding[2]; };
        static_assert(sizeof(Constants) == 32);
        const auto uniforms = [&](UINT w, UINT h, float pass) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(r->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
            *static_cast<Constants*>(mapped.pData) = {{static_cast<float>(w), static_cast<float>(h)},
                r->seconds, r->openness, r->reduced ? 1.0F : 0.0F, pass, {0, 0}};
            context->Unmap(r->constants, 0); return true;
        };
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(r->vertex, nullptr, 0); context->PSSetShader(r->pixel, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0); context->SetPredication(nullptr, FALSE);
        context->VSSetConstantBuffers(0, 1, &r->constants); context->PSSetConstantBuffers(0, 1, &r->constants);
        context->PSSetSamplers(0, 1, &r->sampler); context->OMSetDepthStencilState(r->no_depth, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu); context->RSSetState(nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &r->field_target, nullptr);
        if (!uniforms(width, height, 0.0F)) return failure();
        context->Draw(6, 0);
        viewport.Width = static_cast<float>(frame->width_px); viewport.Height = static_cast<float>(frame->height_px);
        context->RSSetViewports(1, &viewport); context->OMSetRenderTargets(1, &output, nullptr);
        context->PSSetShaderResources(0, 1, &r->field_source);
        if (!uniforms(frame->width_px, frame->height_px, 1.0F)) {
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
