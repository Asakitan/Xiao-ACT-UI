#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"

namespace sao::plugins::loader {

typedef struct plugin_context_s plugin_context_t;

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

using entity_snapshot_callback_fn = int32_t(SAO_PLUGINS_CALL*)(entity_menu_row* rows,
                                                               uint32_t capacity,
                                                               uint32_t* out_count,
                                                               uint64_t* out_revision,
                                                               void* user_data);
using entity_action_handler_fn = int32_t(SAO_PLUGINS_CALL*)(const char* action_id_utf8,
                                                            const char* payload_json_utf8,
                                                            void* user_data);

struct native_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
};

// Adapter-neutral registration ABI. Script hosts register one provider per
// dynamic root while the canonical plugin context is loading. Ownership,
// generation, enable/disable publication and rundown remain loader-owned.
struct entity_root_contribution_descriptor {
    uint32_t struct_size;
    const char* contribution_id_utf8;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    double priority;
};

struct context_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
    const entity_root_contribution_descriptor* root_contribution;
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider(
    plugin_context_t* context, const context_entity_provider_descriptor* descriptor);

// Host-adapter teardown helper. Provider callbacks are quiesced before the
// matching context-owned registrations are removed.
int32_t plugin_context_unregister_entity_providers(plugin_context_t* context,
                                                   const char* const* provider_ids_utf8,
                                                   size_t count) noexcept;

struct entity_provider_view {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* owner_plugin_id_utf8;
    uint64_t generation;
    uint64_t revision;
    uint32_t row_count;
    const entity_menu_row* rows;
};

struct entity_root_action_ref_view {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* action_id_utf8;
};

struct entity_root_contribution_view {
    uint32_t struct_size;
    const char* owner_plugin_id_utf8;
    const char* contribution_id_utf8;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    double priority;
    uint32_t action_count;
    const entity_root_action_ref_view* actions;
};

struct entity_provider_catalog_view {
    uint32_t struct_size;
    uint64_t revision;
    uint32_t provider_count;
    const entity_provider_view* providers;
    uint32_t root_contribution_count;
    const entity_root_contribution_view* root_contributions;
};

using entity_provider_catalog_callback =
    int32_t(SAO_PLUGINS_CALL*)(const entity_provider_catalog_view* catalog, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_snapshot(entity_provider_catalog_callback callback, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_invoke(const char* provider_id_utf8, uint64_t expected_generation,
                                   const char* action_id_utf8, const char* payload_json_utf8);

} // namespace sao::plugins::loader
