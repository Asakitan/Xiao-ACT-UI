// SAO Auto — canonical sao_status_t enum.
//
// Every ABI function in every platform module returns one of these.  Never
// throw across the ABI boundary; never return -1 as a sentinel.
//
// Codes are grouped by hundred: 0=OK, negative = platform errors, positive
// reserved for module-specific info codes (e.g. "cache miss but retry ok").

#pragma once

#include <cstdint>

#include "sao/core/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t sao_status_t;

// clang-format off
enum sao_status_e : int32_t {
    SAO_STATUS_OK                       =    0,

    // ── generic ────────────────────────────────────────────────────
    SAO_STATUS_ERR_INVALID_ARGUMENT     =   -1,
    SAO_STATUS_ERR_NOT_INITIALIZED      =   -2,
    SAO_STATUS_ERR_HANDLE_INVALID       =   -3,
    SAO_STATUS_ERR_BUFFER_TOO_SMALL     =   -4,
    SAO_STATUS_ERR_NOT_IMPLEMENTED      =   -5,   // legitimate during skeleton phase
    SAO_STATUS_ERR_UNKNOWN              =   -6,
    SAO_STATUS_ERR_TIMEOUT              =   -7,
    SAO_STATUS_ERR_CANCELLED            =   -8,
    SAO_STATUS_ERR_ABI_MISMATCH         =   -9,   // plugin/host ABI version disagreement
    // Signals that a code path is intentionally unavailable because the
    // underlying external capability is missing
    // (real driver / real HWID / real DPAPI user context / real network
    // endpoint / non-Windows host).  Distinct from NOT_IMPLEMENTED so
    // callers can distinguish "hasn't been written yet" from "cannot be
    // written here — requires <capability>".  The stub itself must not
    // pretend a fake success; audit-friendly gating only.
    SAO_STATUS_ERR_CAPABILITY_MISSING   =  -10,

    // ── OS ─────────────────────────────────────────────────────────
    SAO_STATUS_ERR_OS_CALL_FAILED       =  -20,   // see sao_core_last_os_error()
    SAO_STATUS_ERR_ACCESS_DENIED        =  -21,
    SAO_STATUS_ERR_NOT_FOUND            =  -22,
    SAO_STATUS_ERR_ALREADY_EXISTS       =  -23,

    // ── memory / process ──────────────────────────────────────────
    SAO_STATUS_ERR_PROCESS_GONE         =  -40,
    SAO_STATUS_ERR_READ_FAULT           =  -41,   // page unmapped or protected
    SAO_STATUS_ERR_MODULE_NOT_FOUND     =  -42,

    // ── net ────────────────────────────────────────────────────────
    SAO_STATUS_ERR_NET_DOWN             =  -60,
    SAO_STATUS_ERR_NET_TLS              =  -61,
    SAO_STATUS_ERR_NET_HTTP_STATUS      =  -62,   // check attached response

    // ── engine ─────────────────────────────────────────────────────
    SAO_STATUS_ERR_TOPIC_UNKNOWN        =  -80,
    SAO_STATUS_ERR_SUBSCRIPTION_GONE    =  -81,

    // ── ui ─────────────────────────────────────────────────────────
    SAO_STATUS_ERR_DEVICE_LOST          = -100,
    SAO_STATUS_ERR_SURFACE_INVALID      = -101,

    // ── scripting ──────────────────────────────────────────────────
    SAO_STATUS_ERR_SCRIPT_LOAD          = -120,
    SAO_STATUS_ERR_SCRIPT_RUNTIME       = -121,
};
// clang-format on

// Human-readable ASCII form for logs.  Points into static storage; never
// free.  Returns "unknown" for out-of-range values (never null).
SAO_CORE_API const char* SAO_CORE_CALL sao_status_str(sao_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif
