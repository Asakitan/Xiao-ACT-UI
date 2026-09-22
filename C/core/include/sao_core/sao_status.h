#pragma once

#include <cstdint>

// Legacy spellings of the canonical sao/core/status.h enumeration.  Values
// are pinned to that enum (the sao_status_t ABI now shares one space); drift
// is caught by static_asserts in license_cli_dispatch.cpp, which includes
// both headers.
enum SaoStatus : int32_t {
    SAO_OK = 0,
    SAO_ERR_INVALID_ARGUMENT = -1,
    SAO_ERR_NOT_INITIALIZED = -2,
    SAO_ERR_HANDLE_INVALID = -3,
    SAO_ERR_BUFFER_TOO_SMALL = -4,
    SAO_ERR_OS_CALL_FAILED = -20,  // check sao_core_last_os_error() for the Win32 GetLastError()
    SAO_ERR_NOT_IMPLEMENTED = -5,  // legitimate during the skeleton phase; never in a "done" phase
    SAO_ERR_NOT_FOUND = -22,
    SAO_ERR_ACCESS_DENIED = -21,
    SAO_ERR_READ_FAULT = -41,
    SAO_ERR_UNKNOWN = -6,
};
