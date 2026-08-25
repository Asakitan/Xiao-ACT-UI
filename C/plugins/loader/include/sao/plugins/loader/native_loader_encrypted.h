#pragma once

#include "sao_plugins/abi.h"

namespace sao::plugins::loader::native_loader {

extern "C" {

SAO_PLUGINS_API int sao_plugins_native_loader_load_encrypted(
    const char* encrypted_path, const char* key_hex, void** out_module_handle);

// The handle is the mapped image base returned by the owned load ABI. The
// caller must prove all plugin threads and callbacks are quiescent first.
SAO_PLUGINS_API int sao_plugins_native_loader_unload_encrypted(void* module_handle);

} // extern "C"

} // namespace sao::plugins::loader::native_loader
