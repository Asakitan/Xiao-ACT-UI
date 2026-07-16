// SAO Auto — logging facade.
//
// The platform ships an internal ring-buffer logger.  Every module posts
// through the same sink; consumers (e.g. the launcher) install a callback
// to route messages to disk / IPC / UI.
//
// Levels mirror Python's `logging` module scaled to /10 for a compact int:
//     0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=FATAL
//
// Design notes:
// - Log record ownership belongs to the emitter — the callback pointer is
//   only valid for the duration of the call.
// - The default sink is a no-op.  Nothing writes to stdout unless the
//   caller opts in via sao_core_set_log_callback().

#pragma once

#include <cstdarg>
#include <cstdint>

#include "sao/core/abi.h"
#include "sao/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sao_log_level_e : int32_t {
    SAO_LOG_TRACE = 0,
    SAO_LOG_DEBUG = 1,
    SAO_LOG_INFO  = 2,
    SAO_LOG_WARN  = 3,
    SAO_LOG_ERROR = 4,
    SAO_LOG_FATAL = 5,
};

// Message is always UTF-8 and null-terminated.  Category tags the source
// module (e.g. "core.process", "engine.event_bus").
typedef void (SAO_CORE_CALL* sao_log_callback_t)(
    int32_t level,
    const char* category_utf8,
    const char* message_utf8,
    void* user_data);

// Replacing or clearing the callback waits for callbacks already in flight.
// The callback may call this function reentrantly.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_set_log_callback(
    sao_log_callback_t callback, void* user_data);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_set_log_level(int32_t min_level);

SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_log(
    int32_t level,
    const char* category_utf8,
    const char* message_utf8);

// printf-style emit.  Message is truncated at 2 KB after formatting.
SAO_CORE_API sao_status_t SAO_CORE_CALL sao_core_logf(
    int32_t level,
    const char* category_utf8,
    const char* fmt_utf8,
    ...);

// Grab the last OS error captured by any core call that returned
// SAO_STATUS_ERR_OS_CALL_FAILED on this thread.  Thread-local.
SAO_CORE_API uint32_t SAO_CORE_CALL sao_core_last_os_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif
