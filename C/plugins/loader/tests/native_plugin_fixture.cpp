#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"

using namespace sao::plugins::loader;

namespace {

const char* const kCapabilities[] = {"native_test", "logging"};

} // namespace

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_plugin_query_descriptor(native_plugin_descriptor* out_descriptor) {
    if (out_descriptor == nullptr || out_descriptor->struct_size < sizeof(*out_descriptor)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    out_descriptor->abi_version = 2;
    out_descriptor->plugin_version = "1.0.0";
    out_descriptor->capability_count = 2;
    out_descriptor->capabilities = kCapabilities;
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_plugin_on_load(plugin_context_t* context) {
    return context != nullptr ? SAO_OK : SAO_ERR_INVALID_ARGUMENT;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_enable() {
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_disable() {
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_unload() {
    return SAO_OK;
}
