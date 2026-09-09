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
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace sao::ui::linkstart_gpu {

#if defined(_WIN32)
namespace {

constexpr uint32_t kParticleCount = 300u;
constexpr uint32_t kCylinderSegments = 10u;
constexpr uint32_t kCylinderVertexCount = kCylinderSegments * 6u;
constexpr float kPi = 3.14159265359F;
constexpr float kCameraStart = -1200.0F;
constexpr float kCameraEnd = 1500.0F;
constexpr float kCameraExitExtra = 900.0F;
constexpr float kStreakLength = 420.0F;
constexpr float kTubeRadius = 1.8F;

template <typename T> void release(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * saturate(t);
}

float ease_in(float t) noexcept {
    const float x = saturate(t);
    return x * x * x;
}

float ease_out(float t) noexcept {
    const float x = 1.0F - saturate(t);
    return 1.0F - x * x * x;
}

float smoothstep(float t) noexcept {
    const float x = saturate(t);
    return x * x * (3.0F - 2.0F * x);
}

float cubic_bezier_y(float time_ratio) noexcept {
    float lo = 0.0F;
    float hi = 1.0F;
    for (int32_t iteration = 0; iteration < 25; ++iteration) {
        const float s = (lo + hi) * 0.5F;
        const float inv = 1.0F - s;
        const float x = 3.0F * inv * inv * s * 0.8F + 3.0F * inv * s * s * 0.9F + s * s * s;
        if (x < saturate(time_ratio))
            lo = s;
        else
            hi = s;
    }
    const float s = (lo + hi) * 0.5F;
    const float inv = 1.0F - s;
    return 3.0F * inv * inv * s * 0.1F + 3.0F * inv * s * s * 0.8F + s * s * s;
}

float camera_z(float elapsed, float duration) noexcept {
    return lerp(kCameraStart, kCameraEnd, cubic_bezier_y(elapsed / std::max(0.01F, duration)));
}

float camera_end_velocity(float duration) noexcept {
    return (kCameraEnd - kCameraStart) * 2.0F / std::max(0.01F, duration);
}

float camera_z_with_exit(float elapsed, float duration, float exit_start,
                         float exit_duration) noexcept {
    if (elapsed <= exit_start || exit_duration <= 0.0F)
        return camera_z(elapsed, duration);
    const float exit_elapsed = std::min(std::max(0.0F, elapsed - exit_start), exit_duration);
    const float velocity = camera_end_velocity(duration);
    const float topup = std::max(0.0F, kCameraExitExtra - velocity * exit_duration);
    return camera_z(exit_start, duration) + velocity * exit_elapsed +
           topup * ease_in(exit_elapsed / exit_duration);
}

struct Constants {
    float resolution[2];
    float time;
    float scene_time;
    float phase_progress;
    float phase;
    float connected_alpha;
    float reduced_motion;
    float camera_z;
    float alpha_mul;
    float radius_mul;
    float energy;
    float flash;
    float startup_burst;
    float startup_wave;
    float motion_mix;
    float cool_mix;
    float bloom_direction[2];
    float padding0;
    float background_color[3];
    float padding1;
    float effect_tint[3];
    float padding2;
};
static_assert(sizeof(Constants) == 112u);

struct Vertex {
    float position[3];
    float normal[3];
};

struct Instance {
    float center[3];
    float length;
    float radius;
    float warm_color[3];
    float cool_color[3];
    float brightness;
    float flicker;
};
static_assert(sizeof(Instance) == 52u);

struct VisualState {
    float scene_time{};
    float camera_z{kCameraStart};
    float camera_velocity{};
    float particle_alpha{};
    float energy{};
    float flash{};
    float startup_burst{};
    float startup_wave{1.0F};
    float motion_mix{0.60F};
    float cool_mix{};
    std::array<float, 3> background{0.008F, 0.016F, 0.039F};
    std::array<float, 3> tint{0.96F, 0.78F, 0.24F};
    bool particles{};
};

std::array<float, 3> mix_color(const std::array<float, 3>& a, const std::array<float, 3>& b,
                               float t) noexcept {
    const float x = saturate(t);
    return {lerp(a[0], b[0], x), lerp(a[1], b[1], x), lerp(a[2], b[2], x)};
}

std::array<float, 3> background_at(float t) noexcept {
    constexpr std::array<float, 3> start{2.0F / 255.0F, 4.0F / 255.0F, 10.0F / 255.0F};
    constexpr std::array<float, 3> tunnel{22.0F / 255.0F, 33.0F / 255.0F, 62.0F / 255.0F};
    constexpr std::array<float, 3> text{42.0F / 255.0F, 42.0F / 255.0F, 58.0F / 255.0F};
    constexpr std::array<float, 3> blue{10.0F / 255.0F, 22.0F / 255.0F, 40.0F / 255.0F};
    constexpr std::array<float, 3> blue_exit{26.0F / 255.0F, 42.0F / 255.0F, 74.0F / 255.0F};
    if (t < 0.12F)
        return start;
    if (t < 0.72F)
        return mix_color(start, tunnel, (t - 0.12F) / 0.60F);
    if (t < 3.0F)
        return tunnel;
    if (t < 3.5F)
        return mix_color(tunnel, text, (t - 3.0F) / 0.5F);
    if (t < 5.2F)
        return text;
    if (t < 5.7F)
        return mix_color(text, blue, (t - 5.2F) / 0.5F);
    if (t < 7.0F)
        return blue;
    if (t < 7.5F)
        return mix_color(blue, blue_exit, (t - 7.0F) / 0.5F);
    return blue_exit;
}

VisualState visual_state(const FrameState& frame) noexcept {
    VisualState state{};
    const float elapsed = std::max(0.0F, frame.elapsed_seconds);
    const float prelude = std::max(0.0F, frame.startup_prelude);
    state.scene_time = frame.scene_timeline ? std::max(0.0F, elapsed - prelude) : elapsed;
    state.background = background_at(state.scene_time);

    if (frame.reduced_motion && elapsed < prelude) {
        state.scene_time = frame.p2_start;
        state.background = background_at(frame.p2_start);
        state.energy = 0.12F;
        state.startup_burst = 0.0F;
        state.startup_wave = 1.0F;
        return state;
    }

    if (elapsed < prelude && prelude > 0.0F) {
        state.startup_burst = std::max(0.0F, 1.0F - elapsed / 0.52F);
        state.startup_wave = saturate(elapsed / prelude);
        state.energy = state.startup_wave * 0.18F;
        state.flash =
            std::max(0.0F, 1.0F - elapsed / prelude) * 0.42F + state.startup_burst * 0.30F;
        return state;
    }

    const float p1_duration = std::max(0.01F, frame.p1_end);
    const float p1_time = state.scene_time;
    if (state.scene_time < frame.p1_end + 0.5F) {
        float fade = 1.0F;
        if (p1_time < 0.28F)
            fade = lerp(0.22F, 0.62F, ease_out(p1_time / 0.28F));
        else if (p1_time < 1.0F)
            fade = lerp(0.62F, 1.0F, ease_out((p1_time - 0.28F) / 0.72F));
        const float exit_elapsed = std::max(0.0F, state.scene_time - frame.p1_end);
        fade *= 1.0F - saturate(exit_elapsed / 0.5F);
        state.particles = fade > 0.01F;
        state.particle_alpha = fade;
        state.camera_z = camera_z_with_exit(p1_time, p1_duration, p1_duration, 0.5F);
        state.camera_velocity = camera_end_velocity(p1_duration);
        const float bridge = std::min(prelude + 0.64F, elapsed);
        state.startup_burst = std::max(0.0F, 1.0F - bridge / 1.06F);
        state.startup_wave = saturate(bridge / std::max(0.01F, prelude + 0.64F));
        state.energy = std::min(
            1.0F, fade * (0.20F + 0.88F * saturate(p1_time / p1_duration) + exit_elapsed * 0.55F) +
                      state.startup_burst * 0.28F);
        state.flash = std::max(0.0F, 1.0F - bridge / 1.08F) * 0.30F + state.startup_burst * 0.28F +
                      std::min(0.22F, exit_elapsed * 0.38F);
        state.motion_mix = 0.70F;
    }

    if (state.scene_time >= frame.p3_start && state.scene_time < frame.p3_end + 0.55F) {
        const float p3_duration = std::max(0.01F, frame.p3_end - frame.p3_start);
        const float p3_time = state.scene_time - frame.p3_start;
        const float p3_fade = state.scene_time < frame.p3_start + 0.5F
                                  ? saturate((state.scene_time - frame.p3_start) / 0.5F)
                                  : 1.0F;
        const float exit_elapsed = std::max(0.0F, state.scene_time - frame.p3_end);
        const float exit_tail = saturate(exit_elapsed / 0.55F);
        const float smooth_tail = smoothstep(exit_tail);
        const float exit_keep = 1.0F - smooth_tail;
        const float phase = saturate(p3_time / p3_duration);
        const float run_energy = p3_fade * (0.20F + 0.80F * phase) *
                                 (state.scene_time >= frame.p3_end ? exit_keep : 1.0F);
        const float exit_energy = p3_fade * std::min(0.38F, exit_elapsed * 0.70F) * exit_keep;
        const float p4_floor = state.scene_time >= frame.p3_end ? 0.12F * smooth_tail : 0.0F;
        state.particles = p3_fade > 0.01F;
        state.particle_alpha = p3_fade * exit_keep;
        state.camera_z = camera_z_with_exit(p3_time, p3_duration, p3_duration, 0.55F);
        state.camera_velocity = camera_end_velocity(p3_duration);
        state.energy = state.scene_time >= frame.p3_end
                           ? std::max(0.12F, run_energy + exit_energy + p4_floor)
                           : run_energy;
        state.flash = std::max(0.0F, 1.0F - p3_time / 0.70F) * 0.18F +
                      std::min(0.24F, exit_elapsed * 0.52F) * exit_keep;
        state.startup_burst = 0.0F;
        state.startup_wave = 1.0F;
        state.motion_mix = exit_elapsed > 0.0F ? 0.60F + 0.32F * exit_tail : 0.60F;
        state.cool_mix = 1.0F;
        state.tint = {0.45F, 0.80F, 1.0F};
    } else if (!state.particles) {
        state.startup_burst = 0.0F;
        state.startup_wave = 1.0F;
        if (state.scene_time >= frame.p3_start) {
            const float exit_elapsed = std::max(0.0F, state.scene_time - frame.p3_end);
            const float tail = smoothstep(saturate(exit_elapsed / 0.55F));
            state.energy = std::max(0.12F, 0.12F * tail);
            state.flash = 0.0F;
            state.cool_mix = 1.0F;
            state.tint = {0.45F, 0.80F, 1.0F};
            state.motion_mix = 0.92F;
        } else {
            const float fade = saturate((state.scene_time - frame.p1_end) / 1.2F);
            state.energy = lerp(0.85F, 0.12F, ease_out(fade));
            state.flash = lerp(0.15F, 0.0F, fade);
        }
    }

    if (frame.reduced_motion) {
        state.particles = false;
        state.camera_velocity = 0.0F;
        state.energy = std::min(state.energy, 0.16F);
        state.flash = std::min(state.flash, 0.08F);
        state.startup_burst = 0.0F;
        state.startup_wave =
            elapsed < prelude ? saturate(elapsed / std::max(0.01F, prelude)) : 1.0F;
    }
    return state;
}

uint32_t xorshift(uint32_t& state) noexcept {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

float random_unit(uint32_t& state) noexcept {
    return static_cast<float>(xorshift(state) & 0x00ffffffu) / 16777215.0F;
}

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
    ID3D11Buffer* cylinder_buffer{};
    ID3D11Buffer* instance_buffer{};
    ID3D11Buffer* constants{};
    ID3D11SamplerState* sampler{};
    ID3D11BlendState* premultiplied_blend{};
    ID3D11BlendState* additive_blend{};
    ID3D11DepthStencilState* depth_enabled{};
    ID3D11DepthStencilState* depth_disabled{};
    ID3D11RasterizerState* no_cull_rasterizer{};
    ID3D11Texture2D* scene{};
    ID3D11RenderTargetView* scene_rtv{};
    ID3D11ShaderResourceView* scene_srv{};
    ID3D11Texture2D* scene_depth{};
    ID3D11DepthStencilView* scene_dsv{};
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

namespace {

void release_targets(Renderer& renderer) noexcept {
    release(renderer.bloom_b_srv);
    release(renderer.bloom_b_rtv);
    release(renderer.bloom_b);
    release(renderer.bloom_a_srv);
    release(renderer.bloom_a_rtv);
    release(renderer.bloom_a);
    release(renderer.scene_dsv);
    release(renderer.scene_depth);
    release(renderer.scene_srv);
    release(renderer.scene_rtv);
    release(renderer.scene);
    renderer.width = 0u;
    renderer.height = 0u;
}

void release_device(Renderer& renderer) noexcept {
    release_targets(renderer);
    release(renderer.no_cull_rasterizer);
    release(renderer.depth_disabled);
    release(renderer.depth_enabled);
    release(renderer.additive_blend);
    release(renderer.premultiplied_blend);
    release(renderer.sampler);
    release(renderer.constants);
    release(renderer.instance_buffer);
    release(renderer.cylinder_buffer);
    release(renderer.streak_layout);
    release(renderer.post_ps);
    release(renderer.streak_ps);
    release(renderer.streak_vs);
    release(renderer.background_ps);
    release(renderer.fullscreen_vs);
    renderer.device = nullptr;
    renderer.uploaded_seed = 0u;
    renderer.instances_ready = false;
}

bool create_color_target(ID3D11Device* device, uint32_t width, uint32_t height,
                         ID3D11Texture2D** texture, ID3D11RenderTargetView** rtv,
                         ID3D11ShaderResourceView** srv) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1u;
    desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1u;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture)) &&
           SUCCEEDED(device->CreateRenderTargetView(*texture, nullptr, rtv)) &&
           SUCCEEDED(device->CreateShaderResourceView(*texture, nullptr, srv));
}

bool ensure_device(Renderer& renderer, ID3D11Device* device) noexcept {
    const bool ready = renderer.fullscreen_vs != nullptr && renderer.background_ps != nullptr &&
                       renderer.streak_vs != nullptr && renderer.streak_ps != nullptr &&
                       renderer.post_ps != nullptr && renderer.streak_layout != nullptr &&
                       renderer.cylinder_buffer != nullptr && renderer.instance_buffer != nullptr &&
                       renderer.constants != nullptr && renderer.sampler != nullptr &&
                       renderer.premultiplied_blend != nullptr &&
                       renderer.additive_blend != nullptr && renderer.depth_enabled != nullptr &&
                       renderer.depth_disabled != nullptr && renderer.no_cull_rasterizer != nullptr;
    if (renderer.device == device && ready)
        return true;
    release_device(renderer);
    renderer.device = device;

    bool ok = SUCCEEDED(device->CreateVertexShader(g_sao_ui_linkstart_fullscreen_vs,
                                                   sizeof(g_sao_ui_linkstart_fullscreen_vs),
                                                   nullptr, &renderer.fullscreen_vs)) &&
              SUCCEEDED(device->CreatePixelShader(g_sao_ui_linkstart_background_ps,
                                                  sizeof(g_sao_ui_linkstart_background_ps), nullptr,
                                                  &renderer.background_ps)) &&
              SUCCEEDED(device->CreateVertexShader(g_sao_ui_linkstart_streak_vs,
                                                   sizeof(g_sao_ui_linkstart_streak_vs), nullptr,
                                                   &renderer.streak_vs)) &&
              SUCCEEDED(device->CreatePixelShader(g_sao_ui_linkstart_streak_ps,
                                                  sizeof(g_sao_ui_linkstart_streak_ps), nullptr,
                                                  &renderer.streak_ps)) &&
              SUCCEEDED(device->CreatePixelShader(g_sao_ui_linkstart_post_ps,
                                                  sizeof(g_sao_ui_linkstart_post_ps), nullptr,
                                                  &renderer.post_ps));
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTANCE_CENTER", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Instance, center),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE_LENGTH", 0, DXGI_FORMAT_R32_FLOAT, 1, offsetof(Instance, length),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE_RADIUS", 0, DXGI_FORMAT_R32_FLOAT, 1, offsetof(Instance, radius),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE_WARM", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Instance, warm_color),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE_COOL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, offsetof(Instance, cool_color),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE_EFFECT", 0, DXGI_FORMAT_R32_FLOAT, 1, offsetof(Instance, brightness),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTANCE_EFFECT", 1, DXGI_FORMAT_R32_FLOAT, 1, offsetof(Instance, flicker),
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    ok = ok && SUCCEEDED(device->CreateInputLayout(
                   layout, static_cast<UINT>(std::size(layout)), g_sao_ui_linkstart_streak_vs,
                   sizeof(g_sao_ui_linkstart_streak_vs), &renderer.streak_layout));
    if (!ok) {
        release_device(renderer);
        return false;
    }

    std::array<Vertex, kCylinderVertexCount> vertices{};
    uint32_t cursor = 0u;
    for (uint32_t segment = 0u; segment < kCylinderSegments; ++segment) {
        const float a0 =
            2.0F * kPi * static_cast<float>(segment) / static_cast<float>(kCylinderSegments);
        const float a1 =
            2.0F * kPi * static_cast<float>(segment + 1u) / static_cast<float>(kCylinderSegments);
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        vertices[cursor++] = {{c0, s0, 0.0F}, {c0, s0, 0.0F}};
        vertices[cursor++] = {{c1, s1, 0.0F}, {c1, s1, 0.0F}};
        vertices[cursor++] = {{c0, s0, 1.0F}, {c0, s0, 0.0F}};
        vertices[cursor++] = {{c1, s1, 0.0F}, {c1, s1, 0.0F}};
        vertices[cursor++] = {{c1, s1, 1.0F}, {c1, s1, 0.0F}};
        vertices[cursor++] = {{c0, s0, 1.0F}, {c0, s0, 0.0F}};
    }
    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = static_cast<UINT>(sizeof(vertices));
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{vertices.data(), 0u, 0u};
    if (FAILED(device->CreateBuffer(&buffer_desc, &init, &renderer.cylinder_buffer))) {
        release_device(renderer);
        return false;
    }
    buffer_desc = {};
    buffer_desc.ByteWidth = sizeof(Instance) * kParticleCount;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &renderer.instance_buffer))) {
        release_device(renderer);
        return false;
    }
    buffer_desc = {};
    buffer_desc.ByteWidth = sizeof(Constants);
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &renderer.constants))) {
        release_device(renderer);
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &renderer.sampler))) {
        release_device(renderer);
        return false;
    }
    const auto create_blend = [&](bool additive, ID3D11BlendState** output) noexcept {
        D3D11_BLEND_DESC desc{};
        auto& target = desc.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(device->CreateBlendState(&desc, output));
    };
    if (!create_blend(false, &renderer.premultiplied_blend) ||
        !create_blend(true, &renderer.additive_blend)) {
        release_device(renderer);
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &renderer.depth_enabled))) {
        release_device(renderer);
        return false;
    }
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &renderer.depth_disabled))) {
        release_device(renderer);
        return false;
    }
    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &renderer.no_cull_rasterizer))) {
        release_device(renderer);
        return false;
    }
    return true;
}

bool ensure_targets(Renderer& renderer, uint32_t width, uint32_t height) noexcept {
    if (renderer.scene != nullptr && renderer.width == width && renderer.height == height)
        return true;
    release_targets(renderer);
    const uint32_t half_width = std::max(1u, width / 2u);
    const uint32_t half_height = std::max(1u, height / 2u);
    if (!create_color_target(renderer.device, width, height, &renderer.scene, &renderer.scene_rtv,
                             &renderer.scene_srv) ||
        !create_color_target(renderer.device, half_width, half_height, &renderer.bloom_a,
                             &renderer.bloom_a_rtv, &renderer.bloom_a_srv) ||
        !create_color_target(renderer.device, half_width, half_height, &renderer.bloom_b,
                             &renderer.bloom_b_rtv, &renderer.bloom_b_srv)) {
        release_targets(renderer);
        return false;
    }
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1u;
    depth_desc.ArraySize = 1u;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1u;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(renderer.device->CreateTexture2D(&depth_desc, nullptr, &renderer.scene_depth)) ||
        FAILED(renderer.device->CreateDepthStencilView(renderer.scene_depth, nullptr,
                                                       &renderer.scene_dsv))) {
        release_targets(renderer);
        return false;
    }
    renderer.width = width;
    renderer.height = height;
    return true;
}

bool upload_instances(Renderer& renderer, ID3D11DeviceContext* context) noexcept {
    if (renderer.instances_ready && renderer.uploaded_seed == renderer.frame.seed)
        return true;
    constexpr std::array<std::array<float, 3>, 8> warm{{
        {1.0F, 0.0F, 0.0F},
        {1.0F, 1.0F, 0.0F},
        {0.133F, 0.545F, 0.133F},
        {0.133F, 0.133F, 0.133F},
        {0.502F, 0.502F, 0.502F},
        {0.0F, 0.749F, 1.0F},
        {0.576F, 0.439F, 0.859F},
        {1.0F, 0.078F, 0.576F},
    }};
    constexpr std::array<std::array<float, 3>, 8> cool{{
        {0.0F, 0.267F, 0.8F},
        {0.0F, 0.533F, 1.0F},
        {0.0F, 0.8F, 1.0F},
        {0.0F, 0.133F, 0.533F},
        {0.533F, 0.933F, 1.0F},
        {0.0F, 0.4F, 0.867F},
        {0.667F, 0.933F, 1.0F},
        {1.0F, 1.0F, 1.0F},
    }};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(renderer.instance_buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped)))
        return false;
    auto* instances = static_cast<Instance*>(mapped.pData);
    uint32_t random = renderer.frame.seed == 0u ? 0x51a0c3d7u : renderer.frame.seed;
    for (uint32_t index = 0u; index < kParticleCount; ++index) {
        const float angle = random_unit(random) * 2.0F * kPi;
        const float radius = lerp(10.0F, 38.0F, random_unit(random));
        const float depth = lerp(-800.0F, 1400.0F, random_unit(random));
        const float brightness = lerp(0.7F, 1.0F, random_unit(random));
        const float flicker = lerp(3.0F, 8.0F, random_unit(random));
        const float width = lerp(0.8F, 1.4F, random_unit(random));
        const auto& warm_color = warm[index % warm.size()];
        const auto& cool_color = cool[index % cool.size()];
        instances[index] = {{radius * std::cos(angle), radius * std::sin(angle), depth},
                            kStreakLength,
                            kTubeRadius * width,
                            {warm_color[0], warm_color[1], warm_color[2]},
                            {cool_color[0], cool_color[1], cool_color[2]},
                            brightness,
                            flicker};
    }
    context->Unmap(renderer.instance_buffer, 0u);
    renderer.uploaded_seed = renderer.frame.seed;
    renderer.instances_ready = true;
    return true;
}

} // namespace

sao_status_t create(Renderer** out_renderer) noexcept {
    if (out_renderer == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_renderer = new (std::nothrow) Renderer;
    return *out_renderer != nullptr ? SAO_STATUS_OK : SAO_STATUS_ERR_UNKNOWN;
}

void destroy(Renderer* renderer) noexcept {
    if (renderer == nullptr)
        return;
    release_device(*renderer);
    delete renderer;
}

void update(Renderer* renderer, const FrameState& frame) noexcept {
    if (renderer == nullptr)
        return;
    std::lock_guard lock(renderer->mutex);
    renderer->frame = frame;
}

sao_status_t SAO_UI_CALL render(const SaoUiD3d11LayerRenderContext* context,
                                void* user_data) noexcept {
    auto* renderer = static_cast<Renderer*>(user_data);
    if (renderer == nullptr || context == nullptr ||
        context->struct_size < SAO_UI_D3D11_LAYER_RENDER_CONTEXT_V1_SIZE)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(renderer->mutex);
        auto* device = static_cast<ID3D11Device*>(context->d3d11_device);
        auto* device_context = static_cast<ID3D11DeviceContext*>(context->d3d11_context);
        auto* output = static_cast<ID3D11RenderTargetView*>(context->render_target_view);
        if (device == nullptr || device_context == nullptr || output == nullptr ||
            context->width_px == 0u || context->height_px == 0u)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        const auto gpu_failure = [&]() noexcept {
            return FAILED(device->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST
                                                            : SAO_STATUS_ERR_OS_CALL_FAILED;
        };
        if (!ensure_device(*renderer, device) ||
            !ensure_targets(*renderer, context->width_px, context->height_px))
            return gpu_failure();

        const VisualState visual = visual_state(renderer->frame);
        const auto upload_constants = [&](float width, float height, float blur_x, float blur_y,
                                          float camera, float alpha, float radius) noexcept {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(device_context->Map(renderer->constants, 0u, D3D11_MAP_WRITE_DISCARD, 0u,
                                           &mapped)))
                return false;
            auto* values = static_cast<Constants*>(mapped.pData);
            *values = {{width, height},
                       renderer->frame.elapsed_seconds,
                       visual.scene_time,
                       renderer->frame.phase_progress,
                       renderer->frame.phase,
                       renderer->frame.connected_alpha,
                       renderer->frame.reduced_motion ? 1.0F : 0.0F,
                       camera,
                       alpha,
                       radius,
                       visual.energy,
                       visual.flash,
                       visual.startup_burst,
                       visual.startup_wave,
                       visual.motion_mix,
                       visual.cool_mix,
                       {blur_x, blur_y},
                       0.0F,
                       {visual.background[0], visual.background[1], visual.background[2]},
                       0.0F,
                       {visual.tint[0], visual.tint[1], visual.tint[2]},
                       0.0F};
            device_context->Unmap(renderer->constants, 0u);
            return true;
        };
        if (!upload_constants(static_cast<float>(context->width_px),
                              static_cast<float>(context->height_px), 0.0F, 0.0F, visual.camera_z,
                              visual.particle_alpha, 1.0F))
            return gpu_failure();

        ID3D11Buffer* constant_buffer = renderer->constants;
        device_context->VSSetConstantBuffers(0u, 1u, &constant_buffer);
        device_context->PSSetConstantBuffers(0u, 1u, &constant_buffer);
        device_context->PSSetSamplers(0u, 1u, &renderer->sampler);
        constexpr float transparent[4]{0.0F, 0.0F, 0.0F, 0.0F};
        constexpr float blend_factor[4]{0.0F, 0.0F, 0.0F, 0.0F};
        device_context->ClearRenderTargetView(renderer->scene_rtv, transparent);
        device_context->ClearDepthStencilView(renderer->scene_dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0F, 0u);
        ID3D11RenderTargetView* scene_target = renderer->scene_rtv;
        device_context->OMSetRenderTargets(1u, &scene_target, renderer->scene_dsv);
        device_context->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        device_context->OMSetDepthStencilState(renderer->depth_disabled, 0u);
        D3D11_VIEWPORT viewport{0.0F,
                                0.0F,
                                static_cast<float>(context->width_px),
                                static_cast<float>(context->height_px),
                                0.0F,
                                1.0F};
        device_context->RSSetViewports(1u, &viewport);
        device_context->IASetInputLayout(nullptr);
        device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device_context->VSSetShader(renderer->fullscreen_vs, nullptr, 0u);
        device_context->PSSetShader(renderer->background_ps, nullptr, 0u);
        device_context->Draw(6u, 0u);

        if (visual.particles && !renderer->frame.reduced_motion) {
            if (!upload_instances(*renderer, device_context))
                return gpu_failure();
            UINT strides[]{sizeof(Vertex), sizeof(Instance)};
            UINT offsets[]{0u, 0u};
            ID3D11Buffer* buffers[]{renderer->cylinder_buffer, renderer->instance_buffer};
            device_context->IASetVertexBuffers(0u, 2u, buffers, strides, offsets);
            device_context->IASetInputLayout(renderer->streak_layout);
            device_context->VSSetShader(renderer->streak_vs, nullptr, 0u);
            device_context->PSSetShader(renderer->streak_ps, nullptr, 0u);
            device_context->RSSetState(renderer->no_cull_rasterizer);

            const float blur_strength = visual.cool_mix > 0.5F ? 0.78F : 0.70F;
            device_context->OMSetDepthStencilState(renderer->depth_disabled, 0u);
            device_context->OMSetBlendState(renderer->additive_blend, blend_factor, 0xffffffffu);
            constexpr std::array<float, 2> ghost_steps{1.05F, 0.55F};
            constexpr std::array<float, 2> ghost_alpha{0.16F, 0.30F};
            constexpr std::array<float, 2> ghost_radius{1.095F, 1.055F};
            for (size_t index = 0u; index < ghost_steps.size(); ++index) {
                const float ghost_camera =
                    visual.camera_z - visual.camera_velocity * (ghost_steps[index] / 60.0F);
                if (!upload_constants(static_cast<float>(context->width_px),
                                      static_cast<float>(context->height_px), 0.0F, 0.0F,
                                      ghost_camera,
                                      visual.particle_alpha * ghost_alpha[index] * blur_strength,
                                      ghost_radius[index]))
                    return gpu_failure();
                device_context->DrawInstanced(kCylinderVertexCount, kParticleCount, 0u, 0u);
            }
            if (!upload_constants(static_cast<float>(context->width_px),
                                  static_cast<float>(context->height_px), 0.0F, 0.0F,
                                  visual.camera_z, visual.particle_alpha, 1.0F))
                return gpu_failure();
            device_context->OMSetDepthStencilState(renderer->depth_enabled, 0u);
            device_context->OMSetBlendState(renderer->premultiplied_blend, blend_factor,
                                            0xffffffffu);
            device_context->DrawInstanced(kCylinderVertexCount, kParticleCount, 0u, 0u);
        }

        const uint32_t half_width = std::max(1u, context->width_px / 2u);
        const uint32_t half_height = std::max(1u, context->height_px / 2u);
        D3D11_VIEWPORT half_viewport{
            0.0F, 0.0F, static_cast<float>(half_width), static_cast<float>(half_height),
            0.0F, 1.0F};
        device_context->RSSetViewports(1u, &half_viewport);
        device_context->RSSetState(nullptr);
        device_context->IASetInputLayout(nullptr);
        device_context->VSSetShader(renderer->fullscreen_vs, nullptr, 0u);
        device_context->PSSetShader(renderer->post_ps, nullptr, 0u);
        device_context->OMSetDepthStencilState(renderer->depth_disabled, 0u);
        device_context->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        const auto blur = [&](ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
                              float x, float y) noexcept {
            if (!upload_constants(static_cast<float>(half_width), static_cast<float>(half_height),
                                  x, y, visual.camera_z, visual.particle_alpha, 1.0F))
                return false;
            device_context->OMSetRenderTargets(1u, &target, nullptr);
            device_context->PSSetShaderResources(0u, 1u, &source);
            device_context->Draw(6u, 0u);
            ID3D11ShaderResourceView* null_source = nullptr;
            device_context->PSSetShaderResources(0u, 1u, &null_source);
            return true;
        };
        if (!renderer->frame.reduced_motion &&
            (!blur(renderer->scene_srv, renderer->bloom_a_rtv, 1.0F, 0.0F) ||
             !blur(renderer->bloom_a_srv, renderer->bloom_b_rtv, 0.0F, 1.0F)))
            return gpu_failure();

        if (!upload_constants(static_cast<float>(context->width_px),
                              static_cast<float>(context->height_px), 0.0F, 0.0F, visual.camera_z,
                              visual.particle_alpha, 1.0F))
            return gpu_failure();
        device_context->RSSetViewports(1u, &viewport);
        device_context->OMSetRenderTargets(1u, &output, nullptr);
        ID3D11ShaderResourceView* sources[]{
            renderer->scene_srv, renderer->frame.reduced_motion ? nullptr : renderer->bloom_b_srv};
        device_context->PSSetShaderResources(0u, 2u, sources);
        device_context->OMSetBlendState(renderer->premultiplied_blend, blend_factor, 0xffffffffu);
        device_context->Draw(6u, 0u);
        ID3D11ShaderResourceView* null_sources[]{nullptr, nullptr};
        device_context->PSSetShaderResources(0u, 2u, null_sources);
        return FAILED(device->GetDeviceRemovedReason()) ? SAO_STATUS_ERR_DEVICE_LOST
                                                        : SAO_STATUS_OK;
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
