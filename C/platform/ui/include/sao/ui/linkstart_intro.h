#pragma once

#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/nervegear.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_linkstart_s* sao_ui_linkstart_handle_t;

// LinkStart owns compositor-native GPU resources. Creation, show/tick/dismiss,
// and destruction are compositor owner-thread operations; the renderer never
// creates a second swap chain or graphics device.

enum SaoUiLinkStartPhase : int32_t {
    SAO_UI_LINKSTART_PHASE_HIDDEN = 0,
    SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL = 1,
    SAO_UI_LINKSTART_PHASE_TEXT_REVEAL = 2,
    SAO_UI_LINKSTART_PHASE_RADIAL_BURST = 3,
    SAO_UI_LINKSTART_PHASE_CONNECTED = 4,
    SAO_UI_LINKSTART_PHASE_COMPLETE = 5,
};

enum SaoUiLinkStartCompletionReason : int32_t {
    SAO_UI_LINKSTART_COMPLETION_NONE = 0,
    SAO_UI_LINKSTART_COMPLETION_NATURAL = 1,
    SAO_UI_LINKSTART_COMPLETION_SKIPPED = 2,
    SAO_UI_LINKSTART_COMPLETION_RENDER_FAILED = 3,
    SAO_UI_LINKSTART_COMPLETION_DEVICE_LOST = 4,
    SAO_UI_LINKSTART_COMPLETION_OFFLINE = 5,
    SAO_UI_LINKSTART_COMPLETION_TEARDOWN = 6,
    // Deferred completion released as a failure edge: the holder armed a
    // bootstrap hold and a driver/engine stage failed while it was held.
    SAO_UI_LINKSTART_COMPLETION_BOOTSTRAP_FAILED = 7,
};

enum SaoUiLinkStartAudioState : int32_t {
    SAO_UI_LINKSTART_AUDIO_READY = 0,
    SAO_UI_LINKSTART_AUDIO_SUPPRESSED = 1,
    SAO_UI_LINKSTART_AUDIO_DEGRADED = 2,
};

struct SaoUiLinkStartConfig {
    uint32_t struct_size;
    uint32_t width_px;
    uint32_t height_px;
    uint32_t particle_seed;
    const SaoUiLinkStartTimeline* timeline;
};

#define SAO_UI_LINKSTART_CONFIG_V1_SIZE 24u

// Bootstrap telemetry.  The launcher arms a hold, then publishes the stage it
// is currently bootstrapping so the intro covers driver/engine bring-up
// instead of finishing before it starts.
enum SaoUiLinkStartBootstrapFlags : uint32_t {
    SAO_UI_LINKSTART_BOOTSTRAP_FLAG_NONE = 0u,
    SAO_UI_LINKSTART_BOOTSTRAP_FLAG_FAILED = 1u << 0,
};

#define SAO_UI_LINKSTART_BOOTSTRAP_CAPTION_CAPACITY 48u

struct SaoUiLinkStartBootstrap {
    uint32_t struct_size;
    uint32_t stage_index;     // 0-based index of the stage being bootstrapped
    uint32_t stage_count;     // total stages; 0 → no telemetry, rail stays on time
    uint32_t flags;           // SaoUiLinkStartBootstrapFlags bits
    float stage_progress;     // 0..1 progress inside the current stage
    const char* caption_utf8; // optional stage label; truncated to the capacity
};

#define SAO_UI_LINKSTART_BOOTSTRAP_V1_SIZE 32u

// Arm the deferred completion: the intro freezes on the CONNECTED frame at
// `p4_hold_end` and stops advancing until release, so whatever the caller runs
// in the meantime happens strictly underneath the animation.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_linkstart_arm_bootstrap_hold(sao_ui_linkstart_handle_t handle);

// Publish the stage currently being bootstrapped.  Ignored while no hold is
// armed; the caption is copied under the handle mutex.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_linkstart_set_bootstrap(sao_ui_linkstart_handle_t handle,
                               const SaoUiLinkStartBootstrap* state);

// Release the hold.  `failed` non-zero completes the intro with
// SAO_UI_LINKSTART_COMPLETION_BOOTSTRAP_FAILED; zero lets the remaining
// p4_hold_end → p4_fade_end tail play and complete naturally.
SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_linkstart_release_bootstrap_hold(sao_ui_linkstart_handle_t handle, int32_t failed);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_create(sao_ui_compositor_handle_t compositor,
                                                            sao_ui_nervegear_handle_t nervegear,
                                                            const SaoUiLinkStartConfig* config,
                                                            sao_ui_linkstart_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_linkstart_destroy(sao_ui_linkstart_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_show(sao_ui_linkstart_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_dismiss(sao_ui_linkstart_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_dismiss_with_reason(
    sao_ui_linkstart_handle_t handle, SaoUiLinkStartCompletionReason reason);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_resize(sao_ui_linkstart_handle_t handle,
                                                            uint32_t width_px, uint32_t height_px,
                                                            uint32_t dpi);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_is_active(sao_ui_linkstart_handle_t handle,
                                                               bool* out_active);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_tick(sao_ui_linkstart_handle_t handle,
                                                          int32_t delta_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_get_phase(sao_ui_linkstart_handle_t handle,
                                                               SaoUiLinkStartPhase* out_phase,
                                                               float* out_phase_progress);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_poll_completion(
    sao_ui_linkstart_handle_t handle, SaoUiLinkStartCompletionReason* out_reason);

SAO_UI_API sao_status_t SAO_UI_CALL
sao_ui_linkstart_get_audio_state(sao_ui_linkstart_handle_t handle,
                                 SaoUiLinkStartAudioState* out_state, sao_status_t* out_status);

#ifdef __cplusplus
}

static_assert(sizeof(SaoUiLinkStartConfig) == SAO_UI_LINKSTART_CONFIG_V1_SIZE);
static_assert(sizeof(SaoUiLinkStartBootstrap) == SAO_UI_LINKSTART_BOOTSTRAP_V1_SIZE);
#endif
