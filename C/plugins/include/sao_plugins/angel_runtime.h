#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"

typedef struct sao_plugins_angel_s* sao_plugins_angel_handle_t;

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_angel_create(
    sao_plugins_angel_handle_t* out_handle);

// Matches sao_plugins_angel_create. Safe to call with a null handle (no-op).
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_angel_destroy(
    sao_plugins_angel_handle_t handle);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_angel_run_script(
    sao_plugins_angel_handle_t handle,
    const char* script_utf8,
    size_t script_len);
