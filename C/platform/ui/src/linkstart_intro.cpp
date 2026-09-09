#include "sao/ui/linkstart_intro.h"

#include "sao/ui/animator.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/sound.h"

#include "classic_text_roles.h"
#include "linkstart_renderer_d3d11.h"
#include "widget_raster_internal.h"

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

namespace sao::ui::detail {
bool measure_text_dwrite(const char*, float, float*, float*) noexcept;
}

extern "C" SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_nervegear_tick(sao_ui_nervegear_handle_t handle, int32_t dt_ms);

namespace {

constexpr uint32_t kDefaultParticleSeed = 0x51a0c3d7u;
constexpr int32_t kLinkStartZOrder = 5000;
constexpr uint32_t kOverlayMaxWidth = 960u;
constexpr uint32_t kOverlayMaxHeight = 320u;

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
    sao_ui_layer_handle_t gpu_layer{};
    sao::ui::linkstart_gpu::Renderer* gpu_renderer{};
    sao_ui_offscreen_raster_handle_t raster{};
    sao_ui_paint_ctx_handle_t paint_ctx{};
    SaoUiLinkStartTimeline timeline{};
    uint32_t width{};
    uint32_t height{};
    uint32_t overlay_width{};
    uint32_t overlay_height{};
    uint32_t seed{kDefaultParticleSeed};
    int32_t elapsed_ms{};
    bool active{};
    bool nervegear_sound_played{};
    bool welcome_sound_played{};
    sao_ui_sound_group_t sound_group{};
    int32_t rendered_motion_stage{-1};
    std::vector<uint8_t> pixels;
    std::mutex mutex;
};

namespace {

void destroy_resources(sao_ui_linkstart_s* handle) {
    if (handle == nullptr)
        return;
    sao_ui_paint_ctx_destroy(handle->paint_ctx);
    handle->paint_ctx = nullptr;
    sao_ui_offscreen_raster_destroy(handle->raster);
    handle->raster = nullptr;
    sao_ui_layer_destroy(handle->layer);
    handle->layer = nullptr;
    if (handle->gpu_layer != nullptr)
        (void)sao_ui_layer_set_d3d11_render_fn(handle->gpu_layer, nullptr, nullptr);
    sao_ui_layer_destroy(handle->gpu_layer);
    handle->gpu_layer = nullptr;
    sao::ui::linkstart_gpu::destroy(handle->gpu_renderer);
    handle->gpu_renderer = nullptr;
    if (handle->sound_group != 0) {
        (void)sao_ui_sound_group_destroy(handle->sound_group);
        handle->sound_group = 0;
    }
}

sao_status_t centered_text(sao_ui_paint_ctx_handle_t ctx, float cx, float y, const char* text,
                           float size, uint32_t color, sao::ui::detail::ClassicTextRole role) {
    sao::ui::detail::ScopedTextRole font(role);
    float width = static_cast<float>(std::strlen(text)) * size * 0.55F;
    float height = size;
    (void)sao::ui::detail::measure_text_dwrite(text, size, &width, &height);
    return sao_ui_paint_ctx_draw_utf8(ctx, cx - width * 0.5F, y, text, size, color);
}

void SAO_UI_CALL skip_intro_click(int32_t button, int32_t action, int32_t, float, float,
                                  void* value) {
    if (button == 0 && action == 1 && value != nullptr)
        (void)sao_ui_linkstart_dismiss(static_cast<sao_ui_linkstart_handle_t>(value));
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

sao_status_t render_frame_locked(sao_ui_linkstart_s* handle) {
    const float seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
    const bool reduced_motion = sao_ui_reduced_motion_enabled();
    float phase_progress = 0.0F;
    const SaoUiLinkStartPhase phase = phase_at(*handle, seconds, &phase_progress);
    const float connected_alpha =
        seconds <= handle->timeline.p4_hold_end
            ? 1.0F
            : 1.0F - interval_progress(seconds, handle->timeline.p4_hold_end,
                                       handle->timeline.p4_fade_end);
    sao::ui::linkstart_gpu::update(handle->gpu_renderer,
                                   {seconds, phase_progress, static_cast<float>(phase),
                                    connected_alpha, handle->seed, reduced_motion});
    const int32_t motion_stage =
        reduced_motion ? (seconds >= handle->timeline.p4_start ? 2 : 1) : 0;
    if (motion_stage != 0 && handle->rendered_motion_stage == motion_stage)
        return SAO_STATUS_OK;
    sao_status_t status = sao_ui_layer_request_redraw(handle->gpu_layer);
    if (status != SAO_STATUS_OK)
        return status;
    status = sao_ui_paint_ctx_begin_frame(handle->paint_ctx);
    if (status != SAO_STATUS_OK)
        return status;
    // This is only a cropped transparent text layer, never a full-screen
    // CPU background. Clear rather than alpha-blending over the last frame.
    {
        std::lock_guard lock(handle->raster->mutex);
        std::fill(handle->raster->pixels.begin(), handle->raster->pixels.end(),
                  sao::ui::raster::BgraPixel{});
    }

    const float center_x = static_cast<float>(handle->overlay_width) * 0.5F;
    const float center_y = static_cast<float>(handle->overlay_height) * 0.5F;
    const float viewport_scale = std::min(center_x, center_y);
    if (status == SAO_STATUS_OK && !reduced_motion &&
        phase == SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL) {
        const float pulse = std::fmod(std::max(0.0F, seconds) * 0.75F, 1.0F);
        status = sao_ui_paint_ctx_draw_clock_pulse(handle->paint_ctx, center_x, center_y,
                                                   std::max(12.0F, viewport_scale * 0.62F), pulse,
                                                   1.0F, 0x6074d6e5u);
    }

    if (status == SAO_STATUS_OK && (reduced_motion || seconds >= handle->timeline.p2_start) &&
        seconds < handle->timeline.p4_start) {
        const float reveal = reduced_motion ? 1.0F
                                            : interval_progress(seconds, handle->timeline.p2_start,
                                                                handle->timeline.p2_end);
        const std::string title = reveal_text("LINK START", reveal);
        const float title_size =
            std::max(1.0F, std::min({56.0F, static_cast<float>(handle->overlay_height) * 0.19F,
                                     static_cast<float>(handle->overlay_width) / 12.0F}));
        const float title_alpha =
            1.0F -
            interval_progress(seconds, handle->timeline.p3_start + 0.6F, handle->timeline.p4_start);
        status = centered_text(handle->paint_ctx, center_x, center_y - title_size, title.c_str(),
                               title_size, with_alpha(0xfff8f8f8u, title_alpha),
                               sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK && reveal > 0.45F) {
            status = centered_text(
                handle->paint_ctx, center_x, center_y + title_size * 0.4F, "NERVEGEAR / LINK START",
                title_size * 0.4F,
                with_alpha(0xffbcc4cau, title_alpha * interval_progress(reveal, 0.45F, 1.0F)),
                sao::ui::detail::ClassicTextRole::Display);
        }
    }

    if (status == SAO_STATUS_OK && !reduced_motion && handle->overlay_width > 48 &&
        handle->overlay_height > 48 && seconds >= handle->timeline.p3_start &&
        seconds <= handle->timeline.p3_end) {
        const float burst_progress =
            interval_progress(seconds, handle->timeline.p3_start, handle->timeline.p3_end);
        status = sao_ui_paint_ctx_draw_corner_brackets(
            handle->paint_ctx, 12.0F, 12.0F, static_cast<float>(handle->overlay_width) - 24.0F,
            static_cast<float>(handle->overlay_height) - 24.0F,
            std::max(10.0F, std::min(handle->overlay_width, handle->overlay_height) * 0.08F), 1.5F,
            with_alpha(0xfff0be62u, 1.0F - burst_progress * 0.5F));
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
        // The white-field transition is GPU-rendered over the whole viewport.
        (void)flash_alpha;
        if (status == SAO_STATUS_OK) {
            const float text_size =
                std::max(1.0F, std::min({42.0F, static_cast<float>(handle->overlay_height) * 0.16F,
                                         static_cast<float>(handle->overlay_width) / 10.0F}));
            status = centered_text(
                handle->paint_ctx, center_x, center_y - text_size * 0.5F, "WELCOME TO SAO",
                text_size,
                with_alpha(0xff646364u, reduced_motion
                                            ? 1.0F
                                            : interval_progress(seconds, handle->timeline.p4_start,
                                                                handle->timeline.p4_start + 0.32F)),
                sao::ui::detail::ClassicTextRole::Display);
        }
    }

    if (status == SAO_STATUS_OK && handle->overlay_width >= 160 && handle->overlay_height >= 120) {
        const float progress = reduced_motion
                                   ? (motion_stage == 2 ? 1.0F : 0.0F)
                                   : interval_progress(seconds, 0.0F, handle->timeline.p4_start);
        const float rail_width =
            std::min(320.0F, static_cast<float>(handle->overlay_width) * 0.55F);
        const float segment = rail_width / 5.0F;
        for (int32_t index = 0; index < 5 && status == SAO_STATUS_OK; ++index) {
            status = sao_ui_paint_ctx_fill_rounded_rect(
                handle->paint_ctx,
                center_x - rail_width * 0.5F + segment * static_cast<float>(index),
                static_cast<float>(handle->overlay_height) * 0.82F, segment - 5.0F, 3.0F, 1.5F,
                progress >= static_cast<float>(index + 1) / 5.0F ? 0xfff3af12u : 0xffbcc4cau);
        }
        if (status == SAO_STATUS_OK)
            status = centered_text(handle->paint_ctx, center_x,
                                   static_cast<float>(handle->overlay_height) - 32.0F, "SKIP INTRO",
                                   13.0F,
                                   seconds >= handle->timeline.p4_start ? 0xff646364u : 0xffbcc4cau,
                                   sao::ui::detail::ClassicTextRole::Body);
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
    status = sao_ui_layer_update_bgra(handle->layer, handle->pixels.data(), width, height, stride);
    if (status == SAO_STATUS_OK) {
        const float alpha = reduced_motion || seconds <= handle->timeline.p4_hold_end
                                ? 1.0F
                                : 1.0F - interval_progress(seconds, handle->timeline.p4_hold_end,
                                                           handle->timeline.p4_fade_end);
        status = sao_ui_layer_set_alpha(handle->layer, alpha);
    }
    if (status == SAO_STATUS_OK)
        handle->rendered_motion_stage = motion_stage;
    return status;
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
    handle->overlay_width = std::min(config->width_px, kOverlayMaxWidth);
    handle->overlay_height = std::min(config->height_px, kOverlayMaxHeight);
    handle->seed = config->particle_seed == 0u ? kDefaultParticleSeed : config->particle_seed;

    try {
        const std::string base_name =
            "linkstart.intro." +
            std::to_string(g_linkstart_sequence.fetch_add(1u, std::memory_order_relaxed) + 1u);
        const std::string gpu_name = base_name + ".gpu";
        SaoLayerConfig gpu_config{};
        gpu_config.struct_size = sizeof(SaoLayerConfig);
        gpu_config.name_utf8 = gpu_name.c_str();
        gpu_config.width = static_cast<int32_t>(handle->width);
        gpu_config.height = static_cast<int32_t>(handle->height);
        gpu_config.z_order = kLinkStartZOrder;
        // The host's window region also clips DComp output. The full-screen
        // intro therefore owns a full logical region until dismissal; no
        // alpha readback is needed and clicks never reach obscured UI.
        gpu_config.click_through = false;
        gpu_config.rect_hit = true;
        gpu_config.bgra_swizzle = true;
        gpu_config.high_fps = true;
        gpu_config.target_fps = 60;
        sao_status_t status = sao::ui::linkstart_gpu::create(&handle->gpu_renderer);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_create(compositor, &gpu_config, &handle->gpu_layer);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_d3d11_render_fn(
                handle->gpu_layer, &sao::ui::linkstart_gpu::render, handle->gpu_renderer);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_visible(handle->gpu_layer, false);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_input_callbacks(handle->gpu_layer, nullptr, nullptr,
                                                      skip_intro_click, nullptr, handle);

        const std::string overlay_name = base_name + ".status";
        SaoLayerConfig layer_config{};
        layer_config.struct_size = sizeof(SaoLayerConfig);
        layer_config.name_utf8 = overlay_name.c_str();
        layer_config.x = static_cast<int32_t>((handle->width - handle->overlay_width) / 2u);
        layer_config.y = static_cast<int32_t>((handle->height - handle->overlay_height) / 2u);
        layer_config.width = static_cast<int32_t>(handle->overlay_width);
        layer_config.height = static_cast<int32_t>(handle->overlay_height);
        layer_config.z_order = kLinkStartZOrder + 1;
        layer_config.click_through = false;
        layer_config.bgra_swizzle = true;
        layer_config.high_fps = true;
        layer_config.target_fps = 60;
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_create(compositor, &layer_config, &handle->layer);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_visible(handle->layer, false);
        if (status == SAO_STATUS_OK && handle->overlay_width >= 160 &&
            handle->overlay_height >= 120) {
            const SaoUiLayerInputRect skip_rect{
                static_cast<int32_t>(handle->overlay_width / 2) - 80,
                static_cast<int32_t>(handle->overlay_height) - 38, 160, 32};
            status = sao_ui_layer_set_input_rects(handle->layer, &skip_rect, 1);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_input_callbacks(handle->layer, nullptr, nullptr,
                                                          skip_intro_click, nullptr, handle);
        } else if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_input_enabled(handle->layer, false);
        }

        SaoUiOffscreenRasterDesc raster_desc{};
        raster_desc.width_px = handle->overlay_width;
        raster_desc.height_px = handle->overlay_height;
        raster_desc.clear_argb = 0x00000000u;
        if (status == SAO_STATUS_OK)
            status = sao_ui_offscreen_raster_create(&raster_desc, &handle->raster);
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_create_offscreen(handle->raster, &handle->paint_ctx);

        if (status == SAO_STATUS_OK) {
            handle->pixels.resize(static_cast<size_t>(handle->overlay_width) *
                                  handle->overlay_height * 4u);
            if (nervegear != nullptr)
                status = sao_ui_nervegear_set_timeline(nervegear, &handle->timeline);
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
        (void)sao_ui_layer_set_visible(handle->gpu_layer, false);
        if (handle->sound_group != 0)
            (void)sao_ui_sound_group_stop(handle->sound_group);
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
            handle->nervegear_sound_played = false;
            handle->welcome_sound_played = false;
            handle->rendered_motion_stage = -1;
            if (handle->sound_group != 0) {
                (void)sao_ui_sound_group_destroy(handle->sound_group);
                handle->sound_group = 0;
            }
            sao_status_t status = sao_ui_sound_group_create(&handle->sound_group);
            if (status == SAO_STATUS_OK)
                status = render_frame_locked(handle);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->gpu_layer, true);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->layer, true);
            if (status == SAO_STATUS_OK)
                (void)sao_ui_sound_play_in_group(SAO_UI_SOUND_LINK_START, 80, handle->sound_group);
            if (status != SAO_STATUS_OK) {
                handle->active = false;
                (void)sao_ui_layer_set_visible(handle->layer, false);
                (void)sao_ui_layer_set_visible(handle->gpu_layer, false);
                if (handle->sound_group != 0)
                    (void)sao_ui_sound_group_stop(handle->sound_group);
                return status;
            }
            nervegear = handle->nervegear;
            timeline = handle->timeline;
        }
        const sao_status_t status = reset_nervegear_for_show(nervegear, timeline);
        if (status != SAO_STATUS_OK) {
            std::lock_guard lock(handle->mutex);
            handle->active = false;
            (void)sao_ui_layer_set_visible(handle->layer, false);
            (void)sao_ui_layer_set_visible(handle->gpu_layer, false);
            if (handle->sound_group != 0)
                (void)sao_ui_sound_group_stop(handle->sound_group);
        }
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
        handle->nervegear_sound_played = false;
        handle->welcome_sound_played = false;
        status = sao_ui_layer_set_visible(handle->layer, false);
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_visible(handle->gpu_layer, false);
        if (handle->sound_group != 0)
            (void)sao_ui_sound_group_stop(handle->sound_group);
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
    sao_ui_sound_group_t sound_group = 0;
    try {
        {
            std::lock_guard lock(handle->mutex);
            if (!handle->active)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (delta_ms == 0)
                return SAO_STATUS_OK;
            const int64_t next_elapsed = static_cast<int64_t>(handle->elapsed_ms) + delta_ms;
            handle->elapsed_ms = static_cast<int32_t>(
                std::min<int64_t>(next_elapsed, std::numeric_limits<int32_t>::max()));
            sao_status_t status = SAO_STATUS_OK;
            const float seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
            status = render_frame_locked(handle);
            if (status != SAO_STATUS_OK)
                return status;
            if (!handle->nervegear_sound_played && handle->elapsed_ms >= 1500) {
                handle->nervegear_sound_played = true;
                play_nervegear_sound = true;
            }
            if (!handle->welcome_sound_played && seconds >= handle->timeline.p4_start) {
                handle->welcome_sound_played = true;
                play_welcome_sound = true;
            }
            completed = seconds >= handle->timeline.total_duration;
            if (completed) {
                handle->active = false;
                const sao_status_t hide_status = sao_ui_layer_set_visible(handle->layer, false);
                if (hide_status != SAO_STATUS_OK)
                    return hide_status;
                const sao_status_t hide_gpu_status =
                    sao_ui_layer_set_visible(handle->gpu_layer, false);
                if (hide_gpu_status != SAO_STATUS_OK)
                    return hide_gpu_status;
            }
            nervegear = handle->nervegear;
            sound_group = handle->sound_group;
        }
        if (play_nervegear_sound)
            (void)sao_ui_sound_play_in_group(SAO_UI_SOUND_NERVEGEAR, 80, sound_group);
        if (play_welcome_sound)
            (void)sao_ui_sound_play_in_group(SAO_UI_SOUND_WELCOME, 80, sound_group);
        if (completed && sound_group != 0)
            (void)sao_ui_sound_group_stop(sound_group);
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
