// SAO Auto — script engine registry (factory table).
//
// One instance per process.  Every concrete engine host under
// `../plugins/*_host/` registers its vtable at DLL init time; the
// registry hands the correct vtable back to the plugin loader when a
// script asks for a specific language.
//
// Registration is idempotent — re-registering an engine with the same
// name replaces the previous entry.  Engines are never unregistered
// individually; the registry is torn down as one at process exit.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/scripting/abi.h"
#include "sao/scripting/script_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_registry_register(
    const struct SaoScriptEngineVTable* vtable);

// Query — returns SAO_STATUS_ERR_SCRIPT_UNSUPPORTED if no provider claims the
// language.
SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_registry_find(
    int32_t language,
    const struct SaoScriptEngineVTable** out_vtable);

// Enumerate — copies vtable pointers into caller buffer.  When
// out_vtables is null, *out_count receives the required count.
SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_registry_enumerate(
    const struct SaoScriptEngineVTable** out_vtables,
    size_t max_vtables,
    size_t* out_count);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_registry_unregister(int32_t language);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_registry_release_owner(
    const void* owner,
    size_t* out_removed);

#ifdef __cplusplus
}  // extern "C"
#endif
