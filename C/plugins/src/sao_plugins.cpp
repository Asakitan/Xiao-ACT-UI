#include "sao_plugins/sao_plugins.h"

extern "C" uint32_t SAO_PLUGINS_CALL sao_plugins_abi_version(void) {
    return (1u << 16) | 0u;
}
