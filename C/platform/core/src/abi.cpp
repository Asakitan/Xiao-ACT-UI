// TODO(phase-2): keep this stable across all subsequent phases.  Bump the
// major only if a public struct in another core header changes layout.

#include "sao/core/abi.h"

extern "C" uint32_t SAO_CORE_CALL sao_core_abi_version(void) {
    return SAO_CORE_ABI_VERSION;
}
