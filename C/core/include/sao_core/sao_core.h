#pragma once

#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/class_index.h"
#include "sao_core/pixels.h"
#include "sao_core/process.h"
#include "sao_core/sao_status.h"
#include "sao_core/scan.h"
#include "sao_core/window.h"

typedef void(SAO_LEGACY_CORE_CALL* sao_legacy_core_log_callback_t)(int32_t level,
                                                                   const char* utf8_message);

enum SaoLegacyCoreLogLevel : int32_t {
    SAO_LEGACY_CORE_LOG_LEVEL_INFO = 1,
    SAO_LEGACY_CORE_LOG_LEVEL_WARNING = 2,
    SAO_LEGACY_CORE_LOG_LEVEL_ERROR = 3,
};

typedef void(SAO_LEGACY_CORE_CALL* sao_legacy_core_structured_log_callback_t)(
    int32_t level, const char* utf8_component, int32_t status, const char* utf8_message,
    void* user_data);

// component/message are valid UTF-8 borrowed strings for the duration of the call. status is a
// SaoStatus value. Replacement and clear wait for calls using the prior user_data to drain; a
// callback may reenter operations or replace/clear itself without recursive logging. A
// self-reentrant setter allows its current callback invocation to finish before the outer
// operation returns.

// This probe reports the legacy core ABI. Platform core keeps the distinct
// sao_core_abi_version() probe and currently reports platform ABI 1.0.
extern "C" SAO_LEGACY_CORE_API uint32_t SAO_LEGACY_CORE_CALL sao_legacy_core_abi_version(void);

extern "C" SAO_LEGACY_CORE_API void SAO_LEGACY_CORE_CALL
sao_legacy_core_set_log_callback(sao_legacy_core_log_callback_t callback);
extern "C" SAO_LEGACY_CORE_API int32_t SAO_LEGACY_CORE_CALL
sao_legacy_core_set_structured_log_callback(sao_legacy_core_structured_log_callback_t callback,
                                            void* user_data);
