#include "sao/ui/linkstart_intro.h"

#include "sao/ui/animator.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/sound.h"

#include "classic_text_roles.h"
#include "layer_paint_internal.h"
#include "linkstart_renderer_d3d11.h"
#include "widget_paint_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>

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

} // namespace

struct sao_ui_linkstart_s {
    sao_ui_compositor_handle_t compositor{};
    sao_ui_nervegear_handle_t nervegear{};
    sao_ui_layer_handle_t layer{};
    sao_ui_layer_handle_t gpu_layer{};
    sao::ui::linkstart_gpu::Renderer* gpu_renderer{};
    SaoUiLinkStartTimeline timeline{};
    uint32_t width{};
    uint32_t height{};
    uint32_t overlay_width{};
    uint32_t overlay_height{};
    uint32_t seed{kDefaultParticleSeed};
    bool default_timeline{};
    int32_t elapsed_ms{};
    bool active{};
    bool nervegear_sound_played{};
    bool welcome_sound_played{};
    sao_ui_sound_group_t sound_group{};
    int32_t rendered_motion_stage{-1};
    std::mutex mutex;
};

namespace {

void release_sound_group(sao_ui_linkstart_s* handle) noexcept {
    if (handle == nullptr)
        return;
    const sao_ui_sound_group_t group = std::exchange(handle->sound_group, 0);
    if (group != 0)
        (void)sao_ui_sound_group_destroy(group);
}

void destroy_resources(sao_ui_linkstart_s* handle) {
    if (handle == nullptr)
        return;
    sao_ui_layer_destroy(handle->layer);
    handle->layer = nullptr;
    if (handle->gpu_layer != nullptr)
        (void)sao_ui_layer_set_d3d11_render_fn(handle->gpu_layer, nullptr, nullptr);
    sao_ui_layer_destroy(handle->gpu_layer);
    handle->gpu_layer = nullptr;
    sao::ui::linkstart_gpu::destroy(handle->gpu_renderer);
    handle->gpu_renderer = nullptr;
    release_sound_group(handle);
}

sao_status_t centered_text(sao_ui_paint_ctx_handle_t ctx, float cx, float y, const char* text,
                           float size, uint32_t color, sao::ui::detail::ClassicTextRole role) {
    sao::ui::detail::ScopedTextRole font(role);
    float width = static_cast<float>(std::strlen(text)) * size * 0.55F;
    float height = size;
    (void)sao::ui::detail::measure_text_dwrite(text, size, &width, &height);
    return sao_ui_paint_ctx_draw_utf8(ctx, cx - width * 0.5F, y, text, size, color);
}

sao_status_t centered_glow_text(sao_ui_paint_ctx_handle_t ctx, float cx, float y, const char* text,
                                float size, uint32_t glow, uint32_t foreground,
                                sao::ui::detail::ClassicTextRole role) {
    sao_status_t status = centered_text(ctx, cx - 2.0F, y, text, size, glow, role);
    if (status == SAO_STATUS_OK)
        status = centered_text(ctx, cx + 2.0F, y, text, size, glow, role);
    if (status == SAO_STATUS_OK)
        status = centered_text(ctx, cx, y - 1.0F, text, size, glow, role);
    if (status == SAO_STATUS_OK)
        status = centered_text(ctx, cx, y, text, size, foreground, role);
    return status;
}

float text_width(const char* text, float size) {
    sao::ui::detail::ScopedTextRole font(sao::ui::detail::ClassicTextRole::Display);
    float width = static_cast<float>(std::strlen(text)) * size * 0.55F;
    float height = size;
    (void)sao::ui::detail::measure_text_dwrite(text, size, &width, &height);
    return width;
}

sao_status_t positioned_text(sao_ui_paint_ctx_handle_t ctx, float x, float y, const char* text,
                             float size, uint32_t color, sao::ui::detail::ClassicTextRole role) {
    sao::ui::detail::ScopedTextRole font(role);
    return sao_ui_paint_ctx_draw_utf8(ctx, x, y, text, size, color);
}

void SAO_UI_CALL skip_intro_click(int32_t button, int32_t action, int32_t, float, float,
                                  void* value) {
    if (button == 0 && action == 1 && value != nullptr)
        (void)sao_ui_linkstart_dismiss(static_cast<sao_ui_linkstart_handle_t>(value));
}

float timeline_seconds(const sao_ui_linkstart_s& handle, float elapsed_seconds) {
    return handle.default_timeline
               ? std::max(0.0F, elapsed_seconds - handle.timeline.startup_prelude)
               : std::max(0.0F, elapsed_seconds);
}

SaoUiLinkStartPhase phase_at(const sao_ui_linkstart_s& handle, float seconds, float* out_progress) {
    SaoUiLinkStartPhase phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
    float progress = 0.0F;
    const float scene_seconds = timeline_seconds(handle, seconds);
    if (!handle.active && seconds <= 0.0F) {
        phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
    } else if (seconds < handle.timeline.startup_prelude ||
               scene_seconds < handle.timeline.p2_start) {
        phase = SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL;
        progress = interval_progress(scene_seconds, 0.0F, handle.timeline.p1_end);
    } else if (scene_seconds < handle.timeline.p3_start) {
        phase = SAO_UI_LINKSTART_PHASE_TEXT_REVEAL;
        progress =
            interval_progress(scene_seconds, handle.timeline.p2_start, handle.timeline.p2_end);
    } else if (scene_seconds < handle.timeline.p4_start) {
        phase = SAO_UI_LINKSTART_PHASE_RADIAL_BURST;
        progress =
            interval_progress(scene_seconds, handle.timeline.p3_start, handle.timeline.p3_end);
    } else if (scene_seconds < handle.timeline.total_duration) {
        phase = SAO_UI_LINKSTART_PHASE_CONNECTED;
        progress =
            interval_progress(scene_seconds, handle.timeline.p4_start, handle.timeline.p4_fade_end);
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
    const float scene_seconds = timeline_seconds(*handle, seconds);
    const bool reduced_motion = sao_ui_reduced_motion_enabled();
    float phase_progress = 0.0F;
    const SaoUiLinkStartPhase phase = phase_at(*handle, seconds, &phase_progress);
    const float connected_alpha =
        scene_seconds <= handle->timeline.p4_hold_end
            ? 1.0F
            : 1.0F - interval_progress(scene_seconds, handle->timeline.p4_hold_end,
                                       handle->timeline.p4_fade_end);
    sao::ui::linkstart_gpu::FrameState gpu_frame{};
    gpu_frame.elapsed_seconds = seconds;
    gpu_frame.phase_progress = phase_progress;
    gpu_frame.phase = static_cast<float>(phase);
    gpu_frame.connected_alpha = connected_alpha;
    gpu_frame.startup_prelude = handle->timeline.startup_prelude;
    gpu_frame.p1_end = handle->timeline.p1_end;
    gpu_frame.p2_start = handle->timeline.p2_start;
    gpu_frame.p2_end = handle->timeline.p2_end;
    gpu_frame.p3_start = handle->timeline.p3_start;
    gpu_frame.p3_end = handle->timeline.p3_end;
    gpu_frame.p4_start = handle->timeline.p4_start;
    gpu_frame.p4_hold_end = handle->timeline.p4_hold_end;
    gpu_frame.p4_fade_end = handle->timeline.p4_fade_end;
    gpu_frame.total_duration = handle->timeline.total_duration;
    gpu_frame.seed = handle->seed;
    gpu_frame.reduced_motion = reduced_motion;
    gpu_frame.scene_timeline = handle->default_timeline;
    sao::ui::linkstart_gpu::update(handle->gpu_renderer, gpu_frame);
    const int32_t motion_stage =
        reduced_motion ? (scene_seconds >= handle->timeline.p4_start ? 2 : 1) : 0;
    if (motion_stage != 0 && handle->rendered_motion_stage == motion_stage)
        return SAO_STATUS_OK;
    sao_status_t status = sao_ui_layer_request_redraw(handle->gpu_layer);
    if (status != SAO_STATUS_OK)
        return status;
    sao_ui_paint_ctx_handle_t paint_ctx = nullptr;
    std::shared_ptr<const sao::ui::detail::PaintDisplayList> display_list;
    status = sao::ui::detail::create_recording_paint_context(handle->overlay_width,
                                                             handle->overlay_height, &paint_ctx);
    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_begin_frame(paint_ctx);

    const float center_x = static_cast<float>(handle->overlay_width) * 0.5F;
    const float center_y = static_cast<float>(handle->overlay_height) * 0.5F;
    if (status == SAO_STATUS_OK && !reduced_motion && seconds < handle->timeline.startup_prelude &&
        handle->overlay_width >= 360 && handle->overlay_height >= 150) {
        const float startup = interval_progress(seconds, 0.0F, handle->timeline.startup_prelude);
        const float opacity =
            reduced_motion ? 1.0F : 1.0F - interval_progress(startup, 0.72F, 1.0F);
        const float panel_width =
            std::min(560.0F, static_cast<float>(handle->overlay_width) - 48.0F);
        const float panel_height =
            std::min(122.0F, static_cast<float>(handle->overlay_height) - 32.0F);
        const float x = center_x - panel_width * 0.5F;
        const float y = center_y - panel_height * 0.5F;
        status = sao_ui_paint_ctx_fill_rounded_rect(paint_ctx, x, y, panel_width, panel_height,
                                                    18.0F, with_alpha(0xdf061426u, opacity));
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_draw_corner_brackets(paint_ctx, x, y, panel_width,
                                                           panel_height, 34.0F, 1.5F,
                                                           with_alpha(0xff6ee8ffu, opacity));
        if (status == SAO_STATUS_OK)
            status = positioned_text(paint_ctx, x + 20.0F, y + 16.0F, "SYSTEM", 11.0F,
                                     with_alpha(0xffaaeeffu, opacity),
                                     sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK) {
            constexpr char standby[] = "[ LINK STANDBY ]";
            status = positioned_text(
                paint_ctx, x + panel_width - 20.0F - text_width(standby, 11.0F), y + 16.0F, standby,
                11.0F, with_alpha(0xffffda74u, opacity), sao::ui::detail::ClassicTextRole::Display);
        }
        if (status == SAO_STATUS_OK)
            status = centered_glow_text(
                paint_ctx, center_x, y + 43.0F, "NerveGear", std::min(48.0F, panel_height * 0.34F),
                with_alpha(0x886ee8ffu, opacity), with_alpha(0xfff2fbffu, opacity),
                sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK)
            status = centered_text(
                paint_ctx, center_x, y + panel_height - 24.0F, "FULLDIVE AUTHENTICATION", 12.0F,
                with_alpha(0xffffda84u, opacity), sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK) {
            const float scan_x = x + panel_width * startup;
            status = sao_ui_paint_ctx_stroke_line(paint_ctx, scan_x, y + 8.0F, scan_x,
                                                  y + panel_height - 8.0F, 2.0F,
                                                  with_alpha(0xffbff6ffu, opacity * 0.72F));
        }
    }

    const float link_start_begin = std::max(0.0F, handle->timeline.p1_end - 0.62F);
    const float link_start_end = handle->timeline.p1_end + 0.18F;
    if (status == SAO_STATUS_OK && !reduced_motion && scene_seconds >= link_start_begin &&
        scene_seconds < link_start_end) {
        const float fade_in =
            interval_progress(scene_seconds, link_start_begin, link_start_begin + 0.20F);
        const float fade_out =
            1.0F - interval_progress(scene_seconds, handle->timeline.p1_end, link_start_end);
        const float opacity = std::min(fade_in, fade_out);
        const float title_size =
            std::max(1.0F, std::min({54.0F, static_cast<float>(handle->overlay_height) * 0.19F,
                                     static_cast<float>(handle->overlay_width) / 12.0F}));
        const float rail_width =
            std::min(460.0F, static_cast<float>(handle->overlay_width) * 0.62F);
        status = sao_ui_paint_ctx_stroke_line(
            paint_ctx, center_x - rail_width * 0.5F, center_y - title_size * 0.96F,
            center_x - title_size * 2.15F, center_y - title_size * 0.96F, 1.0F,
            with_alpha(0xff6ee8ffu, opacity * 0.54F));
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_stroke_line(
                paint_ctx, center_x + title_size * 2.15F, center_y - title_size * 0.96F,
                center_x + rail_width * 0.5F, center_y - title_size * 0.96F, 1.0F,
                with_alpha(0xffffd678u, opacity * 0.54F));
        if (status == SAO_STATUS_OK)
            status = centered_glow_text(paint_ctx, center_x, center_y - title_size * 1.42F,
                                        "LINK START", title_size, with_alpha(0x806ee8ffu, opacity),
                                        with_alpha(0xfff5fbffu, opacity),
                                        sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK)
            status = centered_text(paint_ctx, center_x, center_y - title_size * 0.20F,
                                   "NERVEGEAR // FULLDIVE READY", 12.0F,
                                   with_alpha(0xffffda84u, opacity * 0.88F),
                                   sao::ui::detail::ClassicTextRole::Display);
    }

    const bool show_text_phase = reduced_motion ? scene_seconds < handle->timeline.p4_start
                                                : scene_seconds >= handle->timeline.p2_start &&
                                                      scene_seconds < handle->timeline.p2_end;
    if (status == SAO_STATUS_OK && show_text_phase) {
        const float text_time = reduced_motion ? handle->timeline.p2_start + 0.9F : scene_seconds;
        const float fly_in_end = handle->timeline.p2_start + 0.7F;
        const float display_end = fly_in_end + 0.5F;
        const float fly_out_end = display_end + 0.55F;
        float opacity = 1.0F;
        float scale = 1.0F;
        if (text_time < fly_in_end) {
            const float t = interval_progress(text_time, handle->timeline.p2_start, fly_in_end);
            opacity = t;
            scale = 0.2F + 0.8F * (1.0F - std::pow(1.0F - t, 3.0F));
        } else if (text_time < display_end) {
            opacity = 1.0F;
        } else if (text_time < fly_out_end) {
            const float t = interval_progress(text_time, display_end, fly_out_end);
            opacity = 1.0F - t * 0.16F;
            scale = 1.0F + 2.8F * t * t * t;
        } else {
            const float t = interval_progress(text_time, fly_out_end, handle->timeline.p2_end);
            opacity = (1.0F - t) * 0.84F;
            scale = 3.8F + 4.2F * t * t * t;
        }
        const float top_size = std::min(320.0F, 42.0F * scale);
        const float title_size = std::min(360.0F, 64.0F * scale);
        const float top_y = center_y - (top_size + title_size * 0.82F) * 0.5F;
        if (scale < 2.2F) {
            const float frame_width = std::min(static_cast<float>(handle->overlay_width) - 32.0F,
                                               std::max(300.0F, title_size * 6.8F));
            status = sao_ui_paint_ctx_stroke_line(paint_ctx, center_x - frame_width * 0.5F,
                                                  center_y, center_x + frame_width * 0.5F, center_y,
                                                  1.5F, with_alpha(0xffd2f3ffu, opacity * 0.18F));
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_draw_corner_brackets(
                    paint_ctx, center_x - frame_width * 0.5F, top_y - 14.0F, frame_width,
                    top_size + title_size + 32.0F, 44.0F, 1.5F,
                    with_alpha(0xffffd678u, opacity * 0.42F));
        }
        if (status == SAO_STATUS_OK)
            status = centered_glow_text(paint_ctx, center_x, top_y, "WELCOME TO", top_size,
                                        with_alpha(0x806ee8ffu, opacity),
                                        with_alpha(0xfff5f8ffu, opacity),
                                        sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK)
            status = centered_glow_text(paint_ctx, center_x, top_y + top_size * 1.04F, "咲 ACT UI",
                                        title_size, with_alpha(0x78ffd678u, opacity),
                                        with_alpha(0xfffff8ecu, opacity),
                                        sao::ui::detail::ClassicTextRole::Display);
    }

    if (status == SAO_STATUS_OK && scene_seconds >= handle->timeline.p4_start) {
        float opacity = reduced_motion ? 1.0F
                                       : interval_progress(scene_seconds, handle->timeline.p4_start,
                                                           handle->timeline.p4_start + 0.4F);
        const float main_size =
            std::max(1.0F, std::min(38.0F, static_cast<float>(handle->overlay_width) / 22.0F));
        status = centered_glow_text(
            paint_ctx, center_x, center_y - main_size * 0.72F, "SYSTEM >> CONNECTED", main_size,
            with_alpha(0x78aaeeffu, opacity), with_alpha(0xffeaf6ffu, opacity),
            sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK)
            status = centered_text(paint_ctx, center_x, center_y + main_size * 0.72F,
                                   "FULL DIVE INITIALIZED", 15.0F,
                                   with_alpha(0xff9fd8ffu, opacity * 0.86F),
                                   sao::ui::detail::ClassicTextRole::Display);
    }

    if (status == SAO_STATUS_OK)
        status = sao_ui_paint_ctx_end_frame(paint_ctx);
    if (status == SAO_STATUS_OK)
        status = sao::ui::detail::seal_recording_paint_context(paint_ctx, &display_list);
    if (paint_ctx != nullptr)
        sao_ui_paint_ctx_destroy(paint_ctx);
    if (status == SAO_STATUS_OK)
        status = sao::ui::detail::submit_layer_paint(handle->layer, std::move(display_list),
                                                     handle->overlay_width, handle->overlay_height);
    if (status == SAO_STATUS_OK) {
        const float alpha =
            reduced_motion || scene_seconds <= handle->timeline.p4_hold_end
                ? 1.0F
                : 1.0F - interval_progress(scene_seconds, handle->timeline.p4_hold_end,
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
    handle->default_timeline = config->timeline == nullptr;
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

        if (status == SAO_STATUS_OK) {
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
        release_sound_group(handle);
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
            (void)sao_ui_sound_group_create(&handle->sound_group);
            sao_status_t status = render_frame_locked(handle);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->gpu_layer, true);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->layer, true);
            if (status == SAO_STATUS_OK && handle->sound_group != 0)
                (void)sao_ui_sound_play_in_group(SAO_UI_SOUND_LINK_START, 80, handle->sound_group);
            if (status != SAO_STATUS_OK) {
                handle->active = false;
                (void)sao_ui_layer_set_visible(handle->layer, false);
                (void)sao_ui_layer_set_visible(handle->gpu_layer, false);
                release_sound_group(handle);
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
            release_sound_group(handle);
        }
        return status;
    } catch (...) {
        sao_ui_nervegear_handle_t failed_nervegear = nullptr;
        {
            std::lock_guard lock(handle->mutex);
            handle->active = false;
            handle->elapsed_ms = 0;
            handle->nervegear_sound_played = false;
            handle->welcome_sound_played = false;
            (void)sao_ui_layer_set_visible(handle->layer, false);
            (void)sao_ui_layer_set_visible(handle->gpu_layer, false);
            release_sound_group(handle);
            failed_nervegear = handle->nervegear;
        }
        if (failed_nervegear != nullptr)
            (void)sao_ui_nervegear_transition(failed_nervegear, SAO_UI_NG_STATE_IDLE);
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
        release_sound_group(handle);
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
    int32_t nervegear_delta_ms = 0;
    try {
        {
            std::lock_guard lock(handle->mutex);
            if (!handle->active)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (delta_ms == 0)
                return SAO_STATUS_OK;
            const int32_t previous_elapsed_ms = handle->elapsed_ms;
            const int64_t next_elapsed = static_cast<int64_t>(previous_elapsed_ms) + delta_ms;
            handle->elapsed_ms = static_cast<int32_t>(
                std::min<int64_t>(next_elapsed, std::numeric_limits<int32_t>::max()));
            sao_status_t status = SAO_STATUS_OK;
            const float seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
            const float scene_seconds = timeline_seconds(*handle, seconds);
            const int64_t prelude_ms =
                handle->default_timeline
                    ? static_cast<int64_t>(std::llround(
                          static_cast<double>(handle->timeline.startup_prelude) * 1000.0))
                    : 0;
            const int64_t previous_scene_ms =
                std::max<int64_t>(0, static_cast<int64_t>(previous_elapsed_ms) - prelude_ms);
            const int64_t next_scene_ms =
                std::max<int64_t>(0, static_cast<int64_t>(handle->elapsed_ms) - prelude_ms);
            nervegear_delta_ms = static_cast<int32_t>(std::min<int64_t>(
                next_scene_ms - previous_scene_ms, std::numeric_limits<int32_t>::max()));
            status = render_frame_locked(handle);
            if (status != SAO_STATUS_OK)
                return status;
            if (!handle->nervegear_sound_played && scene_seconds >= 1.5F) {
                handle->nervegear_sound_played = true;
                if (handle->sound_group != 0)
                    (void)sao_ui_sound_play_in_group(SAO_UI_SOUND_NERVEGEAR, 80,
                                                      handle->sound_group);
            }
            if (!handle->welcome_sound_played && scene_seconds >= handle->timeline.p3_start) {
                handle->welcome_sound_played = true;
                if (handle->sound_group != 0)
                    (void)sao_ui_sound_play_in_group(SAO_UI_SOUND_ALO_WELCOME, 80,
                                                      handle->sound_group);
            }
            if (scene_seconds >= handle->timeline.total_duration) {
                handle->active = false;
                const sao_status_t hide_status = sao_ui_layer_set_visible(handle->layer, false);
                const sao_status_t hide_gpu_status =
                    sao_ui_layer_set_visible(handle->gpu_layer, false);
                if (hide_status != SAO_STATUS_OK)
                    return hide_status;
                if (hide_gpu_status != SAO_STATUS_OK)
                    return hide_gpu_status;
            }
            nervegear = handle->nervegear;
        }
        if (nervegear != nullptr && nervegear_delta_ms > 0)
            return sao_ui_nervegear_tick(nervegear, nervegear_delta_ms);
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
