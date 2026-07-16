// SAO Auto — scripting error codes + rich error info.
//
// Extends the core status_t namespace with scripting-specific codes.
// Actual codes live in `core/status.h` (SAO_STATUS_ERR_SCRIPT_LOAD /
// SAO_STATUS_ERR_SCRIPT_RUNTIME); this header adds a richer error
// struct that script hosts fill in.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/scripting/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SaoScriptError {
    sao_status_t status;
    int32_t      script_line;         // 1-based, 0 if unknown
    int32_t      script_column;       // 1-based, 0 if unknown
    int32_t      _pad;
    // 512 bytes UTF-8 message from the interpreter, null-terminated.
    char         message_utf8[512];
    char         source_file_utf8[256];
    char         stack_frame_utf8[512];
};

enum sao_scripting_status_e : int32_t {
    SAO_STATUS_ERR_SCRIPT_UNSUPPORTED = -122,
    SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION = -123,
};

typedef sao_status_t (SAO_SCRIPTING_CALL* sao_script_provider_call_t)(
    void* user_data);

SAO_SCRIPTING_API void SAO_SCRIPTING_CALL sao_scripting_error_clear(
    struct SaoScriptError* err);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_scripting_provider_barrier(
    sao_script_provider_call_t call,
    void* user_data,
    struct SaoScriptError* out_error);

#ifdef __cplusplus
}  // extern "C"
#endif
