// SAO Auto — extended error context.
//
// `sao_status_t` (from status.h) is a compact int.  When a caller wants
// human-readable "why did that fail" the error-info API returns the
// last error record on the current thread.
//
// A record is written every time a core function returns a non-OK status and
// remains readable until it is replaced or explicitly cleared.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SaoErrorInfo {
    sao_status_t status;
    uint32_t os_error;          // GetLastError() at the moment of failure, or 0
    uint32_t source_line;
    uint32_t _pad;
    // 512 bytes UTF-8 message, always null-terminated.  Truncated if
    // longer.  Callers may not mutate the contents.
    char message_utf8[512];
    char source_file[128];      // basename only
    char category[64];
};

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_error_last(
    SaoErrorInfo* out_info);

// Manually set the last-error record — useful for engine-side modules
// that want to surface a rich message rather than a bare status code.
SAO_CORE_API void SAO_CORE_CALL sao_core_error_set(
    sao_status_t status,
    const char* category,
    const char* message_utf8,
    const char* source_file,
    uint32_t source_line);

// Clear the record on the current thread.  Idempotent.
SAO_CORE_API void SAO_CORE_CALL sao_core_error_clear(void);

#ifdef __cplusplus
}  // extern "C"
#endif
