#pragma once

#include <cstdint>

enum SaoStatus : int32_t {
    SAO_OK = 0,
    SAO_ERR_INVALID_ARGUMENT = -1,
    SAO_ERR_NOT_INITIALIZED = -2,
    SAO_ERR_HANDLE_INVALID = -3,
    SAO_ERR_BUFFER_TOO_SMALL = -4,
    SAO_ERR_OS_CALL_FAILED = -5,   // check sao_core_last_os_error() for the Win32 GetLastError()
    SAO_ERR_NOT_IMPLEMENTED = -6,  // legitimate during the skeleton phase; never in a "done" phase
    SAO_ERR_NOT_FOUND = -7,
    SAO_ERR_ACCESS_DENIED = -8,
    SAO_ERR_READ_FAULT = -9,
    SAO_ERR_UNKNOWN = -10,
};
