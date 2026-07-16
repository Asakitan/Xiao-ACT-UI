#include "sao_plugins/sao_plugins.h"

namespace {

sao_plugins_log_callback_t g_log_callback = nullptr;

}  // namespace

extern "C" uint32_t SAO_PLUGINS_CALL sao_plugins_abi_version(void) {
    return (1u << 16) | 0u;
}

extern "C" void SAO_PLUGINS_CALL sao_plugins_set_log_callback(
    sao_plugins_log_callback_t callback) {
    g_log_callback = callback;
}
