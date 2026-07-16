#include "sao/plugins/loader/plugin_isolation.h"
#include "plugin_internal.h"

namespace sao::plugins::loader {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_isolation_arm(plugin_handle_t plugin,
                          const isolation_config* cfg) {
    if (retain_plugin(plugin) == nullptr || cfg == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    return SAO_PLUGINS_ERR_UNSUPPORTED;
}

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_isolation_record_failure(plugin_handle_t plugin,
                                     const char* utf8_error_message) {
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr) return;
    std::lock_guard lock(retained->mutex);
    ++retained->failure_count;
    retained->last_error = utf8_error_message ? utf8_error_message : "plugin failure";
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_isolation_failure_stats(plugin_handle_t plugin,
                                    uint32_t* out_failure_count,
                                    const char** out_last_error_utf8) {
    thread_local std::string last_error;
    const auto retained = retain_plugin(plugin);
    if (retained == nullptr || out_failure_count == nullptr || out_last_error_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(retained->mutex);
    *out_failure_count = retained->failure_count;
    last_error = retained->last_error;
    *out_last_error_utf8 = last_error.c_str();
    return SAO_OK;
}

} // namespace sao::plugins::loader
