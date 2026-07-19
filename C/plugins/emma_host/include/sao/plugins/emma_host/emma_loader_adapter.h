#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::emma_host {

typedef struct emma_loader_adapter_owner_s* emma_loader_adapter_owner_t;

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_register_loader_adapter(emma_loader_adapter_owner_t* out_owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unregister_loader_adapter(emma_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_emma_loader_adapter_plugin_count(emma_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_loader_adapter_get_last_error(
    emma_loader_adapter_owner_t owner, void* loader_plugin_handle, char** out_utf8);

} // namespace sao::plugins::emma_host
