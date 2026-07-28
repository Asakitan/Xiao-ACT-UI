// core/perf_probe.h — lightweight per-section profiler.
//
// Phase 14 (Python parity closure) — port of python/utils/perf_probe.py.
// p50/p95/p99/max wall-clock per named section; zero cost when disabled.

#pragma once

#include "sao/core/status.h"

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Enable/disable global profiling.
SAO_CORE_API void SAO_CORE_CALL sao_perf_probe_set_enabled(int enabled);
SAO_CORE_API int SAO_CORE_CALL sao_perf_probe_is_enabled(void);

// Begin/end a section; sections may nest per-thread via distinct keys.
// Returns an opaque timestamp cookie to pass to end.
SAO_CORE_API uint64_t SAO_CORE_CALL sao_perf_probe_begin(const char* section_utf8);
SAO_CORE_API void SAO_CORE_CALL sao_perf_probe_end(const char* section_utf8,
                                                    uint64_t begin_cookie);

// Read percentile stats for a section (nanoseconds).
typedef struct sao_perf_probe_stats_s {
    uint64_t count;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
    uint64_t total_ns;
} sao_perf_probe_stats_t;

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_perf_probe_get_stats(
    const char* section_utf8, sao_perf_probe_stats_t* out_stats);

// Reset a section's counters (or all if section == null).
SAO_CORE_API void SAO_CORE_CALL sao_perf_probe_reset(const char* section_utf8);

#ifdef __cplusplus
} // extern "C"
#endif
