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
constexpr uint32_t kColumnSides = 32u;
constexpr uint32_t kCapBands = 4u;
constexpr uint32_t kCapVertexCount = kColumnSides * kCapBands + 1u;
constexpr uint32_t kStreakVertexCount = kCapVertexCount * 2u;
constexpr uint32_t kStreakIndexCount = kColumnSides * kCapBands * 12u;
constexpr float kPi = 3.14159265359F;

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

float smoothstep(float t) noexcept {
    const float x = saturate(t);
    return x * x * x * (x * (x * 6.0F - 15.0F) + 10.0F);
}

float linear_channel(float value) noexcept {
    return value <= 0.04045F ? value / 12.92F
                             : std::pow((value + 0.055F) / 1.055F, 2.4F);
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
    float bloom_extract;
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
    float camera_z{};
    float camera_velocity{};
    float particle_alpha{};
    float energy{};
    float flash{};
    float startup_burst{};
    float startup_wave{1.0F};
    float motion_mix{0.60F};
    float cool_mix{};
    std::array<float, 3> background{0.96F, 0.97F, 0.98F};
    std::array<float, 3> tint{0.76F, 0.88F, 0.96F};
    bool particles{};
};

std::array<float, 3> mix_color(const std::array<float, 3>& a, const std::array<float, 3>& b,
                               float t) noexcept {
    const float x = saturate(t);
    return {lerp(a[0], b[0], x), lerp(a[1], b[1], x), lerp(a[2], b[2], x)};
}

float camera_position(float elapsed, float duration) noexcept {
    const float exit_duration = std::min(0.45F, duration * 0.18F);
    const float main_duration = std::max(0.001F, duration - exit_duration);
    const float x = saturate(elapsed / main_duration);
    float low = 0.0F, high = 1.0F;
    for (int iteration = 0; iteration < 18; ++iteration) {
        const float t = (low + high) * 0.5F;
        const float u = 1.0F - t;
        const float bx = 3.0F * u * u * t * 0.8F + 3.0F * u * t * t * 0.9F + t * t * t;
        if (bx < x) low = t; else high = t;
    }
    const float t = (low + high) * 0.5F;
    const float u = 1.0F - t;
    const float bezier = 3.0F * u * u * t * 0.1F + 3.0F * u * t * t * 0.8F + t * t * t;
    const float exit_t = saturate((elapsed - main_duration) / std::max(0.001F, exit_duration));
    const float speed = 5400.0F / main_duration;
    return -1200.0F + 2700.0F * bezier + speed * exit_t * exit_duration +
           std::max(0.0F, 900.0F - speed * exit_duration) * exit_t * exit_t * exit_t;
}

VisualState visual_state(const FrameState& frame) noexcept {
    VisualState state{};
    const float elapsed = std::max(0.0F, frame.elapsed_seconds);
    const float prelude = std::max(0.0F, frame.startup_prelude);
    state.scene_time = frame.scene_timeline ? std::max(0.0F, elapsed - prelude) : elapsed;
    state.startup_wave = prelude > 0.0F ? saturate(elapsed / prelude) : 1.0F;
    state.camera_z = -1200.0F;
    state.background = {0.98F, 0.985F, 0.995F};
    const bool blue = state.scene_time >= frame.p3_start;
    const float blue_transition = smoothstep((state.scene_time - (frame.p3_start - 0.30F)) / 0.80F);
    state.tint = mix_color({0.84F, 0.88F, 0.94F}, {0.56F, 0.80F, 1.0F}, blue_transition);
    state.cool_mix = blue ? 1.0F : 0.0F;
    if (state.scene_time < frame.p2_start) {
        const float enter = smoothstep(state.scene_time / 0.70F);
        state.background = mix_color({0.94F, 0.955F, 0.97F}, {0.995F, 0.995F, 1.0F}, enter);
        state.background = mix_color(state.background, {0.955F, 0.965F, 0.98F},
            smoothstep((state.scene_time - (frame.p2_start - 0.55F)) / 0.55F));
    }
    state.background = mix_color(state.background, {0.91F, 0.955F, 0.995F}, blue_transition);
    if (blue) {
        state.background = mix_color(state.background, {0.96F, 0.975F, 0.99F},
            smoothstep((state.scene_time - (frame.p3_end - 0.55F)) / 1.0F));
        const float handoff = state.scene_time - frame.p4_start;
        state.flash = 0.12F * smoothstep(handoff / 0.32F) *
                      (1.0F - smoothstep((handoff - 0.32F) / 0.60F));
    }
    if (frame.reduced_motion) {
        state.background = {0.96F, 0.975F, 0.99F};
        state.flash = 0.0F;
        return state;
    }
    if (elapsed < prelude) {
        state.startup_burst = 1.0F - smoothstep(state.startup_wave / 0.62F);
        state.energy = 0.10F + smoothstep(state.startup_wave) * 0.08F;
        state.motion_mix = 1.8F * state.startup_wave * (1.0F - state.startup_wave);
        return state;
    }
    const float start = blue ? frame.p3_start : 0.0F;
    const float end = blue ? frame.p3_end : frame.p1_end;
    if (end <= start || state.scene_time < start) {
        state.energy = 0.16F;
        state.motion_mix = 0.0F;
        return state;
    }
    const float progress = saturate((state.scene_time - start) / (end - start));
    const float local_time = state.scene_time - start;
    const float settle_start = blue ? end - 0.20F : end;
    state.energy = lerp(0.18F + 0.72F * smoothstep(progress), blue ? 0.14F : 0.16F,
                         smoothstep((state.scene_time - settle_start) / 0.65F));
    if (state.scene_time >= end + 0.20F) {
        state.motion_mix = 0.0F;
        return state;
    }
    state.camera_z = camera_position(local_time, end - start);
    state.camera_velocity = (state.camera_z - camera_position(std::max(0.0F, local_time - 0.005F),
                                                               end - start)) / 0.005F;
    state.particle_alpha = (blue ? smoothstep(local_time / 0.60F)
                          : lerp(0.12F, 1.0F, smoothstep(local_time / 0.65F))) *
                      (1.0F - smoothstep((state.scene_time - (end - 0.30F)) / 0.50F));
    state.particles = state.particle_alpha > 0.001F;
    state.motion_mix = saturate(state.camera_velocity / 1800.0F);
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
    ID3D11Buffer* column_buffer{};
    ID3D11Buffer* index_buffer{};
    ID3D11Buffer* instance_buffer{};
    ID3D11Buffer* constants{};
    ID3D11SamplerState* sampler{};
    ID3D11BlendState* premultiplied_blend{};
    ID3D11BlendState* additive_blend{};
    ID3D11DepthStencilState* depth_disabled{};
    ID3D11DepthStencilState* depth_enabled{};
    ID3D11Texture2D* depth{};
    ID3D11DepthStencilView* depth_view{};
    ID3D11RasterizerState* no_cull_rasterizer{};
    ID3D11Texture2D* scene{};
    ID3D11RenderTargetView* scene_rtv{};
    ID3D11ShaderResourceView* scene_srv{};
    ID3D11Texture2D* multisample_scene{};
    ID3D11RenderTargetView* multisample_rtv{};
    uint32_t sample_count{1u};
    ID3D11Texture2D* bloom_a{};
    ID3D11RenderTargetView* bloom_a_rtv{};
    ID3D11ShaderResourceView* bloom_a_srv{};
    ID3D11Texture2D* bloom_b{};
    ID3D11RenderTargetView* bloom_b_rtv{};
    ID3D11ShaderResourceView* bloom_b_srv{};
    ID3D11Texture2D* bloom_wide_a{};
    ID3D11RenderTargetView* bloom_wide_a_rtv{};
    ID3D11ShaderResourceView* bloom_wide_a_srv{};
    ID3D11Texture2D* bloom_wide_b{};
    ID3D11RenderTargetView* bloom_wide_b_rtv{};
    ID3D11ShaderResourceView* bloom_wide_b_srv{};
    std::array<ID3D11Texture2D*, 2> history{};
    std::array<ID3D11RenderTargetView*, 2> history_rtv{};
    std::array<ID3D11ShaderResourceView*, 2> history_srv{};
    uint32_t history_index{};
    uint32_t history_seed{};
    float history_seconds{};
    float history_cool{};
    bool history_reduced{};
    bool history_valid{};
    uint32_t width{};
    uint32_t height{};
    uint32_t uploaded_seed{};
    bool instances_ready{};
};

namespace {

void release_targets(Renderer& renderer) noexcept {
    for (size_t index = 0; index < renderer.history.size(); ++index) {
        release(renderer.history_srv[index]);
        release(renderer.history_rtv[index]);
        release(renderer.history[index]);
    }
    renderer.history_valid = false;
    release(renderer.multisample_rtv);
    release(renderer.multisample_scene);
    renderer.sample_count = 1u;
    release(renderer.depth_view);
    release(renderer.depth);
    release(renderer.bloom_wide_b_srv);
    release(renderer.bloom_wide_b_rtv);
    release(renderer.bloom_wide_b);
    release(renderer.bloom_wide_a_srv);
    release(renderer.bloom_wide_a_rtv);
    release(renderer.bloom_wide_a);
    release(renderer.bloom_b_srv);
    release(renderer.bloom_b_rtv);
    release(renderer.bloom_b);
    release(renderer.bloom_a_srv);
    release(renderer.bloom_a_rtv);
    release(renderer.bloom_a);
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
    release(renderer.index_buffer);
    release(renderer.column_buffer);
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
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
                       renderer.column_buffer != nullptr && renderer.index_buffer != nullptr &&
                       renderer.instance_buffer != nullptr &&
                       renderer.constants != nullptr && renderer.sampler != nullptr &&
                       renderer.premultiplied_blend != nullptr &&
                       renderer.additive_blend != nullptr &&
                       renderer.depth_disabled != nullptr && renderer.depth_enabled != nullptr &&
                       renderer.no_cull_rasterizer != nullptr;
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

    std::array<Vertex, kStreakVertexCount> vertices{};
    std::array<uint16_t, kStreakIndexCount> indices{};
    size_t index_count = 0u;
    const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        indices[index_count++] = static_cast<uint16_t>(a);
        indices[index_count++] = static_cast<uint16_t>(b);
        indices[index_count++] = static_cast<uint16_t>(c);
    };
    const auto cap_vertex = [](float angle, float latitude, float end, float direction) {
        const float radius = std::max(0.0F, std::cos(latitude));
        const float x = std::cos(angle) * radius;
        const float y = std::sin(angle) * radius;
        return Vertex{{x, y, end}, {x, y, direction * std::sin(latitude)}};
    };
    for (uint32_t end = 0u; end < 2u; ++end) {
        const uint32_t base = end * kCapVertexCount;
        const float direction = end == 0u ? -1.0F : 1.0F;
        for (uint32_t ring = 0u; ring < kCapBands; ++ring) {
            const float latitude = static_cast<float>(ring) * kPi / (2.0F * kCapBands);
            for (uint32_t side = 0u; side < kColumnSides; ++side) {
                const float angle = static_cast<float>(side) * 2.0F * kPi / kColumnSides;
                vertices[base + ring * kColumnSides + side] =
                    cap_vertex(angle, latitude, static_cast<float>(end), direction);
            }
        }
        const uint32_t pole = base + kCapVertexCount - 1u;
        vertices[pole] = {{0.0F, 0.0F, static_cast<float>(end)}, {0.0F, 0.0F, direction}};
        for (uint32_t side = 0u; side < kColumnSides; ++side) {
            const uint32_t next = (side + 1u) % kColumnSides;
            for (uint32_t ring = 0u; ring + 1u < kCapBands; ++ring) {
                const uint32_t row = base + ring * kColumnSides;
                triangle(row + side, row + next, row + side + kColumnSides);
                triangle(row + next, row + next + kColumnSides, row + side + kColumnSides);
            }
            const uint32_t last = base + (kCapBands - 1u) * kColumnSides;
            triangle(last + side, last + next, pole);
            if (end == 0u) {
                triangle(side, next, side + kCapVertexCount);
                triangle(next, next + kCapVertexCount, side + kCapVertexCount);
            }
        }
    }
    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = static_cast<UINT>(sizeof(vertices));
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{vertices.data(), 0u, 0u};
    if (FAILED(device->CreateBuffer(&buffer_desc, &init, &renderer.column_buffer))) {
        release_device(renderer);
        return false;
    }
    buffer_desc.ByteWidth = static_cast<UINT>(sizeof(indices));
    buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = indices.data();
    if (FAILED(device->CreateBuffer(&buffer_desc, &init, &renderer.index_buffer))) {
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
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &renderer.depth_disabled))) {
        release_device(renderer);
        return false;
    }
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &renderer.depth_enabled))) {
        release_device(renderer);
        return false;
    }
    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    rasterizer_desc.MultisampleEnable = TRUE;
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
    const uint32_t quarter_width = std::max(1u, width / 4u);
    const uint32_t quarter_height = std::max(1u, height / 4u);
    if (!create_color_target(renderer.device, width, height, &renderer.scene, &renderer.scene_rtv,
                             &renderer.scene_srv) ||
        !create_color_target(renderer.device, half_width, half_height, &renderer.bloom_a,
                             &renderer.bloom_a_rtv, &renderer.bloom_a_srv) ||
        !create_color_target(renderer.device, half_width, half_height, &renderer.bloom_b,
                             &renderer.bloom_b_rtv, &renderer.bloom_b_srv) ||
        !create_color_target(renderer.device, quarter_width, quarter_height,
                             &renderer.bloom_wide_a, &renderer.bloom_wide_a_rtv,
                             &renderer.bloom_wide_a_srv) ||
        !create_color_target(renderer.device, quarter_width, quarter_height,
                             &renderer.bloom_wide_b, &renderer.bloom_wide_b_rtv,
                             &renderer.bloom_wide_b_srv)) {
        release_targets(renderer);
        return false;
    }
    for (size_t index = 0; index < renderer.history.size(); ++index) {
        if (!create_color_target(renderer.device, width, height, &renderer.history[index],
                                 &renderer.history_rtv[index], &renderer.history_srv[index])) {
            release_targets(renderer);
            return false;
        }
    }
    UINT format_support = 0u;
    const bool can_resolve = SUCCEEDED(renderer.device->CheckFormatSupport(
        DXGI_FORMAT_R16G16B16A16_FLOAT, &format_support)) &&
        (format_support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) != 0u;
    const uint32_t preferred_samples = !can_resolve ? 1u :
        static_cast<uint64_t>(width) * height > 2560u * 1440u ? 2u : 4u;
    for (uint32_t samples = preferred_samples; samples > 1u; samples /= 2u) {
        UINT color_levels = 0u, depth_levels = 0u;
        if (SUCCEEDED(renderer.device->CheckMultisampleQualityLevels(
                DXGI_FORMAT_R16G16B16A16_FLOAT, samples, &color_levels)) && color_levels > 0u &&
            SUCCEEDED(renderer.device->CheckMultisampleQualityLevels(
                DXGI_FORMAT_D32_FLOAT, samples, &depth_levels)) && depth_levels > 0u) {
            renderer.sample_count = samples;
            break;
        }
    }
    if (renderer.sample_count > 1u) {
        D3D11_TEXTURE2D_DESC color_desc{};
        color_desc.Width = width;
        color_desc.Height = height;
        color_desc.MipLevels = color_desc.ArraySize = 1u;
        color_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        color_desc.SampleDesc.Count = renderer.sample_count;
        color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(renderer.device->CreateTexture2D(&color_desc, nullptr, &renderer.multisample_scene)) ||
            FAILED(renderer.device->CreateRenderTargetView(renderer.multisample_scene, nullptr,
                                                            &renderer.multisample_rtv))) {
            release_targets(renderer);
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1u;
    depth_desc.ArraySize = 1u;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = renderer.sample_count;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(renderer.device->CreateTexture2D(&depth_desc, nullptr, &renderer.depth)) ||
        FAILED(renderer.device->CreateDepthStencilView(renderer.depth, nullptr, &renderer.depth_view))) {
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
        {1.00F, 0.00F, 0.00F}, {1.00F, 1.00F, 0.00F},
        {0.133F, 0.545F, 0.133F}, {0.133F, 0.133F, 0.133F},
        {0.502F, 0.502F, 0.502F}, {0.00F, 0.749F, 1.00F},
        {0.576F, 0.439F, 0.859F}, {1.00F, 0.078F, 0.576F},
    }};
    constexpr std::array<std::array<float, 3>, 8> cool{{
        {0.00F, 0.267F, 0.80F},
        {0.00F, 0.533F, 1.00F},
        {0.00F, 0.80F, 1.00F},
        {0.00F, 0.133F, 0.533F},
        {0.533F, 0.933F, 1.00F},
        {0.00F, 0.40F, 0.867F},
        {0.667F, 0.933F, 1.00F},
        {1.00F, 1.00F, 1.00F},
    }};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(renderer.instance_buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped)))
        return false;
    auto* instances = static_cast<Instance*>(mapped.pData);
    uint32_t random = renderer.frame.seed == 0u ? 0x51a0c3d7u : renderer.frame.seed;
    for (uint32_t index = 0u; index < kParticleCount; ++index) {
        const float angle = static_cast<float>(index) * 2.39996323F + random_unit(random) * 0.35F;
        const bool foreground = index % 6u == 0u;
        const float radius = lerp(16.0F, foreground ? 44.0F : 82.0F, std::sqrt(random_unit(random)));
        const float length = lerp(260.0F, 540.0F, random_unit(random));
        const float width = foreground ? lerp(2.1F, 3.3F, random_unit(random))
                          : lerp(0.9F, 1.9F, random_unit(random));
        const float depth = -800.0F + (static_cast<float>(index) + random_unit(random)) *
                           (2200.0F / static_cast<float>(kParticleCount));
        const float brightness = lerp(0.90F, 1.04F, random_unit(random));
        const float flicker = random_unit(random);
        const auto& warm_color = warm[index % warm.size()];
        const auto& cool_color = cool[index % cool.size()];
        instances[index] = {{radius * std::cos(angle), radius * std::sin(angle), depth},
                            length,
                            width,
                            {linear_channel(warm_color[0]), linear_channel(warm_color[1]),
                             linear_channel(warm_color[2])},
                            {linear_channel(cool_color[0]), linear_channel(cool_color[1]),
                             linear_channel(cool_color[2])},
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
    {
        std::lock_guard lock(renderer->mutex);
        release_device(*renderer);
    }
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
        float history_weight = 0.0F;
        const auto upload_constants = [&](float width, float height, float blur_x, float blur_y,
                                          float camera, float alpha, float radius,
                                          float extract = 0.0F,
                                          const VisualState* sample = nullptr) noexcept {
            const VisualState& current = sample == nullptr ? visual : *sample;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(device_context->Map(renderer->constants, 0u, D3D11_MAP_WRITE_DISCARD, 0u,
                                           &mapped)))
                return false;
            auto* values = static_cast<Constants*>(mapped.pData);
            *values = {{width, height},
                       renderer->frame.elapsed_seconds,
                       current.scene_time,
                       renderer->frame.phase_progress,
                       renderer->frame.phase,
                       renderer->frame.connected_alpha,
                       renderer->frame.reduced_motion ? 1.0F : 0.0F,
                       camera,
                       alpha,
                       radius,
                       current.energy,
                       current.flash,
                       current.startup_burst,
                       current.startup_wave,
                       current.motion_mix,
                       visual.cool_mix,
                       {blur_x, blur_y},
                       extract,
                       {visual.background[0], visual.background[1], visual.background[2]},
                       history_weight,
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
        device_context->SetPredication(nullptr, FALSE);
        device_context->GSSetShader(nullptr, nullptr, 0u);
        device_context->HSSetShader(nullptr, nullptr, 0u);
        device_context->DSSetShader(nullptr, nullptr, 0u);
        ID3D11Buffer* null_vertex_buffers[2]{nullptr, nullptr};
        constexpr UINT null_strides[2]{0u, 0u};
        constexpr UINT null_offsets[2]{0u, 0u};
        device_context->IASetVertexBuffers(0u, 2u, null_vertex_buffers, null_strides, null_offsets);
        ID3D11ShaderResourceView* null_inputs[4]{nullptr, nullptr, nullptr, nullptr};
        device_context->PSSetShaderResources(0u, 4u, null_inputs);
        constexpr float transparent[4]{0.0F, 0.0F, 0.0F, 0.0F};
        constexpr float blend_factor[4]{0.0F, 0.0F, 0.0F, 0.0F};
        ID3D11RenderTargetView* scene_target = renderer->multisample_rtv != nullptr
            ? renderer->multisample_rtv : renderer->scene_rtv;
        device_context->ClearRenderTargetView(scene_target, transparent);
        device_context->ClearDepthStencilView(renderer->depth_view, D3D11_CLEAR_DEPTH, 1.0F, 0u);
        device_context->OMSetRenderTargets(1u, &scene_target, renderer->depth_view);
        device_context->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        device_context->OMSetDepthStencilState(renderer->depth_disabled, 0u);
        D3D11_VIEWPORT viewport{0.0F,
                                0.0F,
                                static_cast<float>(context->width_px),
                                static_cast<float>(context->height_px),
                                0.0F,
                                1.0F};
        device_context->RSSetViewports(1u, &viewport);
        device_context->RSSetState(renderer->no_cull_rasterizer);
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
            ID3D11Buffer* buffers[]{renderer->column_buffer, renderer->instance_buffer};
            device_context->IASetVertexBuffers(0u, 2u, buffers, strides, offsets);
            device_context->IASetIndexBuffer(renderer->index_buffer, DXGI_FORMAT_R16_UINT, 0u);
            device_context->IASetInputLayout(renderer->streak_layout);
            device_context->VSSetShader(renderer->streak_vs, nullptr, 0u);
            device_context->PSSetShader(renderer->streak_ps, nullptr, 0u);
            device_context->RSSetState(renderer->no_cull_rasterizer);

            device_context->OMSetBlendState(renderer->premultiplied_blend, blend_factor, 0xffffffffu);
            constexpr std::array<float, 2> shutter_offsets{0.5F / 60.0F, 0.0F};
            constexpr std::array<float, 2> shutter_weights{0.12F, 1.0F};
            constexpr std::array<float, 2> shutter_radius{1.025F, 1.0F};
            for (size_t index = 0u; index < shutter_offsets.size(); ++index) {
                const bool current_sample = index + 1u == shutter_offsets.size();
                if (!current_sample && visual.motion_mix < 0.02F)
                    continue;
                device_context->OMSetDepthStencilState(
                    current_sample ? renderer->depth_enabled : renderer->depth_disabled, 0u);
                FrameState sample_frame = renderer->frame;
                sample_frame.elapsed_seconds =
                    std::max(0.0F, sample_frame.elapsed_seconds - shutter_offsets[index]);
                const VisualState sample = visual_state(sample_frame);
                if (!upload_constants(static_cast<float>(context->width_px),
                                      static_cast<float>(context->height_px), 0.0F, 0.0F,
                                      sample.camera_z,
                                      sample.particle_alpha * shutter_weights[index] *
                                          (current_sample ? 1.0F : visual.motion_mix),
                                      shutter_radius[index], 0.0F, &sample))
                    return gpu_failure();
                device_context->DrawIndexedInstanced(kStreakIndexCount, kParticleCount, 0u, 0, 0u);
            }
        }

        if (renderer->multisample_scene != nullptr) {
            device_context->OMSetRenderTargets(0u, nullptr, nullptr);
            device_context->ResolveSubresource(renderer->scene, 0u, renderer->multisample_scene, 0u,
                                               DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
        const uint32_t half_width = std::max(1u, context->width_px / 2u);
        const uint32_t half_height = std::max(1u, context->height_px / 2u);
        const uint32_t quarter_width = std::max(1u, context->width_px / 4u);
        const uint32_t quarter_height = std::max(1u, context->height_px / 4u);
        device_context->RSSetState(nullptr);
        device_context->IASetVertexBuffers(0u, 2u, null_vertex_buffers, null_strides, null_offsets);
        device_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0u);
        device_context->IASetInputLayout(nullptr);
        device_context->VSSetShader(renderer->fullscreen_vs, nullptr, 0u);
        device_context->PSSetShader(renderer->post_ps, nullptr, 0u);
        device_context->OMSetDepthStencilState(renderer->depth_disabled, 0u);
        device_context->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        const auto blur = [&](ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
                              uint32_t width, uint32_t height, float x, float y,
                              float extract) noexcept {
            if (!upload_constants(static_cast<float>(width), static_cast<float>(height),
                                  x, y, visual.camera_z, visual.particle_alpha, 1.0F, extract))
                return false;
            const D3D11_VIEWPORT blur_viewport{0.0F, 0.0F, static_cast<float>(width),
                                               static_cast<float>(height), 0.0F, 1.0F};
            device_context->RSSetViewports(1u, &blur_viewport);
            device_context->OMSetRenderTargets(1u, &target, nullptr);
            device_context->PSSetShaderResources(0u, 1u, &source);
            device_context->Draw(6u, 0u);
            ID3D11ShaderResourceView* null_source = nullptr;
            device_context->PSSetShaderResources(0u, 1u, &null_source);
            return true;
        };
        if (!renderer->frame.reduced_motion &&
            (!blur(renderer->scene_srv, renderer->bloom_a_rtv,
                 half_width, half_height, 1.0F, 0.0F, 1.0F) ||
             !blur(renderer->bloom_a_srv, renderer->bloom_b_rtv,
                 half_width, half_height, 0.0F, 1.0F, 0.0F) ||
             !blur(renderer->bloom_b_srv, renderer->bloom_wide_a_rtv,
                 quarter_width, quarter_height, 2.4F, 0.0F, 0.0F) ||
             !blur(renderer->bloom_wide_a_srv, renderer->bloom_wide_b_rtv,
                 quarter_width, quarter_height, 0.0F, 2.4F, 0.0F)))
            return gpu_failure();

        device_context->RSSetViewports(1u, &viewport);
        const float delta = renderer->frame.elapsed_seconds - renderer->history_seconds;
        const bool same_frame = renderer->history_valid && delta == 0.0F &&
            renderer->history_seed == renderer->frame.seed &&
            renderer->history_reduced == renderer->frame.reduced_motion;
        if (!same_frame) {
            const bool continuous = renderer->history_valid && delta > 0.0F && delta < 0.15F &&
                renderer->history_seed == renderer->frame.seed &&
                renderer->history_cool == visual.cool_mix && !renderer->frame.reduced_motion &&
                !renderer->history_reduced;
            const float retention = lerp(0.06F, 0.22F, visual.motion_mix);
            history_weight = continuous ? std::pow(retention, delta * 60.0F) : 0.0F;
            const uint32_t next = renderer->history_valid ? 1u - renderer->history_index : 0u;
            if (!upload_constants(static_cast<float>(context->width_px),
                                  static_cast<float>(context->height_px), 0.0F, 0.0F, visual.camera_z,
                                  visual.particle_alpha, 1.0F, 2.0F))
                return gpu_failure();
            ID3D11ShaderResourceView* inputs[]{renderer->scene_srv,
                renderer->frame.reduced_motion ? nullptr : renderer->bloom_b_srv,
                renderer->frame.reduced_motion ? nullptr : renderer->bloom_wide_b_srv,
                renderer->history_valid ? renderer->history_srv[renderer->history_index] : nullptr};
            device_context->OMSetRenderTargets(1u, &renderer->history_rtv[next], nullptr);
            device_context->PSSetShaderResources(0u, 4u, inputs);
            device_context->Draw(6u, 0u);
            device_context->PSSetShaderResources(0u, 4u, null_inputs);
            renderer->history_index = next;
            renderer->history_valid = true;
            renderer->history_seconds = renderer->frame.elapsed_seconds;
            renderer->history_seed = renderer->frame.seed;
            renderer->history_cool = visual.cool_mix;
            renderer->history_reduced = renderer->frame.reduced_motion;
        }
        if (!upload_constants(static_cast<float>(context->width_px),
                              static_cast<float>(context->height_px), 0.0F, 0.0F, visual.camera_z,
                              visual.particle_alpha, 1.0F))
            return gpu_failure();
        device_context->OMSetRenderTargets(1u, &output, nullptr);
        device_context->PSSetShaderResources(0u, 1u, &renderer->history_srv[renderer->history_index]);
        device_context->OMSetBlendState(renderer->premultiplied_blend, blend_factor, 0xffffffffu);
        device_context->Draw(6u, 0u);
        device_context->PSSetShaderResources(0u, 4u, null_inputs);
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
