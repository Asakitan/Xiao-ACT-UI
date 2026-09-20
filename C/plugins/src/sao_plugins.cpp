#include "sao_plugins/sao_plugins.h"
#include "sao_plugins/abi.h"

extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL sao_plugins_abi_version(void) {
    return (static_cast<uint32_t>(SAO_PLUGINS_ABI_VERSION_MAJOR) << 16u) |
           static_cast<uint32_t>(SAO_PLUGINS_ABI_VERSION_MINOR);
}
