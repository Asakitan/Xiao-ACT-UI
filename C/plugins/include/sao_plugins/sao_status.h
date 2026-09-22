#pragma once

#include <cstdint>

// Plugin-facing spellings of the canonical sao/core/status.h values.  Kept
// as literals (not an include) so this header stays dependency-free; drift
// is asserted in launcher/src/plugins_provider.cpp.
enum SaoStatus : int32_t {
    SAO_OK = 0,
    SAO_ERR_INVALID_ARGUMENT = -1,
    SAO_ERR_NOT_INITIALIZED = -2,
    SAO_ERR_HANDLE_INVALID = -3,
    SAO_ERR_BUFFER_TOO_SMALL = -4,
    SAO_ERR_OS_CALL_FAILED = -20,
    SAO_ERR_NOT_IMPLEMENTED = -5,
    SAO_ERR_UNKNOWN = -6,
};
