#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/angel_runtime.h"
#include "sao_plugins/loader.h"
#include "sao_plugins/lua_runtime.h"
#include "sao_plugins/sao_status.h"

typedef void (*sao_plugins_log_callback_t)(int32_t level, const char* utf8_message);

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL sao_plugins_abi_version(void);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_set_log_callback(
    sao_plugins_log_callback_t callback);
