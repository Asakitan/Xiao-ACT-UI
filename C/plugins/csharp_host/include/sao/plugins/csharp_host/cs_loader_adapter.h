#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/plugins/csharp_host/cs_error.h"
#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::csharp_host {

typedef struct cs_loader_adapter_owner_s* cs_loader_adapter_owner_t;

// Managed hooks use static int Hook(IntPtr argument, int argumentSize).
// InitSdkPointers, OnEnable, OnDisable, and OnUnload are optional. OnLoad is
// required and receives cs_managed_plugin_context with its full struct size;
// loader plugin_context_t is never passed as SaoSdkContext. Normal OnUnload
// returns 0 to allow, 1 to veto, and any other value as an error. Load rollback
// ignores a missing/vetoing OnUnload so cleanup cannot be permanently blocked.
//
// Registers the generic precompiled-DLL C# host with loader::host_adapter_vtable.
// Source .cs entries are rejected; this path does not invoke Roslyn.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_register_loader_adapter(
    const cs_host_config* host_config, cs_loader_adapter_owner_t* out_owner);

// All loader-owned C# plugins must be logically unloaded before unregistering.
// Assemblies loaded through the current default ALC remain process-resident;
// repeat assembly load and hot reload are rejected until process restart.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unregister_loader_adapter(cs_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_cshost_loader_adapter_plugin_count(cs_loader_adapter_owner_t owner);

// Returns the canonical loader context borrowed by a loaded managed component.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_loader_adapter_get_context(
    cs_loader_adapter_owner_t owner, void* loader_plugin_handle,
    sao::plugins::loader::plugin_context_t** out_context);

// Takes and clears the adapter's last component/runtime/hook error. The caller
// releases the result with sao_plugins_cshost_free_string().
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_loader_adapter_get_last_error(cs_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle, char** out_utf8);

} // namespace sao::plugins::csharp_host
