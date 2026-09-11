#include "sao/ui/linkstart_intro.h"

#include "sao/ui/animator.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/d2d_effects.h"
#include "sao/ui/sound.h"

#include "classic_text_roles.h"
#include "layer_paint_internal.h"
#include "linkstart_renderer_d3d11.h"
#include "sound_sequence_internal.h"
#include "widget_paint_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr int32_t kReducedMotionDurationMs = 450;
// Reduced motion skips the tunnel: holding at 0.20s keeps the CONNECTED plate
// fully lit without letting the fade curve start.
constexpr int32_t kReducedMotionHoldMs = 200;
constexpr size_t kBootstrapCaptionCapacity = SAO_UI_LINKSTART_BOOTSTRAP_CAPTION_CAPACITY;
constexpr uint32_t kDefaultDpi = 96u;
constexpr uint32_t kMinimumDpi = 48u;
constexpr uint32_t kMaximumDpi = 768u;

std::atomic_uint64_t g_linkstart_sequence{};

bool valid_geometry(uint32_t width, uint32_t height, uint32_t dpi) noexcept {
    return width != 0u && height != 0u && width <= 16384u && height <= 16384u &&
           dpi >= kMinimumDpi && dpi <= kMaximumDpi &&
           static_cast<uint64_t>(width) * height * 4u <= SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES;
}

float interval_progress(float value, float start, float end) {
    if (end <= start)
        return value >= end ? 1.0F : 0.0F;
    return std::clamp((value - start) / (end - start), 0.0F, 1.0F);
}

float eased_progress(float value, float start, float end) {
    const float t = interval_progress(value, start, end);
    return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

SaoUiLinkStartTimeline sequence_timeline() noexcept {
    sao::ui::sound_detail::LinkStartAudioSnapshot audio{};
    (void)sao::ui::sound_detail::linkstart_audio_info(&audio);
    const double rate = static_cast<double>(audio.sample_rate);
    const float voice = static_cast<float>(static_cast<double>(audio.cue_end_frames[0]) / rate);
    const float first = static_cast<float>(
        static_cast<double>(audio.cue_end_frames[1] - audio.cue_end_frames[0]) / rate);
    const float second = static_cast<float>(
        static_cast<double>(audio.cue_end_frames[2] - audio.cue_end_frames[1]) / rate);
    const float end = first + second;
    const float title_duration = std::min(2.25F, second * 0.41F);
    return {voice, first, first, first + title_duration,
            first + title_duration * 0.78F, end, end - std::min(0.40F, second * 0.10F),
            end + 0.90F, end + 1.65F, end + 1.65F};
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
    uint32_t dpi{kDefaultDpi};
    uint32_t seed{kDefaultParticleSeed};
    bool default_timeline{};
    int32_t elapsed_ms{};
    bool active{};
    bool completion_emitted{};
    bool completion_pending{};
    sao::ui::sound_detail::LinkStartAudioHandle audio_playback;
    uint64_t last_audio_samples{};
    int32_t audio_stall_ms{};
    bool audio_clock_complete{};
    bool rendered_reduced_motion{};
    // Bootstrap gate.  While armed the intro freezes on the CONNECTED frame
    // and the bottom rail reports the bootstrap stage instead of the clock.
    bool bootstrap_hold_active{};
    bool bootstrap_failed{};
    uint32_t bootstrap_stage_index{};
    uint32_t bootstrap_stage_count{};
    float bootstrap_stage_progress{};
    char bootstrap_caption[kBootstrapCaptionCapacity]{};
    sao_ui_sound_group_t sound_group{};
    SaoUiLinkStartCompletionReason completion_reason{SAO_UI_LINKSTART_COMPLETION_NONE};
    SaoUiLinkStartAudioState audio_state{SAO_UI_LINKSTART_AUDIO_READY};
    sao_status_t audio_status{SAO_STATUS_OK};
    std::mutex mutex;
};

namespace {

sao_status_t release_sound_group(sao_ui_linkstart_s* handle) noexcept {
    if (handle == nullptr)
        return SAO_STATUS_OK;
    const sao_ui_sound_group_t group = std::exchange(handle->sound_group, 0);
    handle->audio_playback.reset();
    return group == 0 ? SAO_STATUS_OK : sao_ui_sound_group_destroy(group);
}

sao_status_t first_failure(sao_status_t current, sao_status_t candidate) noexcept {
    return current == SAO_STATUS_OK ? candidate : current;
}

SaoUiLinkStartCompletionReason completion_reason_for_status(sao_status_t status) noexcept {
    return status == SAO_STATUS_ERR_DEVICE_LOST ? SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST
                                                : SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED;
}

void record_audio_status_locked(sao_ui_linkstart_s* handle, sao_status_t status) noexcept {
    if (status == SAO_STATUS_OK)
        return;
    handle->audio_state = SAO_UI_LINKSTART_AUDIO_DEGRADED;
    handle->audio_status = status;
}

void stop_audio_after_error(sao_ui_linkstart_s* handle) noexcept {
    std::lock_guard lock(handle->mutex);
    record_audio_status_locked(handle, release_sound_group(handle));
}

int64_t audio_elapsed_locked(sao_ui_linkstart_s* handle, int32_t delta_ms,
                             int64_t fallback_ms) noexcept {
    if (!handle->default_timeline || !handle->audio_playback || handle->audio_clock_complete)
        return fallback_ms;
    using sao::ui::sound_detail::LinkStartPlaybackState;
    sao::ui::sound_detail::LinkStartAudioSnapshot audio{};
    sao_status_t status =
        sao::ui::sound_detail::linkstart_audio_snapshot(handle->audio_playback, &audio);
    if (status == SAO_STATUS_OK && (audio.state == LinkStartPlaybackState::playing ||
                                    audio.state == LinkStartPlaybackState::complete)) {
        if (audio.samples_played > handle->last_audio_samples ||
            audio.state == LinkStartPlaybackState::complete)
            handle->audio_stall_ms = 0;
        else
            handle->audio_stall_ms = static_cast<int32_t>(std::min<int64_t>(
                750, static_cast<int64_t>(handle->audio_stall_ms) + delta_ms));
        handle->last_audio_samples = audio.samples_played;
        if (handle->audio_stall_ms < 750) {
            handle->audio_clock_complete = audio.state == LinkStartPlaybackState::complete;
            const uint64_t rounding = handle->audio_clock_complete ? audio.sample_rate - 1u : 0u;
            const int64_t clock_ms = static_cast<int64_t>(
                (audio.samples_played * 1000u + rounding) / audio.sample_rate);
            return std::max<int64_t>(handle->elapsed_ms, clock_ms);
        }
        status = SAO_STATUS_ERR_TIMEOUT;
    }
    bool enabled = true;
    int32_t volume = 0;
    (void)sao_ui_sound_get_enabled(&enabled);
    (void)sao_ui_sound_get_volume(&volume);
    if (enabled && volume > 0)
        record_audio_status_locked(handle, status == SAO_STATUS_OK ? SAO_STATUS_ERR_CANCELLED : status);
    else if (handle->audio_state != SAO_UI_LINKSTART_AUDIO_DEGRADED)
        handle->audio_state = SAO_UI_LINKSTART_AUDIO_SUPPRESSED;
    record_audio_status_locked(handle, release_sound_group(handle));
    return fallback_ms;
}

sao_status_t complete_locked(sao_ui_linkstart_s* handle, SaoUiLinkStartCompletionReason reason,
                             sao_status_t status, bool reset_elapsed) noexcept {
    handle->active = false;
    handle->bootstrap_hold_active = false;
    if (reset_elapsed)
        handle->elapsed_ms = 0;
    sao_status_t result = status;
    result = first_failure(result, sao_ui_layer_set_visible(handle->layer, false));
    result = first_failure(result, sao_ui_layer_set_visible(handle->gpu_layer, false));
    if (handle->default_timeline || reason != SAO_UI_LINKSTART_COMPLETION_NATURAL ||
        result != SAO_STATUS_OK)
        record_audio_status_locked(handle, release_sound_group(handle));
    if (!reset_elapsed && reason == SAO_UI_LINKSTART_COMPLETION_NATURAL &&
        result != SAO_STATUS_OK)
        handle->elapsed_ms = 0;
    if (!handle->completion_emitted) {
        handle->completion_emitted = true;
        handle->completion_pending = true;
        handle->completion_reason =
            reason == SAO_UI_LINKSTART_COMPLETION_NATURAL && result != SAO_STATUS_OK
                ? completion_reason_for_status(result)
                : reason;
    }
    return result;
}

sao_status_t apply_geometry_locked(sao_ui_linkstart_s* handle, uint32_t width, uint32_t height,
                                   uint32_t dpi) noexcept {
    const uint32_t overlay_width = width;
    const uint32_t overlay_height = height;
    const int32_t overlay_x = static_cast<int32_t>((width - overlay_width) / 2u);
    const int32_t overlay_y = static_cast<int32_t>((height - overlay_height) / 2u);

    const uint32_t old_width = handle->width;
    const uint32_t old_height = handle->height;
    const uint32_t old_overlay_width = handle->overlay_width;
    const uint32_t old_overlay_height = handle->overlay_height;
    const int32_t old_overlay_x = static_cast<int32_t>((old_width - old_overlay_width) / 2u);
    const int32_t old_overlay_y = static_cast<int32_t>((old_height - old_overlay_height) / 2u);

    sao_status_t status = sao_ui_layer_set_geometry(handle->gpu_layer, 0, 0,
                                                    static_cast<int32_t>(width),
                                                    static_cast<int32_t>(height));
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_geometry(handle->layer, overlay_x, overlay_y,
                                           static_cast<int32_t>(overlay_width),
                                           static_cast<int32_t>(overlay_height));
    if (status != SAO_STATUS_OK) {
        (void)sao_ui_layer_set_geometry(handle->gpu_layer, 0, 0, static_cast<int32_t>(old_width),
                                        static_cast<int32_t>(old_height));
        (void)sao_ui_layer_set_geometry(handle->layer, old_overlay_x, old_overlay_y,
                                        static_cast<int32_t>(old_overlay_width),
                                        static_cast<int32_t>(old_overlay_height));
        return status;
    }

    handle->width = width;
    handle->height = height;
    handle->overlay_width = overlay_width;
    handle->overlay_height = overlay_height;
    handle->dpi = dpi;
    return SAO_STATUS_OK;
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
    (void)release_sound_group(handle);
}

sao_status_t centered_text(sao_ui_paint_ctx_handle_t ctx, float cx, float y, const char* text,
                           float size, uint32_t color, sao::ui::detail::ClassicTextRole role) {
    sao::ui::detail::ScopedTextRole font(role);
    float width = static_cast<float>(std::strlen(text)) * size * 0.55F;
    float height = size;
    (void)sao::ui::detail::measure_text_dwrite(text, size, &width, &height);
    return sao_ui_paint_ctx_draw_utf8(ctx, cx - width * 0.5F, y, text, size, color);
}

sao_status_t calibration_ring(sao_ui_paint_ctx_handle_t ctx, float cx, float cy, float radius,
                               float rotation, float opacity, uint32_t color) {
    for (int segment = 0; segment < 60; ++segment) {
        if (segment % 15 == 0)
            continue;
        const float a = rotation + static_cast<float>(segment) * 0.104719755F;
        const float b = a + 0.083775804F;
        const auto status = sao_ui_paint_ctx_stroke_line(ctx,
            cx + std::cos(a) * radius, cy + std::sin(a) * radius,
            cx + std::cos(b) * radius, cy + std::sin(b) * radius,
            std::clamp(radius * 0.008F, 0.75F, 1.6F), with_alpha(color, opacity));
        if (status != SAO_STATUS_OK)
            return status;
    }
    return SAO_STATUS_OK;
}

static sao_status_t interface_frame(sao_ui_paint_ctx_handle_t ctx, float cx, float cy,
                                    float width, float height, float scale, float opacity,
                                    bool paper) {
    const float left = cx - width * scale * 0.5F;
    const float top = cy - height * scale * 0.5F;
    const float right = left + width * scale;
    const float bottom = top + height * scale;
    const uint32_t accent = paper ? 0xffb58c48u : 0xff6ac9e6u;
    auto status = sao_ui_paint_ctx_fill_rounded_rect(ctx, left, top, width * scale,
        height * scale, 9.0F * scale, with_alpha(paper ? 0xf0edf2f6u : 0xe90b1928u, opacity));
    if (status == SAO_STATUS_OK)
        status = sao::ui::detail::paint_rounded_rect_stroke(ctx, left, top, width * scale,
            height * scale, 9.0F * scale, std::max(0.75F, scale),
            with_alpha(paper ? 0xffc9d6dfu : 0xff3b7188u, opacity * 0.78F));
    for (int corner = 0; corner < 4 && status == SAO_STATUS_OK; ++corner) {
        const float x = (corner & 1) ? right : left;
        const float y = (corner & 2) ? bottom : top;
        const float sx = (corner & 1) ? -scale : scale;
        const float sy = (corner & 2) ? -scale : scale;
        status = sao_ui_paint_ctx_stroke_line(ctx, x + 12.0F * sx, y + 4.0F * sy,
            x + 42.0F * sx, y + 4.0F * sy, 1.8F * scale, with_alpha(accent, opacity));
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_stroke_line(ctx, x + 4.0F * sx, y + 12.0F * sy,
                x + 4.0F * sx, y + 30.0F * sy, 1.8F * scale, with_alpha(accent, opacity));
    }
    return status;
}

template <size_t Size>
sao_status_t centered_ascii_caption(sao_ui_paint_ctx_handle_t ctx, float cx, float y,
                                     const char (&text)[Size], float size, float tracking,
                                     uint32_t color) {
    sao::ui::detail::ScopedTextRole font(sao::ui::detail::ClassicTextRole::Body);
    std::array<float, Size - 1> advances{};
    float total = 0.0F;
    for (size_t index = 0; index + 1 < Size; ++index) {
        const char glyph[]{text[index], '\0'};
        float width = 24.0F;
        float height = 48.0F;
        (void)sao::ui::detail::measure_text_dwrite(glyph, 48.0F, &width, &height);
        advances[index] = width * size / 48.0F;
        total += advances[index] + (index == 0 ? 0.0F : tracking);
    }
    float x = cx - total * 0.5F;
    for (size_t index = 0; index + 1 < Size; ++index) {
        const char glyph[]{text[index], '\0'};
        const auto status = sao_ui_paint_ctx_draw_utf8(ctx, x, y, glyph, size, color);
        if (status != SAO_STATUS_OK)
            return status;
        x += advances[index] + tracking;
    }
    return SAO_STATUS_OK;
}

float timeline_seconds(const sao_ui_linkstart_s& handle, float elapsed_seconds) {
    return handle.default_timeline
               ? std::max(0.0F, elapsed_seconds - handle.timeline.startup_prelude)
               : std::max(0.0F, elapsed_seconds);
}

// Runtime-length twin of `centered_ascii_caption`: bootstrap captions are
// composed at render time, so their length is not a template argument.
sao_status_t centered_ascii_text(sao_ui_paint_ctx_handle_t ctx, float cx, float y,
                                 const char* text, float size, float tracking, uint32_t color) {
    if (text == nullptr || text[0] == '\0')
        return SAO_STATUS_OK;
    sao::ui::detail::ScopedTextRole font(sao::ui::detail::ClassicTextRole::Body);
    const size_t length = std::strlen(text);
    float total = 0.0F;
    for (size_t index = 0; index < length; ++index) {
        const char glyph[]{text[index], '\0'};
        float width = 24.0F;
        float height = 48.0F;
        (void)sao::ui::detail::measure_text_dwrite(glyph, 48.0F, &width, &height);
        total += width * size / 48.0F;
        if (index != 0u)
            total += tracking;
    }
    float x = cx - total * 0.5F;
    for (size_t index = 0; index < length; ++index) {
        const char glyph[]{text[index], '\0'};
        const sao_status_t status = sao_ui_paint_ctx_draw_utf8(ctx, x, y, glyph, size, color);
        if (status != SAO_STATUS_OK)
            return status;
        float width = 24.0F;
        float height = 48.0F;
        (void)sao::ui::detail::measure_text_dwrite(glyph, 48.0F, &width, &height);
        x += width * size / 48.0F + tracking;
    }
    return SAO_STATUS_OK;
}

// Elapsed time at which the intro parks on the CONNECTED frame: the animated
// hold end is where the "READY TO BEGIN" plate is fully lit, and it is always
// short of `total_duration`, so a held intro can never complete on its own.
int64_t bootstrap_hold_elapsed_ms(const sao_ui_linkstart_s& handle,
                                  bool reduced_motion) noexcept {
    if (reduced_motion)
        return kReducedMotionHoldMs;
    const double prelude = handle.default_timeline
                               ? static_cast<double>(handle.timeline.startup_prelude)
                               : 0.0;
    const double hold_seconds = static_cast<double>(handle.timeline.p4_hold_end) + prelude;
    return static_cast<int64_t>(std::ceil(hold_seconds * 1000.0));
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
    } else if (scene_seconds < handle.timeline.p3_start ||
               (handle.default_timeline && scene_seconds < handle.timeline.p2_end)) {
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
    const bool reduced_motion = sao_ui_reduced_motion_enabled();
    const float elapsed_seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
    const float seconds =
        reduced_motion ? handle->timeline.p4_start +
                             (handle->default_timeline ? handle->timeline.startup_prelude : 0.0F)
                       : elapsed_seconds;
    const float scene_seconds = timeline_seconds(*handle, seconds);
    float phase_progress = 0.0F;
    const SaoUiLinkStartPhase phase = phase_at(*handle, seconds, &phase_progress);
    const float connected_alpha =
        handle->bootstrap_hold_active ? 1.0F : reduced_motion
            ? 1.0F - eased_progress(elapsed_seconds, 0.20F,
                                     static_cast<float>(kReducedMotionDurationMs) / 1000.0F)
            : 1.0F - eased_progress(scene_seconds, handle->timeline.p4_hold_end,
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
    const float ui_scale = std::min({static_cast<float>(handle->dpi) / 96.0F,
                                     static_cast<float>(handle->overlay_width) / 960.0F,
                                     static_cast<float>(handle->overlay_height) / 540.0F});
    const float welcome_duration = handle->timeline.p2_end - handle->timeline.p2_start;
    const float welcome_prepare = handle->timeline.p2_start - std::min(0.20F, welcome_duration * 0.10F);
    const float welcome_end = handle->timeline.p2_end + std::min(0.18F, welcome_duration * 0.08F);
    const bool welcome_visible = !reduced_motion && welcome_duration > 0.0F &&
                                 scene_seconds >= welcome_prepare && scene_seconds < welcome_end;
    const auto draw_motion = [&](auto&& draw, float at, float motion) {
        const float amount = reduced_motion ? 0.0F : std::clamp(motion, 0.0F, 1.0F);
        if (amount > 0.01F) {
            draw(at - 0.024F, amount * 0.045F);
            draw(at - 0.012F, amount * 0.09F);
        }
        draw(at, 1.0F);
    };
    if (status == SAO_STATUS_OK) {
        SaoUiLayerEffects effects{};
        effects.struct_size = sizeof(effects);
        effects.flags = SAO_UI_LAYER_EFFECT_SHADOW;
        effects.shadow_sigma = (welcome_visible ? 5.0F : 7.0F) * ui_scale;
        effects.shadow_argb = welcome_visible ? 0x70081724u : 0x605ac8f0u;
        status = sao_ui_layer_set_effects(handle->layer, &effects);
    }
    if (status == SAO_STATUS_OK && !reduced_motion && handle->timeline.startup_prelude > 0.0F &&
        seconds < handle->timeline.startup_prelude + 0.20F && handle->overlay_width >= 360 &&
        handle->overlay_height >= 150) {
        const auto draw_startup = [&](float sample_time, float weight) {
        const float startup = interval_progress(sample_time, 0.0F, handle->timeline.startup_prelude);
        const float entry = eased_progress(startup, 0.0F, 0.48F);
        const float opacity = eased_progress(startup, 0.0F, 0.28F) * (1.0F -
            eased_progress(sample_time, handle->timeline.startup_prelude - 0.22F,
                            handle->timeline.startup_prelude + 0.20F)) * weight;
        if (opacity < 0.002F || status != SAO_STATUS_OK)
            return;
        const float scale = ui_scale * (0.90F + entry * 0.10F);
        const float cy = center_y + 18.0F * ui_scale * (1.0F - entry);
        status = calibration_ring(paint_ctx, center_x, cy, 168.0F * scale,
            sample_time * 0.35F, opacity * 0.35F, 0xff8ad9f2u);
        if (status == SAO_STATUS_OK)
            status = calibration_ring(paint_ctx, center_x, cy, 205.0F * scale,
                -sample_time * 0.22F, opacity * 0.14F, 0xffdceaf1u);
        if (status == SAO_STATUS_OK)
            status = interface_frame(paint_ctx, center_x, cy, 438.0F, 196.0F, scale, opacity, false);
        if (status == SAO_STATUS_OK)
            status = centered_ascii_caption(paint_ctx, center_x, cy - 68.0F * scale,
                "FULLDIVE / SYSTEM LINK", 10.0F * scale, 2.2F * scale, with_alpha(0xff82b6ccu, opacity));
        if (status == SAO_STATUS_OK)
            status = centered_text(paint_ctx, center_x, cy - 29.0F * scale,
                               "NERVEGEAR", 38.0F * scale, with_alpha(0xffe8f3f9u, opacity),
                               sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK)
            status = centered_ascii_caption(paint_ctx, center_x, cy + 32.0F * scale,
                "INITIALIZING CONNECTION", 10.0F * scale, 2.0F * scale, with_alpha(0xffb4c9d7u, opacity));
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_stroke_line(paint_ctx, center_x - 174.0F * scale,
                cy + 70.0F * scale, center_x + (-174.0F + 348.0F * startup) * scale,
                cy + 70.0F * scale, 1.5F * scale, with_alpha(0xffe8bf79u, opacity));
        };
        const float travel = eased_progress(seconds, 0.0F, handle->timeline.startup_prelude * 0.48F);
        const float previous = eased_progress(seconds - 1.0F / 60.0F, 0.0F,
                                              handle->timeline.startup_prelude * 0.48F);
        draw_motion(draw_startup, seconds, std::abs(travel - previous) * 18.0F);
    }

    if (status == SAO_STATUS_OK && welcome_visible) {
        const auto draw_welcome = [&](float sample_time, float weight) {
            const float entry = eased_progress(sample_time, welcome_prepare,
                handle->timeline.p2_start + welcome_duration * 0.18F);
            const float leave = eased_progress(sample_time, handle->timeline.p2_start + welcome_duration * 0.72F,
                                               welcome_end);
            const float opacity = entry * (1.0F - leave) * weight;
            if (opacity < 0.002F || status != SAO_STATUS_OK)
                return;
            const float scale = ui_scale * (0.82F + entry * 0.18F + leave * 0.32F);
            const float cy = center_y + (30.0F * (1.0F - entry) - 14.0F * leave) * ui_scale;
            status = interface_frame(paint_ctx, center_x, cy, 620.0F, 282.0F, scale, opacity, true);
            const float text_alpha = opacity * eased_progress(sample_time, handle->timeline.p2_start,
                                                               handle->timeline.p2_start + welcome_duration * 0.24F);
            if (status == SAO_STATUS_OK)
                status = centered_ascii_caption(paint_ctx, center_x, cy - 117.0F * scale,
                    "PERSONAL / VIRTUAL INTERFACE", 9.0F * scale, 2.0F * scale, with_alpha(0xff68808fu, text_alpha));
            if (status == SAO_STATUS_OK)
                status = centered_ascii_caption(paint_ctx, center_x, cy - 79.0F * scale,
                    "WELCOME TO", 25.0F * scale, 4.0F * scale, with_alpha(0xff537185u, text_alpha));
            if (status == SAO_STATUS_OK)
                status = centered_text(paint_ctx, center_x, cy - 39.0F * scale, "SAO AUTO", 73.0F * scale,
                    with_alpha(0xff243b4bu, text_alpha), sao::ui::detail::ClassicTextRole::Display);
            if (status == SAO_STATUS_OK)
                status = centered_ascii_caption(paint_ctx, center_x, cy + 50.0F * scale,
                    "VIRTUAL DIVE INTERFACE", 10.0F * scale, 2.1F * scale, with_alpha(0xff738797u, text_alpha));
            constexpr std::array<const char*, 5> senses{"SIGHT", "HEARING", "TOUCH", "TASTE", "SMELL"};
            for (size_t index = 0; index < senses.size() && status == SAO_STATUS_OK; ++index) {
                const float x = center_x + (static_cast<float>(index) - 2.0F) * 102.0F * scale;
                const float light = eased_progress(sample_time, handle->timeline.p2_start + static_cast<float>(index) * 0.08F,
                    handle->timeline.p2_start + 0.40F + static_cast<float>(index) * 0.08F);
                status = sao_ui_paint_ctx_fill_ellipse(paint_ctx, x - 2.5F * scale, cy + 82.0F * scale,
                    5.0F * scale, 5.0F * scale, with_alpha(0xffb69154u, text_alpha * (0.25F + light * 0.75F)));
                if (status == SAO_STATUS_OK)
                    status = centered_text(paint_ctx, x, cy + 97.0F * scale, senses[index], 9.0F * scale,
                        with_alpha(0xff617886u, text_alpha), sao::ui::detail::ClassicTextRole::Body);
            }
        };
        const float entry = eased_progress(scene_seconds, welcome_prepare,
            handle->timeline.p2_start + welcome_duration * 0.18F);
        const float leave = eased_progress(scene_seconds, handle->timeline.p2_start + welcome_duration * 0.72F, welcome_end);
        const float previous_entry = eased_progress(scene_seconds - 1.0F / 60.0F, welcome_prepare,
            handle->timeline.p2_start + welcome_duration * 0.18F);
        const float previous_leave = eased_progress(scene_seconds - 1.0F / 60.0F,
            handle->timeline.p2_start + welcome_duration * 0.72F, welcome_end);
        draw_motion(draw_welcome, scene_seconds,
                    (std::abs(entry - previous_entry) + std::abs(leave - previous_leave)) * 18.0F);
    }

    if (status == SAO_STATUS_OK && scene_seconds >= handle->timeline.p4_start) {
        const float entry_end = std::min(handle->timeline.p4_start + 0.50F,
                                         handle->timeline.p4_hold_end);
        const auto draw_connected = [&](float sample_time, float weight) {
        const float entry = reduced_motion ? 1.0F : eased_progress(sample_time,
            handle->timeline.p4_start, entry_end);
        const float opacity = entry * weight;
        if (opacity < 0.002F || status != SAO_STATUS_OK)
            return;
        const float scale = ui_scale * (0.96F + 0.04F * entry);
        const float cy = center_y + 18.0F * ui_scale * (1.0F - entry);
        status = interface_frame(paint_ctx, center_x, cy, 610.0F, 252.0F, scale, opacity, false);
        if (status == SAO_STATUS_OK)
            status = calibration_ring(paint_ctx, center_x, cy - 69.0F * scale, 25.0F * scale,
                sample_time * 0.20F, opacity * 0.8F, 0xff92dbc9u);
        if (status == SAO_STATUS_OK && !handle->bootstrap_hold_active) {
            const float y = cy - 69.0F * scale;
            status = sao_ui_paint_ctx_stroke_line(paint_ctx, center_x - 9.0F * scale, y,
                center_x - 2.0F * scale, y + 7.0F * scale, 2.1F * scale, with_alpha(0xffc3f4e8u, opacity));
            if (status == SAO_STATUS_OK)
                status = sao_ui_paint_ctx_stroke_line(paint_ctx, center_x - 2.0F * scale, y + 7.0F * scale,
                    center_x + 12.0F * scale, y - 9.0F * scale, 2.1F * scale, with_alpha(0xffc3f4e8u, opacity));
        }
        if (status == SAO_STATUS_OK)
            status = centered_text(paint_ctx, center_x, cy - 21.0F * scale,
                handle->bootstrap_hold_active ? "SYSTEM >> LINKING" : "SYSTEM >> CONNECTED", 31.0F * scale,
                with_alpha(0xffe8f4f7u, opacity), sao::ui::detail::ClassicTextRole::Display);
        if (status == SAO_STATUS_OK)
            status = centered_ascii_text(paint_ctx, center_x, cy + 43.0F * scale,
                handle->bootstrap_hold_active ? "PREPARING INTERFACE" : "FULL DIVE INITIALIZED",
                12.0F * scale, 2.5F * scale, with_alpha(0xff9cbdceu, opacity));
        if (status == SAO_STATUS_OK)
            status = sao_ui_paint_ctx_stroke_line(paint_ctx, center_x - 90.0F * scale, cy + 84.0F * scale,
                center_x + 90.0F * scale, cy + 84.0F * scale, scale, with_alpha(0xff82c9b8u, opacity * 0.5F));
        };
        const float entry = eased_progress(scene_seconds, handle->timeline.p4_start,
                                            entry_end);
        const float previous = eased_progress(scene_seconds - 1.0F / 60.0F,
            handle->timeline.p4_start, entry_end);
        const float motion = scene_seconds < entry_end ? std::abs(entry - previous) * 18.0F : 0.0F;
        draw_motion(draw_connected, scene_seconds, motion);
    }

    if (status == SAO_STATUS_OK && handle->overlay_width >= 240u && handle->overlay_height >= 120u &&
        (handle->bootstrap_hold_active || scene_seconds >= handle->timeline.p4_start)) {
        const uint32_t ink = 0xff9ccee8u;
        const float rail_width = 176.0F * ui_scale;
        const float rail_y = static_cast<float>(handle->overlay_height) - 42.0F * ui_scale;
        // While the bootstrap hold is armed the rail reports driver/engine
        // stage progress instead of the animation clock.
        const bool bootstrap_telemetry =
            handle->bootstrap_hold_active && handle->bootstrap_stage_count > 0u;
        const float progress =
            bootstrap_telemetry
                ? std::clamp((static_cast<float>(handle->bootstrap_stage_index) +
                              std::clamp(handle->bootstrap_stage_progress, 0.0F, 1.0F)) /
                                 static_cast<float>(handle->bootstrap_stage_count),
                             0.0F, 1.0F)
                : (reduced_motion
                       ? interval_progress(elapsed_seconds, 0.0F, 0.45F)
                       : interval_progress(scene_seconds, 0.0F, handle->timeline.total_duration));
        const float gap = 5.0F * ui_scale;
        const float segment_width = (rail_width - gap * 3.0F) / 4.0F;
        for (int segment = 0; segment < 4 && status == SAO_STATUS_OK; ++segment) {
            const float left = center_x - rail_width * 0.5F +
                               static_cast<float>(segment) * (segment_width + gap);
            status = sao_ui_paint_ctx_stroke_line(paint_ctx, left, rail_y,
                                                  left + segment_width, rail_y, ui_scale,
                                                  with_alpha(ink, 0.18F));
            const float fill = std::clamp(progress * 4.0F - static_cast<float>(segment), 0.0F, 1.0F);
            if (status == SAO_STATUS_OK && fill > 0.0F)
                status = sao_ui_paint_ctx_stroke_line(paint_ctx, left, rail_y,
                                                      left + segment_width * fill, rail_y,
                                                      1.4F * ui_scale,
                                                      with_alpha(0xff8ac9f3u, 0.65F));
        }
        char caption[kBootstrapCaptionCapacity + 32u]{};
        if (bootstrap_telemetry) {
            const char* label =
                handle->bootstrap_caption[0] != '\0' ? handle->bootstrap_caption : "BOOTSTRAP";
            const unsigned stage_number = handle->bootstrap_stage_index + 1u;
            if (handle->bootstrap_failed) {
                (void)std::snprintf(caption, sizeof(caption), "%s %u/%u FAILED", label,
                                    stage_number, handle->bootstrap_stage_count);
            } else {
                (void)std::snprintf(caption, sizeof(caption), "%s %u/%u", label, stage_number,
                                    handle->bootstrap_stage_count);
            }
        } else {
            (void)std::snprintf(caption, sizeof(caption), "LINK SEQUENCE");
        }
        if (status == SAO_STATUS_OK)
            status = centered_ascii_text(paint_ctx, center_x,
                                         static_cast<float>(handle->overlay_height) -
                                             26.0F * ui_scale,
                                         caption, 9.0F * ui_scale, 1.6F * ui_scale,
                                         with_alpha(handle->bootstrap_failed ? 0xffd98b8bu : ink,
                                                    0.65F));
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
        const float hold_elapsed = handle->timeline.p4_hold_end +
            (handle->default_timeline ? handle->timeline.startup_prelude : 0.0F);
        const float entrance = reduced_motion ? 1.0F :
            eased_progress(elapsed_seconds, 0.0F, std::min(0.22F, hold_elapsed));
        status = sao_ui_layer_set_alpha(handle->layer, connected_alpha * entrance);
    }
    if (status == SAO_STATUS_OK) {
        handle->rendered_reduced_motion = reduced_motion;
    }
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
    if (declared != SAO_UI_LINKSTART_CONFIG_V1_SIZE ||
        !valid_geometry(config->width_px, config->height_px, kDefaultDpi)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const SaoUiLinkStartTimeline timeline =
        config->timeline == nullptr ? sequence_timeline() : *config->timeline;
    if (sao_ui_nervegear_validate_timeline(&timeline) != SAO_STATUS_OK)
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
    handle->overlay_width = config->width_px;
    handle->overlay_height = config->height_px;
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
        // A full input region keeps obscured UI unreachable until the intro ends.
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
        layer_config.rect_hit = true;
        layer_config.bgra_swizzle = true;
        layer_config.high_fps = true;
        layer_config.target_fps = 60;
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_create(compositor, &layer_config, &handle->layer);
        if (status == SAO_STATUS_OK) {
            SaoUiLayerEffects effects{};
            effects.struct_size = sizeof(effects);
            effects.flags = SAO_UI_LAYER_EFFECT_SHADOW;
            effects.shadow_sigma = 7.0F;
            effects.shadow_argb = 0x704dcaffu;
            status = sao_ui_layer_set_effects(handle->layer, &effects);
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_layer_set_visible(handle->layer, false);

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

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_resize(sao_ui_linkstart_handle_t handle,
                                                            uint32_t width_px, uint32_t height_px,
                                                            uint32_t dpi) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (dpi == 0u)
        dpi = kDefaultDpi;
    if (!valid_geometry(width_px, height_px, dpi))
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    try {
        sao_ui_nervegear_handle_t nervegear = nullptr;
        sao_status_t status = SAO_STATUS_OK;
        {
            std::lock_guard lock(handle->mutex);
            if (handle->width == width_px && handle->height == height_px && handle->dpi == dpi)
                return SAO_STATUS_OK;
            status = apply_geometry_locked(handle, width_px, height_px, dpi);
            if (status == SAO_STATUS_OK && handle->active)
                status = render_frame_locked(handle);
            if (status != SAO_STATUS_OK) {
                if (handle->active) {
                    status = complete_locked(handle, completion_reason_for_status(status), status, true);
                    nervegear = handle->nervegear;
                } else {
                    record_audio_status_locked(handle, release_sound_group(handle));
                }
            }
        }
        const sao_status_t nervegear_status =
            nervegear == nullptr
                ? SAO_STATUS_OK
                : sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
        return first_failure(status, nervegear_status);
    } catch (...) {
        stop_audio_after_error(handle);
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
        (void)complete_locked(handle, SAO_UI_LINKSTART_COMPLETION_TEARDOWN, SAO_STATUS_OK, true);
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
        sao_status_t show_status = SAO_STATUS_OK;
        {
            std::lock_guard lock(handle->mutex);
            handle->elapsed_ms = 0;
            handle->active = true;
            handle->bootstrap_hold_active = false;
            handle->bootstrap_failed = false;
            handle->bootstrap_stage_index = 0u;
            handle->bootstrap_stage_count = 0u;
            handle->bootstrap_stage_progress = 0.0F;
            handle->bootstrap_caption[0] = '\0';
            handle->completion_emitted = false;
            handle->completion_pending = false;
            handle->completion_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
            handle->last_audio_samples = 0u;
            handle->audio_stall_ms = 0;
            handle->audio_clock_complete = false;
            handle->rendered_reduced_motion = sao_ui_reduced_motion_enabled();
            const sao_status_t previous_audio_status = release_sound_group(handle);
            handle->audio_status = SAO_STATUS_OK;
            handle->audio_state = handle->rendered_reduced_motion
                                      ? SAO_UI_LINKSTART_AUDIO_SUPPRESSED
                                      : SAO_UI_LINKSTART_AUDIO_READY;
            record_audio_status_locked(handle, previous_audio_status);
            if (!handle->rendered_reduced_motion) {
                const sao_status_t group_status = sao_ui_sound_group_create(&handle->sound_group);
                record_audio_status_locked(handle, group_status);
            }
            sao_status_t status = render_frame_locked(handle);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->gpu_layer, true);
            if (status == SAO_STATUS_OK)
                status = sao_ui_layer_set_visible(handle->layer, true);
            if (status == SAO_STATUS_OK && handle->sound_group != 0) {
                const auto audio_status = sao::ui::sound_detail::linkstart_audio_begin(
                    handle->sound_group, 80, &handle->audio_playback);
                record_audio_status_locked(handle, audio_status);
                if (audio_status == SAO_STATUS_OK && !handle->audio_playback &&
                    handle->audio_state != SAO_UI_LINKSTART_AUDIO_DEGRADED)
                    handle->audio_state = SAO_UI_LINKSTART_AUDIO_SUPPRESSED;
            }
            if (status != SAO_STATUS_OK) {
                show_status =
                    complete_locked(handle, completion_reason_for_status(status), status, true);
            }
            nervegear = handle->nervegear;
            timeline = handle->timeline;
        }
        if (show_status != SAO_STATUS_OK) {
            const sao_status_t nervegear_status =
                nervegear == nullptr
                    ? SAO_STATUS_OK
                    : sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
            return first_failure(show_status, nervegear_status);
        }
        const sao_status_t status = reset_nervegear_for_show(nervegear, timeline);
        if (status != SAO_STATUS_OK) {
            {
                std::lock_guard lock(handle->mutex);
                (void)complete_locked(handle, SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED, status,
                                      true);
            }
            const sao_status_t nervegear_status =
                nervegear == nullptr
                    ? SAO_STATUS_OK
                    : sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
            return first_failure(status, nervegear_status);
        }
        return SAO_STATUS_OK;
    } catch (...) {
        sao_ui_nervegear_handle_t failed_nervegear = nullptr;
        {
            std::lock_guard lock(handle->mutex);
            (void)complete_locked(handle, SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED,
                                  SAO_STATUS_ERR_UNKNOWN, true);
            failed_nervegear = handle->nervegear;
        }
        if (failed_nervegear != nullptr)
            (void)sao_ui_nervegear_transition(failed_nervegear, SAO_UI_NG_STATE_IDLE);
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_linkstart_arm_bootstrap_hold(sao_ui_linkstart_handle_t handle) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard lock(handle->mutex);
    handle->bootstrap_hold_active = true;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_linkstart_set_bootstrap(sao_ui_linkstart_handle_t handle,
                               const SaoUiLinkStartBootstrap* state) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (state == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const uint32_t declared =
        state->struct_size == 0u ? SAO_UI_LINKSTART_BOOTSTRAP_V1_SIZE : state->struct_size;
    if (declared != SAO_UI_LINKSTART_BOOTSTRAP_V1_SIZE)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    handle->bootstrap_stage_index = state->stage_index;
    handle->bootstrap_stage_count = state->stage_count;
    handle->bootstrap_stage_progress = std::clamp(state->stage_progress, 0.0F, 1.0F);
    handle->bootstrap_failed = (state->flags & SAO_UI_LINKSTART_BOOTSTRAP_FLAG_FAILED) != 0u;
    handle->bootstrap_caption[0] = '\0';
    if (state->caption_utf8 != nullptr) {
        const size_t length =
            std::min(std::strlen(state->caption_utf8), kBootstrapCaptionCapacity - 1u);
        std::memcpy(handle->bootstrap_caption, state->caption_utf8, length);
        handle->bootstrap_caption[length] = '\0';
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_linkstart_release_bootstrap_hold(sao_ui_linkstart_handle_t handle, int32_t failed) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_ui_nervegear_handle_t nervegear = nullptr;
    bool completed = false;
    sao_status_t status = SAO_STATUS_OK;
    {
        std::lock_guard lock(handle->mutex);
        handle->bootstrap_hold_active = false;
        if (failed != 0 && handle->active) {
            status = complete_locked(handle, SAO_UI_LINKSTART_COMPLETION_BOOTSTRAP_FAILED,
                                     SAO_STATUS_OK, true);
            completed = true;
        }
        nervegear = handle->nervegear;
    }
    if (!completed)
        return SAO_STATUS_OK;
    const sao_status_t nervegear_status =
        nervegear == nullptr ? SAO_STATUS_OK
                             : sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
    return first_failure(status, nervegear_status);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_dismiss(sao_ui_linkstart_handle_t handle) {
    return sao_ui_linkstart_dismiss_with_reason(handle, SAO_UI_LINKSTART_COMPLETION_SKIPPED);
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_dismiss_with_reason(
    sao_ui_linkstart_handle_t handle, SaoUiLinkStartCompletionReason reason) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (reason < SAO_UI_LINKSTART_COMPLETION_SKIPPED ||
        reason > SAO_UI_LINKSTART_COMPLETION_BOOTSTRAP_FAILED)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_ui_nervegear_handle_t nervegear = nullptr;
    sao_status_t status = SAO_STATUS_OK;
    {
        std::lock_guard lock(handle->mutex);
        if (handle->active)
            status = complete_locked(handle, reason, SAO_STATUS_OK, true);
        else
            record_audio_status_locked(handle, release_sound_group(handle));
        nervegear = handle->nervegear;
    }
    const sao_status_t nervegear_status =
        nervegear == nullptr ? SAO_STATUS_OK
                             : sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
    return first_failure(status, nervegear_status);
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
    bool reset_nervegear = false;
    sao_status_t result = SAO_STATUS_OK;
    try {
        {
            std::lock_guard lock(handle->mutex);
            if (!handle->active)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            if (delta_ms == 0)
                return SAO_STATUS_OK;
            const int32_t previous_elapsed_ms = handle->elapsed_ms;
            const bool reduced_motion = sao_ui_reduced_motion_enabled();
            const int64_t next_elapsed = static_cast<int64_t>(previous_elapsed_ms) + delta_ms;
            int64_t clamped_elapsed =
                std::min<int64_t>(reduced_motion ? next_elapsed
                                                 : audio_elapsed_locked(handle, delta_ms, next_elapsed),
                                  std::numeric_limits<int32_t>::max());
            if (handle->bootstrap_hold_active) {
                const int64_t hold_elapsed = bootstrap_hold_elapsed_ms(*handle, reduced_motion);
                if (clamped_elapsed > hold_elapsed)
                    clamped_elapsed = std::max<int64_t>(hold_elapsed, previous_elapsed_ms);
            }
            handle->elapsed_ms = static_cast<int32_t>(clamped_elapsed);
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
            result = render_frame_locked(handle);
            if (result != SAO_STATUS_OK) {
                (void)complete_locked(handle, completion_reason_for_status(result), result, true);
                reset_nervegear = true;
                nervegear = handle->nervegear;
            }
            if (result == SAO_STATUS_OK && reduced_motion) {
                record_audio_status_locked(handle, release_sound_group(handle));
                if (handle->audio_state != SAO_UI_LINKSTART_AUDIO_DEGRADED)
                    handle->audio_state = SAO_UI_LINKSTART_AUDIO_SUPPRESSED;
            }
            const bool finished = reduced_motion ? handle->elapsed_ms >= kReducedMotionDurationMs
                                                 : scene_seconds >= handle->timeline.total_duration;
            if (result == SAO_STATUS_OK && finished) {
                result = complete_locked(handle, SAO_UI_LINKSTART_COMPLETION_NATURAL, SAO_STATUS_OK,
                                         false);
                reset_nervegear = reduced_motion || result != SAO_STATUS_OK;
            }
            if (result == SAO_STATUS_OK || nervegear == nullptr)
                nervegear = handle->nervegear;
        }
        sao_status_t nervegear_status = SAO_STATUS_OK;
        if (nervegear != nullptr && reset_nervegear)
            nervegear_status = sao_ui_nervegear_transition(nervegear, SAO_UI_NG_STATE_IDLE);
        else if (nervegear != nullptr && nervegear_delta_ms > 0)
            nervegear_status = sao_ui_nervegear_tick(nervegear, nervegear_delta_ms);
        if (nervegear_status != SAO_STATUS_OK)
            stop_audio_after_error(handle);
        return first_failure(result, nervegear_status);
    } catch (...) {
        stop_audio_after_error(handle);
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
    if (handle->completion_emitted &&
        handle->completion_reason == SAO_UI_LINKSTART_COMPLETION_NATURAL) {
        *out_phase = SAO_UI_LINKSTART_PHASE_COMPLETE;
        *out_phase_progress = 1.0F;
        return SAO_STATUS_OK;
    }
    const float elapsed_seconds = static_cast<float>(handle->elapsed_ms) / 1000.0F;
    const float seconds =
        handle->active && sao_ui_reduced_motion_enabled()
            ? handle->timeline.p4_start +
                  (handle->default_timeline ? handle->timeline.startup_prelude : 0.0F)
            : elapsed_seconds;
    *out_phase = phase_at(*handle, seconds, out_phase_progress);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_linkstart_poll_completion(
    sao_ui_linkstart_handle_t handle, SaoUiLinkStartCompletionReason* out_reason) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_reason == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    *out_reason = SAO_UI_LINKSTART_COMPLETION_NONE;
    if (handle->completion_pending) {
        *out_reason = handle->completion_reason;
        handle->completion_pending = false;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL
sao_ui_linkstart_get_audio_state(sao_ui_linkstart_handle_t handle,
                                 SaoUiLinkStartAudioState* out_state, sao_status_t* out_status) {
    if (handle == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_state == nullptr || out_status == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    *out_state = handle->audio_state;
    *out_status = handle->audio_status;
    return SAO_STATUS_OK;
}
