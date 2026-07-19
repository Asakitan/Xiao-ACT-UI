#include "sao_core/sao_core.h"

extern "C" uint32_t SAO_LEGACY_CORE_CALL sao_legacy_core_abi_version(void) {
    return SAO_LEGACY_CORE_ABI_VERSION;
}
