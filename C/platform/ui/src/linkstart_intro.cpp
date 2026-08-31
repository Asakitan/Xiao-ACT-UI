#include "sao/ui/linkstart_intro.h"

#include "sao/ui/d2d_widgets.h"
#include "sao/ui/particle_system.h"
#include "sao/ui/sound.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_tick(sao_ui_nervegear_handle_t handle, int32_t dt_ms);

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr uint32_t kDefaultParticleSeed = 0x51a0c3d7u;
constexpr int32_t kLinkStartZOrder = 5000;

std::atomic_uint64_t g_linkstart_sequence{};

bool finite_nonnegative(float value) {
    return std::isfinite(value) && value >= 0.0F;
}

bool validate_timeline(const SaoUiLinkStartTimeline& timeline) {
    const float values[] = {
        timeline.startup_prelude, timeline.p1_end,         timeline.p2_start, timeline.p2_end,
        timeline.p3_start,        timeline.p3_end,         timeline.p4_start, timeline.p4_hold_end,
        timeline.p4_fade_end,     timeline.total_duration,
    };
    for (float value : values) {
        if (!finite_nonnegative(value))
            return false;
    }
    return timeline.startup_prelude <= timeline.p1_end && timeline.p2_start <= timeline.p2_end &&
           timeline.p3_start <= timeline.p3_end && timeline.p4_start <= timeline.p4_hold_end &&
           timeline.p4_hold_end <= timeline.p4_fade_end &&
           timeline.p1_end <= timeline.total_duration &&
           timeline.p2_end <= timeline.total_duration &&
           timeline.p3_end <= timeline.total_duration &&
           timeline.p4_fade_end <= timeline.total_duration && timeline.total_duration > 0.0F;
}

float interval_progress(float value, float start, float end) {
    if (end <= start)
        return value >= end ? 1.0F : 0.0F;
    return std::clamp((value - start) / (end - start), 0.0F, 1.0F);
}

uint32_t with_alpha(uint32_t color, float alpha) {
    const uint32_t source_alpha = (color >> 24u) & 0xffu;
    const uint32_t scaled = static_cast<uint32_t>(std::clamp(
        std::lround(static_cast<float>(source_alpha) * std::clamp(alpha, 0.0F, 1.0F)), 0L, 255L));
    return (color & 0x00ffffffu) | (scaled << 24u);
}

std::string reveal_text(const char* text, float progress) {
    const size_t length = std::strlen(text);
    const size_t visible = static_cast<size_t>(
        std::clamp(std::floor(static_cast<float>(length) * std::clamp(progress, 0.0F, 1.0F)), 0.0F,
                   static_cast<float>(length)));
    return std::string(text, visible);
}

} // namespace

struct sao_ui_linkstart_s {
    sao_ui_compositor_handle_t compositor{};
    sao_ui_nervegear_handle_t nervegear{};
    sao_ui_layer_handle_t layer{};
    sao_ui_offscreen_raster_handle_t raster{};
    sao_ui_paint_ctx_handle_t paint_ctx{};
    sao_ui_particle_emitter_handle_t tunnel{};
    sao_ui_particle_emitter_handle_t burst{};
    SaoUiLinkStartTimeline timeline{};
    uint32_t width{};
    uint32_t height{};
    uint32_t seed{kDefaultParticleSeed};
    int32_t elapsed_ms{};
    bool active{};
    bool burst_spawned{};
    bool nervegear_sound_played{};
    bool welcome_sound_played{};
    std::vector<uint8_t> pixels;
    std::mutex mutex;
};

namespace {

void destroy_resources(sao_ui_linkstart_s* handle) {
    if (handle == nullptr)
        return;
    sao_ui_particle_emitter_destroy(handle->burst);
    handle->burst = nullptr;
    sao_ui_particle_emitter_destroy(handle->tunnel);
    handle->tunnel = nullptr;
    sao_ui_paint_ctx_destroy(handle->paint_ctx);
    handle->paint_ctx = nullptr;
    sao_ui_offscreen_raster_destroy(handle->raster);
    handle->raster = nullptr;
    sao_ui_layer_destroy(handle->layer);
    handle->layer = nullptr;
}

SaoUiLinkStartPhase phase_at(const sao_ui_linkstart_s& handle, float seconds, float* out_progress) {
    SaoUiLinkStartPhase phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
    float progress = 0.0F;
    if (!handle.active && seconds <= 0.0F) {
        phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
    } else if (seconds < handle.timeline.p2_start) {
        phase = SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL;
        progress = interval_progress(seconds, 0.0F, handle.timeline.p1_end);
    } else if (seconds < handle.timeline.p3_start) {
        phase = SAO_UI_LINKSTART_PHASE_TEXT_REVEAL;
        progress = interval_progress(seconds, handle.timeline.p2_start, handle.timeline.p2_end);
    } else if (seconds < handle.timeline.p4_start) {
        phase = SAO_UI_LINKSTART_PHASE_RADIAL_BURST;
        progress = interval_progress(seconds, handle.timeline.p3_start, handle.timeline.p3_end);
    } else if (seconds < handle.timeline.total_duration) {
        phase = SAO_UI_LINKSTART_PHASE_CONNECTED;
        progress =
            interval_progress(seconds, handle.timeline.p4_start, handle.timeline.p4_fade_end);
    } else {
        phase = SAO_UI_LINKSTART_PHASE_COMPLETE;
        progress = 1.0F;
    }
    if (out_progress != nullptr)
        *out_progress = progress;
    return phase;
}

sao_status_t create_emitter(const SaoUiParticleEmitterConfig& config,
                            sao_ui_particle_emitter_handle_t* out_handle) {
    return sao_ui_particle_emitter_create(nullptr, &config, out_handle);
}

sao_status_t render_frame_locked(sao_ui_linkstart_s* handle) {
    const float seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
    sao_status_t status = sao_ui_paint_ctx_begin_frame(handle->paint_ctx);
    if (status != SAO_STATUS_OK)
        return status;
    status =
        sao_ui_paint_ctx_fill_rect(handle->paint_ctx, 0.0F, 0.0F, static_cast<float>(handle->width),
                                   static_cast<float>(handle->height), 0xff020611u);
    if (status == SAO_STATUS_OK) {
        status = sao_ui_paint_ctx_draw_scanlines(
            handle->paint_ctx, 0.0F, 0.0F, static_cast<float>(handle->width),
            static_cast<float>(handle->height), 4.0F, 1.0F, 0x1600c8ffu);
    }
    if (status == SAO_STATUS_OK)
        status = sao_ui_particle_emitter_render(handle->tunnel, handle->paint_ctx);
    if (status == SAO_STATUS_OK && seconds >= handle->timeline.p3_start)
        status = sao_ui_particle_emitter_render(handle->burst, handle->paint_ctx);

    const float center_x = static_cast<float>(handle->width) * 0.5F;
    const float center_y = static_cast<float>(handle->height) * 0.5F;
    if (status == SAO_STATUS_OK && seconds < handle->timeline.p1_end) {
        const float pulse = std::fmod(std::max(0.0F, seconds) * 0.75F, 1.0F);
        status = sao_ui_paint_ctx_draw_clock_pulse(
            handle->paint_ctx, center_x, center_y,
            std::max(12.0F, std::min(center_x, center_y) * 0.72F), pulse, 1.0F, 0xa000d8ffu);
    }

    if (status == SAO_STATUS_OK && seconds >= handle->timeline.p2_start &&
        seconds <= handle->timeline.p2_end) {
        const float reveal =
            interval_progress(seconds, handle->timeline.p2_start, handle->timeline.p2_end);
        const std::string title = reveal_text("LINK START", reveal);
        const float title_size = std::max(18.0F, static_cast<float>(handle->height) * 0.10F);
        status = sao_ui_paint_ctx_draw_utf8(
            handle->paint_ctx, center_x - static_cast<float>(title.size()) * title_size * 0.28F,
            center_y - title_size, title.c_str(), title_size, 0xffeafcffu);
        if (status == SAO_STATUS_OK && reveal > 0.45F) {
            status = sao_ui_paint_ctx_draw_utf8(
                handle->paint_ctx, center_x - 86.0F, center_y + 16.0F, "NEURAL LINK ESTABLISHED",
                std::max(10.0F, title_size * 0.42F),
                with_alpha(0xff68e4ffu, interval_progress(reveal, 0.45F, 1.0F)));
        }
    }

    if (status == SAO_STATUS_OK && seconds >= handle->timeline.p3_start &&
        seconds <= handle->timeline.p3_end) {
        const float burst_progress =
            interval_progress(seconds, handle->timeline.p3_start, handle->timeline.p3_end);
        status = sao_ui_paint_ctx_draw_corner_brackets(
            handle->paint_ctx, 12.0F, 12.0F, static_cast<float>(handle->width) - 24.0F,
            static_cast<float>(handle->height) - 24.0F,
            std::max(10.0F, std::min(handle->width, handle->height) * 0.08F), 1.5F,
            with_alpha(0xffd49c17u, 1.0F - burst_progress * 0.5F));
    }

    if (status == SAO_STATUS_OK && seconds >= handle->timeline.p4_start) {
        const float flash_in_end =
            std::min(handle->timeline.p4_hold_end, handle->timeline.p4_start + 0.18F);
        float flash_alpha = 0.0F;
        if (seconds <= flash_in_end) {
            flash_alpha = interval_progress(seconds, handle->timeline.p4_start, flash_in_end);
        } else {
            flash_alpha =
                1.0F - interval_progress(seconds, flash_in_end,
                                         std::max(flash_in_end, handle->timeline.p4_hold_end));
        }
        status = sao_ui_paint_ctx_fill_rect(
            handle->paint_ctx, 0.0F, 0.0F, static_cast<float>(handle->width),
            static_cast<float>(handle->height), with_alpha(0xffffffffu, flash_alpha));
        if (status == SAO_STATUS_OK) {
            const float connected_alpha =
                seconds <= handle->timeline.p4_hold_end
                    ? 1.0F
                    : 1.0F - interval_progress(seconds, handle->timeline.p4_hold_end,
                                               handle->timeline.p4_fade_end);
            const float text_size = std::max(16.0F, static_cast<float>(handle->height) * 0.075F);
            status = sao_ui_paint_ctx_draw_utf8(
                handle->paint_ctx, center_x - text_size * 4.2F, center_y - text_size * 0.5F,
                "SYSTEM >> CONNECTED", text_size, with_alpha(0xff10243bu, connected_alpha));
        }
    }

    const sao_status_t end_status = sao_ui_paint_ctx_end_frame(handle->paint_ctx);
    if (status == SAO_STATUS_OK)
        status = end_status;
    if (status != SAO_STATUS_OK)
        return status;

    size_t bytes = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t stride = 0u;
    status =
        sao_ui_offscreen_raster_snapshot(handle->raster, handle->pixels.data(),
                                         handle->pixels.size(), &bytes, &width, &height, &stride);
    if (status != SAO_STATUS_OK)
        return status;
    return sao_ui_layer_update_bgra(handle->layer, handle->pixels.data(), width, height, stride);
}

sao_status_t reset_nervegear_for_show(sao_ui_nervegear_handle_t nervegear,
                                      const SaoUiLinkStartTimeline& timeline) {
    if (nervegear == nullptr)
        return SAO_STATUS_OK;
    sao_status_t status = sao_ui_nervegear_set_timeline(nervegear, &timeline);
    if (status != SAO_STATUS_OK)
        return status;
    SaoUiNerveGearState state = SAO_UI_NG_STATE_IDLE;
    status = sao_ui_nervegear_get_state(nervegear, &state);
    if (status != SAO_STATUS_OK)
        return status;
    if (state != SAO_UI_NG_STATE_IDLE) {
        status = sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
        if (status != SAO_STATUS_OK)
            return status;
    }
    return sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_LINKING);
}

} // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_create(sao_ui_compositor_handle_t compositor,
                                                            sao_ui_nervegear_handle_t nervegear,
                                                            const SaoUiLinkStartConfig* config,
                                                            sao_ui_linkstart_handle_t* out_handle) {
    if (out_handle == nullptr || compositor == nullptr || config == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    const uint32_t declared =
        config->struct_size == 0u ? SAO_UI_LINKSTART_CONFIG_V1_SIZE : config->struct_size;
    if (declared != SAO_UI_LINKSTART_CONFIG_V1_SIZE || config->width_px == 0u ||
        config->height_px == 0u || config->width_px > 16384u || config->height_px > 16384u ||
        static_cast<uint64_t>(config->width_px) * config->height_px * 4u >
            SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const SaoUiLinkStartTimeline timeline =
        config->timeline == nullptr ? *sao_ui_nervegear_default_timeline() : *config->timeline;
    if (!validate_timeline(timeline))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;

    auto* handle = new (std::nothrow) sao_ui_linkstart_s();
    if (handle == nullptr)
        return SAO_STATUS_ERR_UNKNOWN;
    handle->compositor = compositor;
    handle->nervegear = nervegear;
    handle->timeline = timeline;
    handle->width = config->width_px;
    handle->height = config->height_px;
    handle->seed = config->particle_seed == 0u ? kDefaultParticleSeed : config->particle_seed;

    try {
        const std::string layer_name =
            "linkstart.intro." +
            std::to_string(g_linkstart_sequence.fetch_add(1u, std::memory_order_relaxed) + 1u);
        SaoLayerConfig layer_config{};
        layer_config.struct_size = sizeof(SaoLayerConfig);
        layer_config.name_utf8 = layer_name.c_str();
        layer_config.width = static_cast<int32_t>(handle->width);
        layer_config.height = static_cast<int32_t>(handle->height);
        layer_config.z_order = kLinkStartZOrder;
        layer_config.click_through = true;
        layer_config.bgra_swizzle = true;
        layer_config.high_fps = true;
        layer_config.target_fps = 60;
        sao_status_t status = sao_ui_layer_create(compositor, &layer_config, &handle->layer);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_visible(handle->layer, false);

        SaoUiOffscreenRasterDesc raster_desc{};
        raster_desc.width_px = handle->width;
        raster_desc.height_px = handle->height;
        raster_desc.clear_argb = 0xff020611u;
        if (status == SAO_STATUS_OK)
            status = sao_ui_offscreen_raster_create(&raster_desc, &handle->raster);
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_create_offscreen(handle->raster, &handle->paint_ctx);

        SaoUiParticleEmitterConfig tunnel_config{};
        tunnel_config.struct_size = sizeof(tunnel_config);
        tunnel_config.max_particles = 1024u;
        tunnel_config.seed = handle->seed;
        tunnel_config.origin_x = static_cast<float>(handle->width) * 0.5F;
        tunnel_config.origin_y = static_cast<float>(handle->height) * 0.5F;
        tunnel_config.min_speed = 35.0F;
        tunnel_config.max_speed = 210.0F;
        tunnel_config.direction_radians = 0.0F;
        tunnel_config.spread_radians = 2.0F * kPi;
        tunnel_config.min_life_seconds = 1.2F;
        tunnel_config.max_life_seconds = 3.6F;
        tunnel_config.min_size_px = 1.0F;
        tunnel_config.max_size_px = 4.0F;
        tunnel_config.damping_per_second = 0.02F;
        tunnel_config.color_start_argb = 0xff68e4ffu;
        tunnel_config.color_end_argb = 0xffd49c17u;
        if (status == SAO_STATUS_OK)
            status = create_emitter(tunnel_config, &handle->tunnel);

        SaoUiParticleEmitterConfig burst_config = tunnel_config;
        burst_config.max_particles = 512u;
        burst_config.seed = handle->seed ^ 0xa5a5a5a5u;
        burst_config.min_speed = 120.0F;
        burst_config.max_speed = 420.0F;
        burst_config.min_life_seconds = 0.45F;
        burst_config.max_life_seconds = 1.6F;
        burst_config.min_size_px = 1.5F;
        burst_config.max_size_px = 6.0F;
        if (status == SAO_STATUS_OK)
            status = create_emitter(burst_config, &handle->burst);

        if (status == SAO_STATUS_OK) {
            handle->pixels.resize(static_cast<size_t>(handle->width) * handle->height * 4u);
            status = sao_ui_nervegear_set_timeline(nervegear, &handle->timeline);
            if (nervegear == nullptr)
                status = SAO_STATUS_OK;
        }
        if (status != SAO_STATUS_OK) {
            destroy_resources(handle);
            delete handle;
            return status;
        }
        *out_handle = handle;
        return SAO_STATUS_OK;
    } catch (...) {
        destroy_resources(handle);
        delete handle;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" void SAO_UI_CALL sao_ui_linkstart_destroy(sao_ui_linkstart_handle_t handle) {
    if (handle == nullptr)
        return;
    sao_ui_nervegear_handle_t nervegear = nullptr;
    {
        std::lock_guard lock(handle->mutex);
        nervegear = handle->nervegear;
        handle->active = false;
        (void)sao_ui_layer_set_visible(handle->layer, false);
    }
    if (nervegear != nullptr)
        (void)sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
    destroy_resources(handle);
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_show(sao_ui_linkstart_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_ui_nervegear_handle_t nervegear = nullptr;
    SaoUiLinkStartTimeline timeline{};
    try {
        {
            std::lock_guard lock(handle->mutex);
            handle->elapsed_ms = 0;
            handle->active = true;
            handle->burst_spawned = false;
            handle->nervegear_sound_played = false;
            handle->welcome_sound_played = false;
            sao_status_t status = sao_ui_particle_emitter_reset(handle->tunnel, handle->seed);
            if (status == SAO_STATUS_OK) {
                status = sao_ui_particle_emitter_reset(handle->burst, handle->seed ^ 0xa5a5a5a5u);
            }
            uint32_t spawned = 0u;
            if (status == SAO_STATUS_OK)
                status = sao_ui_particle_emitter_spawn_burst(handle->tunnel, 240u, &spawned);
            if (status == SAO_STATUS_OK)
                status = render_frame_locked(handle);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->layer, true);
            if (status != SAO_STATUS_OK) {
                handle->active = false;
                (void)sao_ui_layer_set_visible(handle->layer, false);
                return status;
            }
            nervegear = handle->nervegear;
            timeline = handle->timeline;
        }
        const sao_status_t status = reset_nervegear_for_show(nervegear, timeline);
        if (status == SAO_STATUS_OK)
            (void)sao_ui_sound_play(SAO_UI_SOUND_LINK_START, 80);
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_dismiss(sao_ui_linkstart_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_ui_nervegear_handle_t nervegear = nullptr;
    sao_status_t status = SAO_STATUS_OK;
    {
        std::lock_guard lock(handle->mutex);
        handle->active = false;
        handle->elapsed_ms = 0;
        handle->burst_spawned = false;
        handle->nervegear_sound_played = false;
        handle->welcome_sound_played = false;
        status = sao_ui_layer_set_visible(handle->layer, false);
        nervegear = handle->nervegear;
    }
    if (status != SAO_STATUS_OK)
        return status;
    return nervegear == nullptr ? SAO_STATUS_OK
                                : sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_is_active(sao_ui_linkstart_handle_t handle,
                                                               bool* out_active) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_active == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    *out_active = handle->active;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_tick(sao_ui_linkstart_handle_t handle,
                                                          int32_t delta_ms) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (delta_ms < 0)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_nervegear_handle_t nervegear = nullptr;
    bool completed = false;
    bool play_nervegear_sound = false;
    bool play_welcome_sound = false;
    try {
        {
            std::lock_guard lock(handle->mutex);
            if (!handle->active)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            const int32_t previous_elapsed = handle->elapsed_ms;
            const int64_t next_elapsed = static_cast<int64_t>(handle->elapsed_ms) + delta_ms;
            handle->elapsed_ms = static_cast<int32_t>(
                std::min<int64_t>(next_elapsed, std::numeric_limits<int32_t>::max()));
            const float delta_seconds = static_cast<float>(delta_ms) / 1000.0F;
            sao_status_t status = sao_ui_particle_emitter_update(handle->tunnel, delta_seconds);
            if (status == SAO_STATUS_OK)
                status = sao_ui_particle_emitter_update(handle->burst, delta_seconds);
            const float seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
            const float previous_seconds = static_cast<float>(previous_elapsed) / 1000.0F;
            if (status == SAO_STATUS_OK && seconds < handle->timeline.p3_end) {
                uint32_t spawned = 0u;
                const uint32_t count = static_cast<uint32_t>(std::clamp(delta_ms / 8, 1, 48));
                status = sao_ui_particle_emitter_spawn_burst(handle->tunnel, count, &spawned);
            }
            if (status == SAO_STATUS_OK && !handle->burst_spawned &&
                previous_seconds < handle->timeline.p3_start &&
                seconds >= handle->timeline.p3_start) {
                uint32_t spawned = 0u;
                status = sao_ui_particle_emitter_spawn_burst(handle->burst, 220u, &spawned);
                if (status == SAO_STATUS_OK)
                    handle->burst_spawned = true;
            }
            if (status == SAO_STATUS_OK)
                status = render_frame_locked(handle);
            if (status != SAO_STATUS_OK)
                return status;
            if (!handle->nervegear_sound_played && handle->elapsed_ms >= 1500) {
                handle->nervegear_sound_played = true;
                play_nervegear_sound = true;
            }
            if (!handle->welcome_sound_played && seconds >= handle->timeline.p3_start) {
                handle->welcome_sound_played = true;
                play_welcome_sound = true;
            }
            completed = seconds >= handle->timeline.total_duration;
            if (completed) {
                handle->active = false;
                const sao_status_t hide_status = sao_ui_layer_set_visible(handle->layer, false);
                if (hide_status != SAO_STATUS_OK)
                    return hide_status;
            }
            nervegear = handle->nervegear;
        }
        if (play_nervegear_sound)
            (void)sao_ui_sound_play(SAO_UI_SOUND_NERVEGEAR, 80);
        if (play_welcome_sound)
            (void)sao_ui_sound_play(SAO_UI_SOUND_ALO_WELCOME, 80);
        if (nervegear != nullptr)
            return sao_ui_nervegear_tick(nervegear, delta_ms);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_get_phase(sao_ui_linkstart_handle_t handle,
                                                               SaoUiLinkStartPhase* out_phase,
                                                               float* out_phase_progress) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_phase == nullptr || out_phase_progress == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    const float seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
    *out_phase = phase_at(*handle, seconds, out_phase_progress);
    return SAO_STATUS_OK;
}
