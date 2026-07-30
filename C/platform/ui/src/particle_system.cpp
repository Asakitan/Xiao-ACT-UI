#include "sao/ui/particle_system.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <windows.h>
#endif

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr uint32_t kDefaultSeed = 0x6d2b79f5u;
constexpr uint32_t kMaxParticleCapacity = 1000000u;

struct ParticleSlot {
    SaoUiParticle particle{};
    float initial_life{};
};

uint32_t next_random(uint32_t* state) {
    uint32_t value = *state == 0u ? kDefaultSeed : *state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value == 0u ? kDefaultSeed : value;
    return *state;
}

float random_unit(uint32_t* state) {
    return static_cast<float>(next_random(state) >> 8u) / static_cast<float>(0x01000000u);
}

float random_range(uint32_t* state, float minimum, float maximum) {
    return minimum + (maximum - minimum) * random_unit(state);
}

uint32_t blend_argb(uint32_t from, uint32_t to, float amount) {
    const float t = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [t](uint32_t left, uint32_t right) {
        return static_cast<uint32_t>(std::lround(
            static_cast<float>(left) + (static_cast<float>(right) - static_cast<float>(left)) * t));
    };
    return (channel((from >> 24u) & 0xffu, (to >> 24u) & 0xffu) << 24u) |
           (channel((from >> 16u) & 0xffu, (to >> 16u) & 0xffu) << 16u) |
           (channel((from >> 8u) & 0xffu, (to >> 8u) & 0xffu) << 8u) |
           channel(from & 0xffu, to & 0xffu);
}

uint32_t scale_alpha(uint32_t color, float amount) {
    const uint32_t alpha = static_cast<uint32_t>(std::clamp(
        std::lround(static_cast<float>((color >> 24u) & 0xffu) * std::clamp(amount, 0.0F, 1.0F)),
        0L, 255L));
    return (color & 0x00ffffffu) | (alpha << 24u);
}

bool validate_config(const SaoUiParticleEmitterConfig& config) {
    const uint32_t declared =
        config.struct_size == 0u ? SAO_UI_PARTICLE_EMITTER_CONFIG_V1_SIZE : config.struct_size;
    if (declared != SAO_UI_PARTICLE_EMITTER_CONFIG_V1_SIZE || config.max_particles == 0u ||
        config.max_particles > kMaxParticleCapacity || config.reserved[0] != 0u ||
        config.reserved[1] != 0u || config.reserved[2] != 0u) {
        return false;
    }
    const float values[] = {
        config.origin_x,           config.origin_y,          config.min_speed,
        config.max_speed,          config.direction_radians, config.spread_radians,
        config.min_life_seconds,   config.max_life_seconds,  config.min_size_px,
        config.max_size_px,        config.acceleration_x,    config.acceleration_y,
        config.damping_per_second,
    };
    for (float value : values) {
        if (!std::isfinite(value))
            return false;
    }
    return config.min_speed >= 0.0F && config.max_speed >= config.min_speed &&
           std::fabs(config.spread_radians) <= 8.0F * kPi && config.min_life_seconds > 0.0F &&
           config.max_life_seconds >= config.min_life_seconds && config.min_size_px > 0.0F &&
           config.max_size_px >= config.min_size_px && config.damping_per_second >= 0.0F &&
           config.damping_per_second <= 100.0F;
}

} // namespace

struct sao_ui_particle_emitter_s {
    SaoUiParticleEmitterConfig config{};
    uint32_t random_state{kDefaultSeed};
    std::vector<ParticleSlot> particles;
    std::mutex mutex;
#if defined(_WIN32)
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11Buffer* gpu_buffer{};
#endif
};

namespace {

#if defined(_WIN32)

void release_gpu_no_lock(sao_ui_particle_emitter_s* emitter) {
    if (emitter->gpu_buffer != nullptr) {
        emitter->gpu_buffer->Release();
        emitter->gpu_buffer = nullptr;
    }
    if (emitter->context != nullptr) {
        emitter->context->Release();
        emitter->context = nullptr;
    }
    if (emitter->device != nullptr) {
        emitter->device->Release();
        emitter->device = nullptr;
    }
}

void sync_gpu_no_lock(sao_ui_particle_emitter_s* emitter) {
    if (emitter->gpu_buffer == nullptr || emitter->context == nullptr)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(
            emitter->context->Map(emitter->gpu_buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped))) {
        release_gpu_no_lock(emitter);
        return;
    }
    auto* destination = static_cast<SaoUiParticle*>(mapped.pData);
    for (size_t index = 0; index < emitter->particles.size(); ++index)
        destination[index] = emitter->particles[index].particle;
    emitter->context->Unmap(emitter->gpu_buffer, 0u);
}

bool initialize_gpu(sao_ui_particle_emitter_s* emitter, void* device_pointer) {
    if (!emitter->config.prefer_gpu || device_pointer == nullptr)
        return false;
    auto* device = static_cast<ID3D11Device*>(device_pointer);
    const uint64_t byte_width =
        static_cast<uint64_t>(emitter->config.max_particles) * sizeof(SaoUiParticle);
    if (byte_width == 0u || byte_width > std::numeric_limits<UINT>::max())
        return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(byte_width);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer)) || buffer == nullptr)
        return false;
    device->AddRef();
    emitter->device = device;
    device->GetImmediateContext(&emitter->context);
    if (emitter->context == nullptr) {
        buffer->Release();
        emitter->device->Release();
        emitter->device = nullptr;
        return false;
    }
    emitter->gpu_buffer = buffer;
    return true;
}

#else

void sync_gpu_no_lock(sao_ui_particle_emitter_s*) {}

#endif

} // namespace

extern "C" sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_create(void* d3d11_device_ptr, const SaoUiParticleEmitterConfig* config,
                               sao_ui_particle_emitter_handle_t* out_handle) {
    if (out_handle == nullptr || config == nullptr || !validate_config(*config))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    auto* emitter = new (std::nothrow) sao_ui_particle_emitter_s();
    if (emitter == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    try {
        emitter->config = *config;
        emitter->config.struct_size = sizeof(SaoUiParticleEmitterConfig);
        emitter->random_state = config->seed == 0u ? kDefaultSeed : config->seed;
        emitter->particles.reserve(config->max_particles);
#if defined(_WIN32)
        (void)initialize_gpu(emitter, d3d11_device_ptr);
#else
        (void)d3d11_device_ptr;
#endif
        *out_handle = emitter;
        return SAO_STATUS_OK;
    } catch (...) {
#if defined(_WIN32)
        release_gpu_no_lock(emitter);
#endif
        delete emitter;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL
sao_ui_particle_emitter_destroy(sao_ui_particle_emitter_handle_t handle) {
    if (handle == nullptr)
        return;
#if defined(_WIN32)
    {
        std::lock_guard lock(handle->mutex);
        release_gpu_no_lock(handle);
    }
#endif
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_reset(sao_ui_particle_emitter_handle_t handle, uint32_t seed) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    try {
        std::lock_guard lock(handle->mutex);
        handle->particles.clear();
        handle->random_state = seed == 0u ? kDefaultSeed : seed;
        sync_gpu_no_lock(handle);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_set_origin(sao_ui_particle_emitter_handle_t handle, float x, float y) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(x) || !std::isfinite(y))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    handle->config.origin_x = x;
    handle->config.origin_y = y;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_spawn_burst(sao_ui_particle_emitter_handle_t handle,
                                    uint32_t requested_count, uint32_t* out_spawned_count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_spawned_count != nullptr)
        *out_spawned_count = 0u;
    try {
        std::lock_guard lock(handle->mutex);
        const size_t available =
            static_cast<size_t>(handle->config.max_particles) - handle->particles.size();
        const uint32_t spawn_count =
            static_cast<uint32_t>(std::min<size_t>(requested_count, available));
        for (uint32_t index = 0; index < spawn_count; ++index) {
            const float angle =
                handle->config.direction_radians +
                (random_unit(&handle->random_state) - 0.5F) * handle->config.spread_radians;
            const float speed = random_range(&handle->random_state, handle->config.min_speed,
                                             handle->config.max_speed);
            const float life = random_range(&handle->random_state, handle->config.min_life_seconds,
                                            handle->config.max_life_seconds);
            ParticleSlot slot{};
            slot.particle.pos_x = handle->config.origin_x;
            slot.particle.pos_y = handle->config.origin_y;
            slot.particle.vel_x = std::cos(angle) * speed;
            slot.particle.vel_y = std::sin(angle) * speed;
            slot.particle.life = life;
            slot.particle.color =
                blend_argb(handle->config.color_start_argb, handle->config.color_end_argb,
                           random_unit(&handle->random_state));
            slot.particle.size = random_range(&handle->random_state, handle->config.min_size_px,
                                              handle->config.max_size_px);
            slot.initial_life = life;
            handle->particles.push_back(slot);
        }
        sync_gpu_no_lock(handle);
        if (out_spawned_count != nullptr)
            *out_spawned_count = spawn_count;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_update(sao_ui_particle_emitter_handle_t handle, float delta_seconds) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0F)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(handle->mutex);
        if (delta_seconds == 0.0F)
            return SAO_STATUS_OK;
        const float damping = std::exp(-handle->config.damping_per_second * delta_seconds);
        for (ParticleSlot& slot : handle->particles) {
            slot.particle.vel_x += handle->config.acceleration_x * delta_seconds;
            slot.particle.vel_y += handle->config.acceleration_y * delta_seconds;
            slot.particle.vel_x *= damping;
            slot.particle.vel_y *= damping;
            slot.particle.pos_x += slot.particle.vel_x * delta_seconds;
            slot.particle.pos_y += slot.particle.vel_y * delta_seconds;
            slot.particle.life -= delta_seconds;
        }
        std::erase_if(handle->particles,
                      [](const ParticleSlot& slot) { return slot.particle.life <= 0.0F; });
        sync_gpu_no_lock(handle);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_particle_emitter_render(
    sao_ui_particle_emitter_handle_t handle, sao_ui_paint_ctx_handle_t paint_ctx) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (paint_ctx == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    struct RenderParticle {
        SaoUiParticle particle;
        float alpha_scale;
    };
    std::vector<RenderParticle> snapshot;
    try {
        {
            std::lock_guard lock(handle->mutex);
            snapshot.reserve(handle->particles.size());
            for (const ParticleSlot& slot : handle->particles) {
                snapshot.push_back(
                    {slot.particle,
                     slot.initial_life <= 0.0F
                         ? 0.0F
                         : std::clamp(slot.particle.life / slot.initial_life, 0.0F, 1.0F)});
            }
        }
        for (const RenderParticle& entry : snapshot) {
            const float size = std::max(0.25F, entry.particle.size);
            const sao_status_t status = sao_ui_paint_ctx_fill_ellipse(
                paint_ctx, entry.particle.pos_x - size * 0.5F, entry.particle.pos_y - size * 0.5F,
                size, size, scale_alpha(entry.particle.color, entry.alpha_scale));
            if (status != SAO_STATUS_OK)
                return status;
        }
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_snapshot(sao_ui_particle_emitter_handle_t handle,
                                 SaoUiParticle* out_particles, size_t capacity, size_t* out_count) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_count == nullptr || (out_particles == nullptr && capacity != 0u))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(handle->mutex);
        *out_count = handle->particles.size();
        if (out_particles == nullptr)
            return capacity == 0u ? SAO_STATUS_OK : SAO_STATUS_ERR_INVALID_ARGUMENT;
        const size_t copied = std::min(capacity, handle->particles.size());
        for (size_t index = 0; index < copied; ++index)
            out_particles[index] = handle->particles[index].particle;
        return copied == handle->particles.size() ? SAO_STATUS_OK : SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    } catch (...) {
        *out_count = 0u;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_particle_emitter_gpu_buffer(
    sao_ui_particle_emitter_handle_t handle, void** out_d3d11_buffer) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_d3d11_buffer == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_d3d11_buffer = nullptr;
#if defined(_WIN32)
    std::lock_guard lock(handle->mutex);
    *out_d3d11_buffer = handle->gpu_buffer;
    return handle->gpu_buffer == nullptr ? SAO_STATUS_ERR_CAPABILITY_MISSING : SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#endif
}
