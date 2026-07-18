#pragma once

#include "entity_action_routes_internal.h"
#include "entity_provider_catalog_internal.h"

#include "sao/ui/entity_shell.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sao::launcher::entity_provider_publication {

using SnapshotCatalogFn = entity_provider_catalog::SnapshotCatalogFn;
using InvokeProviderFn = std::int32_t(SAO_PLUGINS_CALL*)(const char* provider_id_utf8,
                                                         std::uint64_t expected_generation,
                                                         const char* action_id_utf8,
                                                         const char* payload_json_utf8);
using SetRootsFn = sao_status_t(SAO_UI_CALL*)(sao_ui_entity_shell_handle_t handle,
                                              const SaoUiEntityRootItem* roots,
                                              std::size_t root_count);
using GetShellSnapshotFn = sao_status_t(SAO_UI_CALL*)(sao_ui_entity_shell_handle_t handle,
                                                      SaoUiEntityShellSnapshot* out_snapshot);
using HomeFn = sao_status_t(SAO_UI_CALL*)(sao_ui_entity_shell_handle_t handle);

inline constexpr std::uint32_t kRefreshIntervalMs = 250;

struct EntityRootContributionActionRef {
    std::string provider_id;
    std::string action_id;

    bool operator==(const EntityRootContributionActionRef&) const = default;
};

struct EntityRootContributionSpec {
    std::string owner_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::vector<EntityRootContributionActionRef> actions;

    bool operator==(const EntityRootContributionSpec&) const = default;
};

struct EntityProviderPublicationState {
    std::uint64_t catalog_revision = 0;
    std::uint32_t refresh_elapsed_ms = 0;
    bool has_catalog_revision = false;
    std::uint64_t root_contribution_revision = 0;
    std::uint64_t published_root_contribution_revision = 0;
    std::vector<EntityRootContributionSpec> root_contributions;
    mutable std::mutex root_contribution_mutex;
};

sao_status_t
replace_root_contributions(EntityProviderPublicationState& state,
                           const std::vector<EntityRootContributionSpec>& contributions) noexcept;

sao_status_t replace_root_contributions_for_owner(
    EntityProviderPublicationState& state, const std::string& owner_id,
    const std::vector<EntityRootContributionSpec>& contributions) noexcept;

sao_status_t clear_root_contributions_for_owner(EntityProviderPublicationState& state,
                                                const std::string& owner_id) noexcept;

sao_status_t refresh(sao_ui_entity_shell_handle_t shell,
                     entity_action_routes::EntityActionRouteStore& routes,
                     EntityProviderPublicationState& state, bool nervgear_mode,
                     SnapshotCatalogFn snapshot_fn, SetRootsFn set_roots_fn) noexcept;

sao_status_t poll(sao_ui_entity_shell_handle_t shell,
                  entity_action_routes::EntityActionRouteStore& routes,
                  EntityProviderPublicationState& state, std::uint32_t elapsed_ms,
                  bool nervgear_mode, SnapshotCatalogFn snapshot_fn,
                  SetRootsFn set_roots_fn) noexcept;

sao_status_t clear(sao_ui_entity_shell_handle_t shell,
                   entity_action_routes::EntityActionRouteStore& routes,
                   EntityProviderPublicationState& state, bool nervgear_mode,
                   SetRootsFn set_roots_fn) noexcept;

sao_status_t invoke(const entity_action_routes::EntityActionRoute& route,
                    sao_ui_entity_shell_handle_t shell, InvokeProviderFn invoke_fn,
                    GetShellSnapshotFn get_snapshot_fn, HomeFn home_fn) noexcept;

} // namespace sao::launcher::entity_provider_publication
