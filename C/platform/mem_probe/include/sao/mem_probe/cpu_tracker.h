// mem_probe/cpu_tracker.h — per-process CPU% via NtQuerySystemInformation.
// Phase 14 (Python parity closure) — port of python/mem_probe/cpu_tracker.py.

#pragma once

#include "sao/core/status.h"

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Sample current CPU% for a pid. Returns percent (0..100) via out_percent.
// Requires two calls at least 250ms apart per pid to produce a stable reading.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_memprobe_cpu_tracker_sample(
    uint32_t pid, double* out_percent);

// Reset tracking state for a pid (call when target restarts).
SAO_CORE_API void SAO_CORE_CALL sao_memprobe_cpu_tracker_reset(uint32_t pid);

#ifdef __cplusplus
} // extern "C"
#endif
