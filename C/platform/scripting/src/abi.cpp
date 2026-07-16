#include "sao/scripting/abi.h"

extern "C" uint32_t SAO_SCRIPTING_CALL sao_scripting_abi_version(void) {
    return SAO_SCRIPTING_ABI_VERSION;
}
