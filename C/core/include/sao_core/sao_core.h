#pragma once

#include <cstdint>

#include "sao_core/abi.h"
#include "sao_core/class_index.h"
#include "sao_core/pixels.h"
#include "sao_core/process.h"
#include "sao_core/scan.h"
#include "sao_core/sao_status.h"
#include "sao_core/window.h"

typedef void (*sao_log_callback_t)(int32_t level, const char* utf8_message);

extern "C" SAO_CORE_API uint32_t SAO_CORE_CALL sao_core_abi_version(void);

extern "C" SAO_CORE_API void SAO_CORE_CALL sao_core_set_log_callback(
    sao_log_callback_t callback);
