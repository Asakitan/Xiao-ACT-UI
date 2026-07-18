#pragma once

#include "entity_action_routes_internal.h"
#include "entity_provider_catalog_internal.h"

#include "sao/ui/entity_shell.h"

#include <cstddef>
#include <cstdint>

namespace sao::launcher::entity_provider_publication {

using SnapshotCatalogFn = entity_provider_catalog::SnapshotCatalogFn;
using InvokeProviderFn = std::int32_t(SAO_PLUGINS_CALL*)(const char* provider_id_utf8,
                                                         std::uint64_t expected_generation,
                                                         const char* action_id_utf8,
                                                         const char* payload_json_utf8);
using SetChildrenFn = sao_status_t(SAO_UI_CALL*)(sao_ui_entity_shell_handle_t handle,
                                                 const char* parent_name_utf8,
                                                 const SaoUiMenuItem* items,
                                                 std::size_t item_count);
using GetShellSnapshotFn = sao_status_t(SAO_UI_CALL*)(sao_ui_entity_shell_handle_t handle,
                                                      SaoUiEntityShellSnapshot* out_snapshot);
using HomeFn = sao_status_t(SAO_UI_CALL*)(sao_ui_entity_shell_handle_t handle);

inline constexpr std::uint32_t kRefreshIntervalMs = 250;

struct EntityProviderPublicationState {
    std::uint64_t catalog_revision = 0;
    std::uint32_t refresh_elapsed_ms = 0;
    bool has_catalog_revision = false;
};

sao_status_t refresh(sao_ui_entity_shell_handle_t shell,
                     entity_action_routes::EntityActionRouteStore& routes,
                     EntityProviderPublicationState& state, SnapshotCatalogFn snapshot_fn,
                     SetChildrenFn set_children_fn) noexcept;

sao_status_t poll(sao_ui_entity_shell_handle_t shell,
                  entity_action_routes::EntityActionRouteStore& routes,
                  EntityProviderPublicationState& state, std::uint32_t elapsed_ms,
                  SnapshotCatalogFn snapshot_fn, SetChildrenFn set_children_fn) noexcept;

sao_status_t clear(sao_ui_entity_shell_handle_t shell,
                   entity_action_routes::EntityActionRouteStore& routes,
                   EntityProviderPublicationState& state, SetChildrenFn set_children_fn) noexcept;

sao_status_t invoke(const entity_action_routes::EntityActionRoute& route,
                    sao_ui_entity_shell_handle_t shell, InvokeProviderFn invoke_fn,
                    GetShellSnapshotFn get_snapshot_fn, HomeFn home_fn) noexcept;

} // namespace sao::launcher::entity_provider_publication
