#include "sao/engine/abi.h"

extern "C" uint32_t SAO_ENGINE_CALL sao_engine_abi_version(void) {
    return SAO_ENGINE_ABI_VERSION;
}
