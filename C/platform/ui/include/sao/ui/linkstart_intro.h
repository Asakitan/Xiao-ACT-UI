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

enum SaoUiLinkStartPhase : int32_t {
    SAO_UI_LINKSTART_PHASE_HIDDEN = 0,
    SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL = 1,
    SAO_UI_LINKSTART_PHASE_TEXT_REVEAL = 2,
    SAO_UI_LINKSTART_PHASE_RADIAL_BURST = 3,
    SAO_UI_LINKSTART_PHASE_CONNECTED = 4,
    SAO_UI_LINKSTART_PHASE_COMPLETE = 5,
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

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_is_active(sao_ui_linkstart_handle_t handle,
                                                               bool* out_active);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_tick(sao_ui_linkstart_handle_t handle,
                                                          int32_t delta_ms);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_linkstart_get_phase(sao_ui_linkstart_handle_t handle,
                                                               SaoUiLinkStartPhase* out_phase,
                                                               float* out_phase_progress);

#ifdef __cplusplus
}

static_assert(sizeof(SaoUiLinkStartConfig) == SAO_UI_LINKSTART_CONFIG_V1_SIZE);
#endif
