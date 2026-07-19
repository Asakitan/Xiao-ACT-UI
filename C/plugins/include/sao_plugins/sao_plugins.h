#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/angel_runtime.h"
#include "sao_plugins/loader.h"
#include "sao_plugins/lua_runtime.h"
#include "sao_plugins/sao_status.h"

typedef void(SAO_PLUGINS_CALL* sao_plugins_log_callback_t)(int32_t level, const char* utf8_message);

enum SaoPluginsLogLevel : int32_t {
    SAO_PLUGINS_LOG_LEVEL_INFO = 1,
    SAO_PLUGINS_LOG_LEVEL_WARNING = 2,
    SAO_PLUGINS_LOG_LEVEL_ERROR = 3,
};

typedef void(SAO_PLUGINS_CALL* sao_plugins_structured_log_callback_t)(int32_t level,
                                                                      const char* utf8_component,
                                                                      int32_t status,
                                                                      const char* utf8_message,
                                                                      void* user_data);

// component/message are valid UTF-8 borrowed strings for the duration of the call. status is a
// SaoStatus value. Replacement and clear wait for calls using the prior user_data to drain; a
// callback may reenter operations or replace/clear itself without recursive logging. A
// self-reentrant setter allows its current callback invocation to finish before the outer
// operation returns.

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL sao_plugins_abi_version(void);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_set_log_callback(sao_plugins_log_callback_t callback);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_set_structured_log_callback(
    sao_plugins_structured_log_callback_t callback, void* user_data);
