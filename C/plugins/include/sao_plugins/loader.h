#pragma once

#include <cstddef>
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
// If load fails after runtime ownership was acquired and cleanup is still busy, the function
// returns the load error with a non-null out_handle. The caller owns that live handle and must
// retry sao_plugins_legacy_native_unload_status() until teardown succeeds.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_native_load(const wchar_t* dll_path, uint32_t manifest_abi_version,
                        sao_plugins_native_handle_t* out_handle);

// Matches sao_plugins_native_load. Null, stale, and already-unloaded handles are no-ops.
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_native_unload(sao_plugins_native_handle_t handle);

// Status-returning legacy-facade variant. A concurrent unload of the same live handle returns
// the loader BUSY status instead of reporting a false success. The void entry point remains
// ABI-compatible and delegates to this operation.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_legacy_native_unload_status(sao_plugins_native_handle_t handle);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_native_last_status(sao_plugins_native_handle_t handle, int32_t* out_status);

// out_required includes the trailing null. A null or short output buffer
// returns SAO_ERR_BUFFER_TOO_SMALL without clearing the retained diagnostic.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_native_last_error(sao_plugins_native_handle_t handle, char* out_error_utf8,
                              size_t out_capacity, size_t* out_required);
