#pragma once

#include <cstdint>

enum SaoStatus : int32_t {
    SAO_OK = 0,
    SAO_ERR_INVALID_ARGUMENT = -1,
    SAO_ERR_NOT_INITIALIZED = -2,
    SAO_ERR_HANDLE_INVALID = -3,
    SAO_ERR_BUFFER_TOO_SMALL = -4,
    SAO_ERR_OS_CALL_FAILED = -5,
    SAO_ERR_NOT_IMPLEMENTED = -6,
};
