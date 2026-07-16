// SAO Auto — string helpers, UTF-8 first.
//
// The ABI language is UTF-8 everywhere except when Win32 forces wide
// chars (window titles, module names).  These helpers isolate the
// conversions in one place so callers do not sprinkle
// MultiByteToWideChar throughout their code.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

// UTF-8 -> UTF-16.  If out_wide is null, *out_wide_count receives the
// required count *including* the terminating null.  On buffer-too-small
// returns SAO_STATUS_ERR_BUFFER_TOO_SMALL with out_wide_count set.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_string_utf8_to_utf16(
    const char* utf8,
    wchar_t* out_wide,
    size_t wide_capacity,
    size_t* out_wide_count);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_string_utf16_to_utf8(
    const wchar_t* utf16,
    char* out_utf8,
    size_t utf8_capacity,
    size_t* out_utf8_count);

// Case-insensitive comparison of two null-terminated ASCII strings.
// Non-ASCII bytes compare bytewise (no locale awareness — this is a fast
// path for module names, hotkey tokens etc.).
SAO_CORE_API int32_t SAO_CORE_CALL sao_core_string_ascii_icmp(
    const char* a, const char* b);

// Trim ASCII whitespace in place.  Returns the number of trailing bytes
// removed (leading trim rewrites via memmove).
SAO_CORE_API size_t SAO_CORE_CALL sao_core_string_ascii_trim(char* buffer);

// FNV-1a 64-bit hash — used as a cheap keying primitive for topic dispatch
// tables, symbol lookups etc.  Never persist across builds without a
// version tag; the constant is not stable across major versions.
SAO_CORE_API uint64_t SAO_CORE_CALL sao_core_string_fnv1a64(
    const void* data, size_t length);

#ifdef __cplusplus
}  // extern "C"
#endif
