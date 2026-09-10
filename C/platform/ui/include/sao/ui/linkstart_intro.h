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
#endif
