#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"

namespace sao::plugins::loader {

struct entity_menu_row {
    uint32_t struct_size;
    const char* category_id_utf8;
    const char* category_label_utf8;
    const char* category_icon_utf8;
    double category_priority;
    const char* row_label_utf8;
    const char* row_icon_utf8;
    const char* action_id_utf8;
    const char* payload_json_utf8;
    uint8_t can_activate;
    uint8_t keep_menu_open;
    uint8_t close_menu_before;
    uint8_t reserved[5];
};

using entity_snapshot_callback_fn = int32_t(SAO_PLUGINS_CALL*)(
    entity_menu_row* rows, uint32_t capacity, uint32_t* out_count,
    uint64_t* out_revision, void* user_data);
using entity_action_handler_fn = int32_t(SAO_PLUGINS_CALL*)(
    const char* action_id_utf8, const char* payload_json_utf8,
    void* user_data);

struct native_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
};

struct entity_provider_view {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* owner_plugin_id_utf8;
    uint64_t generation;
    uint64_t revision;
    uint32_t row_count;
    const entity_menu_row* rows;
};

struct entity_provider_catalog_view {
    uint32_t struct_size;
    uint64_t revision;
    uint32_t provider_count;
    const entity_provider_view* providers;
};

using entity_provider_catalog_callback = int32_t(SAO_PLUGINS_CALL*)(
    const entity_provider_catalog_view* catalog,
    void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_snapshot(
    entity_provider_catalog_callback callback,
    void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_invoke(const char* provider_id_utf8,
                                   uint64_t expected_generation,
                                   const char* action_id_utf8,
                                   const char* payload_json_utf8);

} // namespace sao::plugins::loader
