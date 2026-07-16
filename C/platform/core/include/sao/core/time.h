// SAO Auto — monotonic + wall-clock timing.
//
// QueryPerformanceCounter under the hood; the values are scaled so
// callers get plain milliseconds/microseconds without dealing with the
// frequency.

#pragma once

#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Monotonic clock, in nanoseconds since first call.  Never goes backwards
// across a suspend/resume; wraps only after >584 years.  Use for
// deltas — do not attempt to interpret the absolute value.
SAO_CORE_API uint64_t SAO_CORE_CALL sao_core_time_now_ns(void);

// Millisecond convenience — equivalent to sao_core_time_now_ns() / 1e6.
SAO_CORE_API uint64_t SAO_CORE_CALL sao_core_time_now_ms(void);

// Wall clock as Unix epoch nanoseconds.  Uses GetSystemTimePreciseAsFileTime
// on Windows 8+.
SAO_CORE_API uint64_t SAO_CORE_CALL sao_core_time_wall_ns(void);

// Millisecond sleep that survives spurious wake-ups.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_time_sleep_ms(uint32_t millis);

// Scoped timer helper for logging — returns a token that
// sao_core_time_scope_end() converts to an elapsed nanosecond count.
typedef uint64_t sao_core_time_scope_t;

SAO_CORE_API sao_core_time_scope_t SAO_CORE_CALL sao_core_time_scope_begin(void);

SAO_CORE_API uint64_t SAO_CORE_CALL sao_core_time_scope_end(
    sao_core_time_scope_t token);

#ifdef __cplusplus
}  // extern "C"
#endif
