// SAO Auto — low-level auto-reset / manual-reset event primitives.
//
// Used for signalling between the capture, scan, UI and timer threads.
// This is deliberately smaller than `engine/event_bus.h` (which owns the
// declarative publish/subscribe channel used by plugins).

#pragma once

#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_core_wait_event_s* sao_core_wait_event_handle_t;

enum sao_core_wait_reset_e : int32_t {
    SAO_WAIT_RESET_AUTO = 0,
    SAO_WAIT_RESET_MANUAL = 1,
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_wait_event_create(
    int32_t reset_mode,
    bool initial_signalled,
    sao_core_wait_event_handle_t* out_event);

SAO_CORE_API void SAO_CORE_CALL sao_core_wait_event_destroy(
    sao_core_wait_event_handle_t event);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_wait_event_signal(
    sao_core_wait_event_handle_t event);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_wait_event_reset(
    sao_core_wait_event_handle_t event);

// Wait up to timeout_ms.  timeout_ms == 0xFFFFFFFF means infinite.
// Returns SAO_STATUS_ERR_TIMEOUT on timeout; SAO_STATUS_OK on signal.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_wait_event_wait(
    sao_core_wait_event_handle_t event, uint32_t timeout_ms);

#ifdef __cplusplus
}  // extern "C"
#endif
