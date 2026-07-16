#include "sao_core/sao_core.h"

static sao_log_callback_t g_log_callback = nullptr;

extern "C" uint32_t SAO_CORE_CALL sao_core_abi_version(void) {
    return (1u << 16) | 1u;
}

extern "C" void SAO_CORE_CALL sao_core_set_log_callback(sao_log_callback_t callback) {
    g_log_callback = callback;
}
