#include "linkstart_renderer_d3d11.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>

#include "sao_ui_linkstart_background_ps.h"
#include "sao_ui_linkstart_fullscreen_vs.h"
#include "sao_ui_linkstart_post_ps.h"
#include "sao_ui_linkstart_streak_ps.h"
#include "sao_ui_linkstart_streak_vs.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <new>

namespace sao::ui::linkstart_gpu {

#if defined(_WIN32)
namespace {

template <typename T> void release(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

struct Constants {
    float resolution[2];
    float time;
    float progress;
    float phase;
    float connected_alpha;
    float reduced_motion;
    float padding0;
    float bloom_direction[2];
    float padding1[2];
};
static_assert(sizeof(Constants) == 48u);

struct Instance {
    float angle;
    float phase;
    float width;
    float gold;
};

} // namespace

struct Renderer {
    std::mutex mutex;
    FrameState frame{};
    ID3D11Device* device{};
    ID3D11VertexShader* fullscreen_vs{};
    ID3D11PixelShader* background_ps{};
    ID3D11VertexShader* streak_vs{};
    ID3D11PixelShader* streak_ps{};
    ID3D11PixelShader* post_ps{};
    ID3D11InputLayout* streak_layout{};
    ID3D11Buffer* quad_buffer{};
    ID3D11Buffer* instance_buffer{};
    ID3D11Buffer* constants{};
    ID3D11SamplerState* sampler{};
    ID3D11BlendState* premultiplied_blend{};
    ID3D11BlendState* additive_blend{};
    ID3D11Texture2D* scene{};
    ID3D11RenderTargetView* scene_rtv{};
    ID3D11ShaderResourceView* scene_srv{};
    ID3D11Texture2D* bloom_a{};
    ID3D11RenderTargetView* bloom_a_rtv{};
    ID3D11ShaderResourceView* bloom_a_srv{};
    ID3D11Texture2D* bloom_b{};
    ID3D11RenderTargetView* bloom_b_rtv{};
    ID3D11ShaderResourceView* bloom_b_srv{};
    uint32_t width{};
    uint32_t height{};
    uint32_t uploaded_seed{};
    bool instances_ready{};
};

void release_targets(Renderer& r) {
    release(r.bloom_b_srv);
    release(r.bloom_b_rtv);
    release(r.bloom_b);
    release(r.bloom_a_srv);
    release(r.bloom_a_rtv);
    release(r.bloom_a);
    release(r.scene_srv);
    release(r.scene_rtv);
    release(r.scene);
    r.width = r.height = 0;
}

void release_device(Renderer& r) {
    release_targets(r);
    release(r.additive_blend);
    release(r.premultiplied_blend);
    release(r.sampler);
    release(r.constants);
    release(r.instance_buffer);
    release(r.quad_buffer);
    release(r.streak_layout);
    release(r.post_ps);
    release(r.streak_ps);
    release(r.streak_vs);
    release(r.background_ps);
    release(r.fullscreen_vs);
    r.device = nullptr;
    r.uploaded_seed = 0;
    r.instances_ready = false;
}

bool create_target(ID3D11Device* d, uint32_t w, uint32_t h, ID3D11Texture2D** tex,
                   ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(d->CreateTexture2D(&desc, nullptr, tex)) &&
           SUCCEEDED(d->CreateRenderTargetView(*tex, nullptr, rtv)) &&
           SUCCEEDED(d->CreateShaderResourceView(*tex, nullptr, srv));
}

bool ensure_device(Renderer& r, ID3D11Device* device) {
    const bool ready =
        r.fullscreen_vs != nullptr && r.background_ps != nullptr && r.streak_vs != nullptr &&
        r.streak_ps != nullptr && r.post_ps != nullptr && r.streak_layout != nullptr &&
        r.quad_buffer != nullptr && r.instance_buffer != nullptr && r.constants != nullptr &&
        r.sampler != nullptr && r.premultiplied_blend != nullptr && r.additive_blend != nullptr;
    if (r.device == device && ready)
        return true;
    release_device(r);
    r.device = device;
    bool ok =
        SUCCEEDED(device->CreateVertexShader(g_sao_ui_linkstart_fullscreen_vs,
                                             sizeof(g_sao_ui_linkstart_fullscreen_vs), nullptr,
                                             &r.fullscreen_vs)) &&
        SUCCEEDED(device->CreatePixelShader(g_sao_ui_linkstart_background_ps,
                                            sizeof(g_sao_ui_linkstart_background_ps), nullptr,
                                            &r.background_ps)) &&
        SUCCEEDED(device->CreateVertexShader(g_sao_ui_linkstart_streak_vs,
                                             sizeof(g_sao_ui_linkstart_streak_vs), nullptr,
                                             &r.streak_vs)) &&
        SUCCEEDED(device->CreatePixelShader(g_sao_ui_linkstart_streak_ps,
                                            sizeof(g_sao_ui_linkstart_streak_ps), nullptr,
                                            &r.streak_ps)) &&
        SUCCEEDED(device->CreatePixelShader(
            g_sao_ui_linkstart_post_ps, sizeof(g_sao_ui_linkstart_post_ps), nullptr, &r.post_ps));
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    ok = ok && SUCCEEDED(device->CreateInputLayout(layout, 2, g_sao_ui_linkstart_streak_vs,
                                                   sizeof(g_sao_ui_linkstart_streak_vs),
                                                   &r.streak_layout));
    if (!ok) {
        release_device(r);
        return false;
    }
    const float quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{quad, 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &init, &r.quad_buffer))) {
        release_device(r);
        return false;
    }
    bd = {};
    bd.ByteWidth = sizeof(Instance) * 320;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &r.instance_buffer))) {
        release_device(r);
        return false;
    }
    bd = {};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &r.constants))) {
        release_device(r);
        return false;
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &r.sampler))) {
        release_device(r);
        return false;
    }
    auto make_blend = [&](bool additive, ID3D11BlendState** out) {
        D3D11_BLEND_DESC b{};
        auto& x = b.RenderTarget[0];
        x.BlendEnable = TRUE;
        x.SrcBlend = D3D11_BLEND_ONE;
        x.DestBlend = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        x.BlendOp = D3D11_BLEND_OP_ADD;
        x.SrcBlendAlpha = D3D11_BLEND_ONE;
        x.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        x.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        x.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(device->CreateBlendState(&b, out));
    };
    if (!make_blend(false, &r.premultiplied_blend) || !make_blend(true, &r.additive_blend)) {
        release_device(r);
        return false;
    }
    return true;
}

bool ensure_targets(Renderer& r, uint32_t w, uint32_t h) {
    if (r.scene && r.width == w && r.height == h)
        return true;
    release_targets(r);
    const uint32_t hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);
    if (!create_target(r.device, w, h, &r.scene, &r.scene_rtv, &r.scene_srv) ||
        !create_target(r.device, hw, hh, &r.bloom_a, &r.bloom_a_rtv, &r.bloom_a_srv) ||
        !create_target(r.device, hw, hh, &r.bloom_b, &r.bloom_b_rtv, &r.bloom_b_srv)) {
        release_targets(r);
        return false;
    }
    r.width = w;
    r.height = h;
    return true;
}

sao_status_t create(Renderer** out_renderer) noexcept {
    if (!out_renderer)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_renderer = new (std::nothrow) Renderer;
    return *out_renderer ? SAO_STATUS_OK : SAO_STATUS_ERR_UNKNOWN;
}
void destroy(Renderer* renderer) noexcept {
    if (!renderer)
        return;
    release_device(*renderer);
    delete renderer;
}
void update(Renderer* renderer, const FrameState& frame) noexcept {
    if (!renderer)
        return;
    std::lock_guard lock(renderer->mutex);
    renderer->frame = frame;
}

sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* rc, void* user_data) noexcept {
    auto* r = static_cast<Renderer*>(user_data);
    if (r == nullptr || rc == nullptr ||
        rc->struct_size < SAO_UI_D3D11_LAYER_RENDER_CONTEXT_V1_SIZE)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(r->mutex);
        auto* d = static_cast<ID3D11Device*>(rc->d3d11_device);
        auto* c = static_cast<ID3D11DeviceContext*>(rc->d3d11_context);
        auto* out = static_cast<ID3D11RenderTargetView*>(rc->render_target_view);
        if (d == nullptr || c == nullptr || out == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto gpu_failure = [&]() {
            return FAILED(d->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST
                                                       : SAO_STATUS_ERR_OS_CALL_FAILED;
        };
        if (!ensure_device(*r, d) || !ensure_targets(*r, rc->width_px, rc->height_px))
            return gpu_failure();

        const auto upload_constants = [&](float width, float height, float dx, float dy) -> bool {
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(c->Map(r->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
                return false;
            auto* constants = static_cast<Constants*>(map.pData);
            *constants = {{width, height},
                          r->frame.elapsed_seconds,
                          r->frame.phase_progress,
                          r->frame.phase,
                          r->frame.connected_alpha,
                          r->frame.reduced_motion ? 1.0F : 0.0F,
                          0.0F,
                          {dx, dy},
                          {0.0F, 0.0F}};
            c->Unmap(r->constants, 0);
            return true;
        };
        if (!upload_constants(static_cast<float>(rc->width_px), static_cast<float>(rc->height_px),
                              0.0F, 0.0F))
            return gpu_failure();

        ID3D11Buffer* constant_buffer = r->constants;
        c->VSSetConstantBuffers(0, 1, &constant_buffer);
        c->PSSetConstantBuffers(0, 1, &constant_buffer);
        c->PSSetSamplers(0, 1, &r->sampler);
        constexpr float transparent[4]{0, 0, 0, 0};
        constexpr float blend_factor[4]{0, 0, 0, 0};
        c->ClearRenderTargetView(r->scene_rtv, transparent);
        ID3D11RenderTargetView* scene_target = r->scene_rtv;
        c->OMSetRenderTargets(1, &scene_target, nullptr);
        c->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        D3D11_VIEWPORT viewport{
            0, 0, static_cast<float>(rc->width_px), static_cast<float>(rc->height_px), 0, 1};
        c->RSSetViewports(1, &viewport);
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(r->fullscreen_vs, nullptr, 0);
        c->PSSetShader(r->background_ps, nullptr, 0);
        c->Draw(6, 0);

        const uint32_t instance_count = r->frame.reduced_motion ? 0u : 320u;
        if (instance_count != 0) {
            if (!r->instances_ready || r->uploaded_seed != r->frame.seed) {
                D3D11_MAPPED_SUBRESOURCE map{};
                if (FAILED(c->Map(r->instance_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
                    return gpu_failure();
                auto* instances = static_cast<Instance*>(map.pData);
                constexpr float pi = 3.14159265359F;
                for (uint32_t i = 0; i < instance_count; ++i) {
                    instances[i] = {
                        static_cast<float>(i) * 2.0F * pi / static_cast<float>(instance_count) +
                            static_cast<float>(i % 7u) * 0.013F,
                        std::fmod(static_cast<float>(i) * 0.6180339F +
                                      static_cast<float>(r->frame.seed & 255u) * 0.001F,
                                  1.0F),
                        0.55F + static_cast<float>(i % 5u) * 0.22F, (i % 11u) == 0u ? 1.0F : 0.0F};
                }
                c->Unmap(r->instance_buffer, 0);
                r->uploaded_seed = r->frame.seed;
                r->instances_ready = true;
            }
            UINT strides[]{sizeof(float) * 2u, sizeof(Instance)};
            UINT offsets[]{0u, 0u};
            ID3D11Buffer* buffers[]{r->quad_buffer, r->instance_buffer};
            c->IASetVertexBuffers(0, 2, buffers, strides, offsets);
            c->IASetInputLayout(r->streak_layout);
            c->VSSetShader(r->streak_vs, nullptr, 0);
            c->PSSetShader(r->streak_ps, nullptr, 0);
            c->OMSetBlendState(r->additive_blend, blend_factor, 0xffffffffu);
            c->DrawInstanced(6, instance_count, 0, 0);
        }

        const uint32_t half_width = std::max(1u, rc->width_px / 2u);
        const uint32_t half_height = std::max(1u, rc->height_px / 2u);
        D3D11_VIEWPORT half_viewport{
            0, 0, static_cast<float>(half_width), static_cast<float>(half_height), 0, 1};
        c->RSSetViewports(1, &half_viewport);
        c->IASetInputLayout(nullptr);
        c->VSSetShader(r->fullscreen_vs, nullptr, 0);
        c->PSSetShader(r->post_ps, nullptr, 0);
        c->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        const auto blur = [&](ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
                              float dx, float dy) -> bool {
            if (!upload_constants(static_cast<float>(half_width), static_cast<float>(half_height),
                                  dx, dy))
                return false;
            c->OMSetRenderTargets(1, &target, nullptr);
            c->PSSetShaderResources(0, 1, &source);
            c->Draw(6, 0);
            ID3D11ShaderResourceView* null_source = nullptr;
            c->PSSetShaderResources(0, 1, &null_source);
            return true;
        };
        if (!r->frame.reduced_motion && (!blur(r->scene_srv, r->bloom_a_rtv, 1.0F, 0.0F) ||
                                         !blur(r->bloom_a_srv, r->bloom_b_rtv, 0.0F, 1.0F)))
            return gpu_failure();

        if (!upload_constants(static_cast<float>(rc->width_px), static_cast<float>(rc->height_px),
                              0.0F, 0.0F))
            return gpu_failure();
        c->RSSetViewports(1, &viewport);
        c->OMSetRenderTargets(1, &out, nullptr);
        ID3D11ShaderResourceView* sources[]{r->scene_srv,
                                            r->frame.reduced_motion ? nullptr : r->bloom_b_srv};
        c->PSSetShaderResources(0, 2, sources);
        c->OMSetBlendState(r->premultiplied_blend, blend_factor, 0xffffffffu);
        c->Draw(6, 0);
        ID3D11ShaderResourceView* null_sources[]{nullptr, nullptr};
        c->PSSetShaderResources(0, 2, null_sources);
        return FAILED(d->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST : SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

#else
struct Renderer {};
sao_status_t create(Renderer**) noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}
void destroy(Renderer*) noexcept {}
void update(Renderer*, const FrameState&) noexcept {}
sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext*, void*) noexcept {
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}
#endif

} // namespace sao::ui::linkstart_gpu
