#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"

typedef struct sao_plugins_native_s* sao_plugins_native_handle_t;

// dll_path accepts a plugin directory, its plugin.json descriptor, or the native DLL named by
// plugin.json.native_entry. The compatibility facade delegates discovery, validation, registry
// ownership, and lifecycle calls to sao_plugins_loader; it never loads the DLL directly.
//
// manifest_abi_version accepts either the native major (1/2) or the legacy major.minor encoding
// returned by sao_plugins_abi_version() when minor is zero. A mismatch maps to
// SAO_ERR_INVALID_ARGUMENT. Missing script host/provider maps to SAO_ERR_NOT_IMPLEMENTED.
// Repeated loads of the same canonical plugin path return the same handle.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_native_load(
    const wchar_t* dll_path,
    uint32_t manifest_abi_version,
    sao_plugins_native_handle_t* out_handle);

// Matches sao_plugins_native_load. Null, stale, and already-unloaded handles are no-ops.
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_native_unload(
    sao_plugins_native_handle_t handle);
