#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"

#include <atomic>
#include <string_view>

using namespace sao::plugins::loader;

namespace {

const char* const kCapabilities[] = {"native_test", "logging"};
std::atomic_int32_t g_on_load_status{SAO_OK};
std::atomic_int32_t g_on_unload_status{SAO_OK};

int32_t SAO_PLUGINS_CALL entity_snapshot(
    entity_menu_row* rows,
    uint32_t capacity,
    uint32_t* out_count,
    uint64_t* out_revision,
    void*) {
    if (out_count == nullptr || out_revision == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_count = 1;
    *out_revision = 7;
    if (rows == nullptr || capacity < 1) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    rows[0] = {
        sizeof(entity_menu_row),
        "fixture-category",
        "Fixture Category",
        "fixture-category-icon",
        5.0,
        "Fixture Action",
        "fixture-row-icon",
        "fixture.action",
        R"({"source":"fixture"})",
        true,
        false,
        false,
    };
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL entity_action(
    const char* action_id_utf8,
    const char* payload_json_utf8,
    void*) {
    return action_id_utf8 != nullptr && payload_json_utf8 != nullptr &&
                   std::string_view(action_id_utf8) == "fixture.action"
               ? SAO_OK
               : SAO_ERR_INVALID_ARGUMENT;
}

const native_entity_provider_descriptor kEntityProviders[] = {{
    sizeof(native_entity_provider_descriptor),
    "fixture",
    entity_snapshot,
    entity_action,
    nullptr,
}};

} // namespace

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_lifecycle_statuses(int32_t on_load_status,
                                       int32_t on_unload_status) {
    g_on_load_status.store(on_load_status);
    g_on_unload_status.store(on_unload_status);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_plugin_query_descriptor(native_plugin_descriptor* out_descriptor) {
    if (out_descriptor == nullptr || out_descriptor->struct_size < sizeof(*out_descriptor)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    out_descriptor->abi_version = 2;
    out_descriptor->plugin_version = "1.0.0";
    out_descriptor->capability_count = 2;
    out_descriptor->capabilities = kCapabilities;
    out_descriptor->entity_provider_count = 1;
    out_descriptor->entity_providers = kEntityProviders;
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_plugin_on_load(plugin_context_t* context) {
    return context != nullptr ? g_on_load_status.load()
                              : SAO_ERR_INVALID_ARGUMENT;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_enable() {
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_disable() {
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_unload() {
    return g_on_unload_status.load();
}
