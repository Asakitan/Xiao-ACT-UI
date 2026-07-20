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
using SnapshotCatalogV2Fn = entity_provider_catalog::SnapshotCatalogV2Fn;
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

enum class PythonRuntimePublicationStatus : std::uint8_t {
    not_applicable = 0,
    ready,
    degraded_unconfigured,
    degraded_unavailable,
    degraded_host_unavailable,
};

enum class PluginRuntimePublicationStatus : std::uint8_t {
    not_applicable = 0,
    ready,
    degraded_internal,
};

enum class ControlPublicationStatus : std::uint8_t {
    ready = 0,
    degraded_internal,
};

enum class TopmostPublicationStatus : std::uint8_t {
    not_applicable = 0,
    ready,
    degraded_authority_unavailable,
};

struct EntityBuiltinAuthorityState {
    bool publication_available = true;
    bool topmost = false;
    bool nervgear = false;
    bool streaming = false;
    bool save_settings = false;
    bool ai_editor = false;
    bool workshop = false;
    bool process_selector = false;
    bool plugin_manager = false;
    bool reload_plugins = false;
    bool plugin_status = false;
    bool fisheye_procedural = false;
    bool fisheye_live = false;
    bool theme = false;
    PythonRuntimePublicationStatus python_runtime = PythonRuntimePublicationStatus::not_applicable;
    PluginRuntimePublicationStatus plugin_runtime = PluginRuntimePublicationStatus::not_applicable;
    ControlPublicationStatus controls = ControlPublicationStatus::ready;
    TopmostPublicationStatus topmost_status = TopmostPublicationStatus::not_applicable;

    bool operator==(const EntityBuiltinAuthorityState&) const = default;
};

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
    sao::plugins::loader::entity_snapshot_content_token_t catalog_content_token =
        sao::plugins::loader::kInvalidEntitySnapshotContentToken;
    std::uint32_t refresh_elapsed_ms = 0;
    bool has_catalog_revision = false;
    bool has_catalog_content_token = false;
    bool topmost = false;
    bool streaming_mode = false;
    EntityBuiltinAuthorityState builtin_authority;
    entity_provider_catalog::OwnedEntityProviderCatalog published_catalog;
    std::uint64_t root_contribution_revision = 0;
    std::uint64_t published_root_contribution_revision = 0;
    std::uint64_t published_route_revision = 0;
    bool has_publication_inputs = false;
    bool published_nervgear_mode = false;
    bool published_topmost = false;
    bool published_streaming_mode = false;
    EntityBuiltinAuthorityState published_builtin_authority;
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
                     SnapshotCatalogV2Fn snapshot_v2_fn, SnapshotCatalogFn snapshot_v1_fn,
                     SetRootsFn set_roots_fn) noexcept;

inline sao_status_t refresh(sao_ui_entity_shell_handle_t shell,
                            entity_action_routes::EntityActionRouteStore& routes,
                            EntityProviderPublicationState& state, bool nervgear_mode,
                            SnapshotCatalogFn snapshot_fn, SetRootsFn set_roots_fn) noexcept {
    return refresh(shell, routes, state, nervgear_mode,
                   static_cast<SnapshotCatalogV2Fn>(nullptr), snapshot_fn, set_roots_fn);
}

sao_status_t poll(sao_ui_entity_shell_handle_t shell,
                  entity_action_routes::EntityActionRouteStore& routes,
                  EntityProviderPublicationState& state, std::uint32_t elapsed_ms,
                  bool nervgear_mode, SnapshotCatalogV2Fn snapshot_v2_fn,
                  SnapshotCatalogFn snapshot_v1_fn,
                  SetRootsFn set_roots_fn) noexcept;

inline sao_status_t poll(sao_ui_entity_shell_handle_t shell,
                         entity_action_routes::EntityActionRouteStore& routes,
                         EntityProviderPublicationState& state, std::uint32_t elapsed_ms,
                         bool nervgear_mode, SnapshotCatalogFn snapshot_fn,
                         SetRootsFn set_roots_fn) noexcept {
    return poll(shell, routes, state, elapsed_ms, nervgear_mode,
                static_cast<SnapshotCatalogV2Fn>(nullptr), snapshot_fn, set_roots_fn);
}

sao_status_t clear(sao_ui_entity_shell_handle_t shell,
                   entity_action_routes::EntityActionRouteStore& routes,
                   EntityProviderPublicationState& state, bool nervgear_mode,
                   SetRootsFn set_roots_fn) noexcept;

sao_status_t invoke(const entity_action_routes::EntityActionRoute& route,
                    sao_ui_entity_shell_handle_t shell, InvokeProviderFn invoke_fn,
                    GetShellSnapshotFn get_snapshot_fn, HomeFn home_fn) noexcept;

} // namespace sao::launcher::entity_provider_publication
