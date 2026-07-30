#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/d2d_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_particle_emitter_s* sao_ui_particle_emitter_handle_t;

struct SaoUiParticle {
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float life;
    uint32_t color;
    float size;
};

struct SaoUiParticleEmitterConfig {
    uint32_t struct_size;
    uint32_t max_particles;
    uint32_t seed;
    float origin_x;
    float origin_y;
    float min_speed;
    float max_speed;
    float direction_radians;
    float spread_radians;
    float min_life_seconds;
    float max_life_seconds;
    float min_size_px;
    float max_size_px;
    float acceleration_x;
    float acceleration_y;
    float damping_per_second;
    uint32_t color_start_argb;
    uint32_t color_end_argb;
    bool prefer_gpu;
    uint8_t reserved[3];
};

#define SAO_UI_PARTICLE_EMITTER_CONFIG_V1_SIZE 76u

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_create(void* d3d11_device_ptr, const SaoUiParticleEmitterConfig* config,
                               sao_ui_particle_emitter_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL
sao_ui_particle_emitter_destroy(sao_ui_particle_emitter_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_reset(sao_ui_particle_emitter_handle_t handle, uint32_t seed);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_set_origin(sao_ui_particle_emitter_handle_t handle, float x, float y);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_particle_emitter_spawn_burst(
    sao_ui_particle_emitter_handle_t handle, uint32_t requested_count, uint32_t* out_spawned_count);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_update(sao_ui_particle_emitter_handle_t handle, float delta_seconds);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_particle_emitter_render(
    sao_ui_particle_emitter_handle_t handle, sao_ui_paint_ctx_handle_t paint_ctx);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_particle_emitter_snapshot(sao_ui_particle_emitter_handle_t handle,
                                 SaoUiParticle* out_particles, size_t capacity, size_t* out_count);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_particle_emitter_gpu_buffer(
    sao_ui_particle_emitter_handle_t handle, void** out_d3d11_buffer);

#ifdef __cplusplus
}

static_assert(sizeof(SaoUiParticle) == 28u);
static_assert(sizeof(SaoUiParticleEmitterConfig) == SAO_UI_PARTICLE_EMITTER_CONFIG_V1_SIZE);
#endif
