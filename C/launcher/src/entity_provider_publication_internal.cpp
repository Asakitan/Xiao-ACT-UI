#include "entity_provider_publication_internal.h"

#include <array>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace sao::launcher::entity_provider_publication {
namespace {

constexpr std::array<SaoUiMenuItem, 2> kFixedPluginRows{{
    {"插件管理面板 Manage",
     "⚙",
     SAO_UI_ENTITY_ACTION_OPEN_PLUGIN_MANAGER,
     true,
     {false, false, false}},
    {"重载全部插件 Reload", "↻", SAO_UI_ENTITY_ACTION_RELOAD_PLUGINS, true, {false, false, false}},
}};

constexpr SaoUiMenuItem kEmptyPluginRow{
    "无已启用面板插件 (去 Manage 启用)", "·", -1, false, {false, false, false}};

sao_status_t build_menu_items(const entity_action_routes::EntityActionRouteSnapshot& snapshot,
                              std::vector<SaoUiMenuItem>& out) {
    std::vector<SaoUiMenuItem> candidate;
    candidate.reserve(kFixedPluginRows.size() +
                      (snapshot.routes.empty() ? 1 : snapshot.routes.size() * 2));
    candidate.insert(candidate.end(), kFixedPluginRows.begin(), kFixedPluginRows.end());
    if (snapshot.routes.empty()) {
        candidate.push_back(kEmptyPluginRow);
    }
    std::string_view previous_category;
    bool has_previous_category = false;
    for (const auto& route : snapshot.routes) {
        if (!has_previous_category || route.category_id != previous_category) {
            const char* label = route.category_label.empty() ? route.category_id.c_str()
                                                             : route.category_label.c_str();
            candidate.push_back({
                label,
                route.category_icon.c_str(),
                -1,
                false,
                {false, false, false},
            });
            previous_category = route.category_id;
            has_previous_category = true;
        }
        candidate.push_back({
            route.row_label.c_str(),
            route.row_icon.c_str(),
            route.token,
            route.can_activate,
            {false, false, false},
        });
    }
    out = std::move(candidate);
    return SAO_STATUS_OK;
}

sao_status_t set_route_snapshot(sao_ui_entity_shell_handle_t shell,
                                const entity_action_routes::EntityActionRouteSnapshot& snapshot,
                                bool nervgear_mode, SetRootsFn set_roots_fn) {
    std::vector<SaoUiMenuItem> plugin_rows;
    const sao_status_t status = build_menu_items(snapshot, plugin_rows);
    if (status != SAO_STATUS_OK)
        return status;

    const std::array<SaoUiMenuItem, 8> control_rows{{
        {"置顶: OFF", "⬆", SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST, true, {false, false, false}},
        {nervgear_mode ? "NervGear: ON" : "NervGear: OFF",
         "◈",
         SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR,
         true,
         {false, false, false}},
        {"──────────", "─", -1, false, {false, false, false}},
        {"Streaming Mode: OFF",
         "◈",
         SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE,
         true,
         {false, false, false}},
        {"鱼眼背景: 程序生成",
         "◆",
         SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL,
         true,
         {false, false, false}},
        {"鱼眼背景: 实时截屏",
         "◇",
         SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE,
         true,
         {false, false, false}},
        {"──────────", "─", -1, false, {false, false, false}},
        {"保存设置", "✓", SAO_UI_ENTITY_ACTION_SAVE_SETTINGS, true, {false, false, false}},
    }};
    constexpr std::array<SaoUiMenuItem, 3> kToolRows{{
        {"AI Editor (LLM)", "✦", SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR, true, {false, false, false}},
        {"Workshop", "◇", SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP, true, {false, false, false}},
        {"Process Selector",
         "⚙",
         SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR,
         true,
         {false, false, false}},
    }};
    constexpr std::array<SaoUiMenuItem, 2> kSkinRows{{
        {"全部 Light", "🎨", SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT, true, {false, false, false}},
        {"全部 Dark", "🌙", SAO_UI_ENTITY_ACTION_SET_ALL_DARK, true, {false, false, false}},
    }};
    const std::array<SaoUiEntityRootItem, 5> roots{{
        {sizeof(SaoUiEntityRootItem),
         "Control",
         "Control",
         "C",
         10,
         true,
         {0, 0, 0},
         control_rows.data(),
         control_rows.size()},
        {sizeof(SaoUiEntityRootItem),
         "Tools",
         "Tools",
         "T",
         11,
         true,
         {0, 0, 0},
         kToolRows.data(),
         kToolRows.size()},
        {sizeof(SaoUiEntityRootItem),
         "Plugins",
         "Plugins",
         "P",
         12,
         true,
         {0, 0, 0},
         plugin_rows.data(),
         plugin_rows.size()},
        {sizeof(SaoUiEntityRootItem),
         "Skins",
         "Skins",
         "S",
         13,
         true,
         {0, 0, 0},
         kSkinRows.data(),
         kSkinRows.size()},
        {sizeof(SaoUiEntityRootItem),
         "About",
         "About",
         "?",
         SAO_UI_ENTITY_ACTION_OPEN_ABOUT,
         true,
         {0, 0, 0},
         nullptr,
         0},
    }};
    return set_roots_fn(shell, roots.data(), roots.size());
}

sao_status_t resync_from_routes(sao_ui_entity_shell_handle_t shell,
                                entity_action_routes::EntityActionRouteStore& routes,
                                bool nervgear_mode, SetRootsFn set_roots_fn) {
    constexpr std::uint32_t kMaximumResyncAttempts = 3;
    for (std::uint32_t attempt = 0; attempt < kMaximumResyncAttempts; ++attempt) {
        entity_action_routes::EntityActionRouteSnapshot before;
        sao_status_t status = routes.snapshot(before);
        if (status != SAO_STATUS_OK)
            return status;
        status = set_route_snapshot(shell, before, nervgear_mode, set_roots_fn);
        if (status != SAO_STATUS_OK)
            return status;
        entity_action_routes::EntityActionRouteSnapshot after;
        status = routes.snapshot(after);
        if (status != SAO_STATUS_OK)
            return status;
        if (before == after)
            return SAO_STATUS_OK;
    }
    return SAO_STATUS_ERR_CANCELLED;
}

sao_status_t
publish_routes_transaction(sao_ui_entity_shell_handle_t shell,
                           entity_action_routes::EntityActionRouteStore& routes,
                           const std::vector<entity_action_routes::EntityActionRouteSpec>& rows,
                           bool nervgear_mode, SetRootsFn set_roots_fn) {
    entity_action_routes::EntityActionRouteStore::PreparedPublication publication;
    sao_status_t status = routes.prepare(rows, publication);
    if (status != SAO_STATUS_OK)
        return status;

    entity_action_routes::EntityActionRouteSnapshot candidate;
    status = publication.snapshot(candidate);
    if (status != SAO_STATUS_OK)
        return status;
    status = set_route_snapshot(shell, candidate, nervgear_mode, set_roots_fn);
    if (status != SAO_STATUS_OK)
        return status;

    status = publication.commit();
    if (status == SAO_STATUS_OK)
        return SAO_STATUS_OK;
    const sao_status_t resync_status =
        resync_from_routes(shell, routes, nervgear_mode, set_roots_fn);
    return resync_status == SAO_STATUS_OK ? status : resync_status;
}

sao_status_t first_failure(sao_status_t current, sao_status_t candidate) noexcept {
    return current == SAO_STATUS_OK && candidate != SAO_STATUS_OK ? candidate : current;
}

sao_status_t close_menu_if_visible(sao_ui_entity_shell_handle_t shell,
                                   GetShellSnapshotFn get_snapshot_fn, HomeFn home_fn) {
    SaoUiEntityShellSnapshot snapshot{};
    const sao_status_t snapshot_status = get_snapshot_fn(shell, &snapshot);
    if (snapshot_status != SAO_STATUS_OK || !snapshot.menu_visible)
        return snapshot_status;
    return home_fn(shell);
}

} // namespace

sao_status_t refresh(sao_ui_entity_shell_handle_t shell,
                     entity_action_routes::EntityActionRouteStore& routes,
                     EntityProviderPublicationState& state, bool nervgear_mode,
                     SnapshotCatalogFn snapshot_fn, SetRootsFn set_roots_fn) noexcept {
    if (shell == nullptr || snapshot_fn == nullptr || set_roots_fn == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        entity_provider_catalog::OwnedEntityProviderCatalog catalog;
        sao_status_t status = entity_provider_catalog::snapshot(snapshot_fn, catalog);
        if (status != SAO_STATUS_OK)
            return status;
        std::vector<entity_action_routes::EntityActionRouteSpec> rows;
        status = entity_provider_catalog::build_routes(catalog, rows);
        if (status != SAO_STATUS_OK)
            return status;
        status = publish_routes_transaction(shell, routes, rows, nervgear_mode, set_roots_fn);
        if (status != SAO_STATUS_OK)
            return status;
        state.catalog_revision = catalog.revision;
        state.has_catalog_revision = true;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t poll(sao_ui_entity_shell_handle_t shell,
                  entity_action_routes::EntityActionRouteStore& routes,
                  EntityProviderPublicationState& state, std::uint32_t elapsed_ms,
                  bool nervgear_mode, SnapshotCatalogFn snapshot_fn,
                  SetRootsFn set_roots_fn) noexcept {
    if (shell == nullptr || snapshot_fn == nullptr || set_roots_fn == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto remaining = std::numeric_limits<std::uint32_t>::max() - state.refresh_elapsed_ms;
    state.refresh_elapsed_ms += elapsed_ms > remaining ? remaining : elapsed_ms;
    if (state.refresh_elapsed_ms < kRefreshIntervalMs) {
        return SAO_STATUS_OK;
    }
    state.refresh_elapsed_ms = 0;
    return refresh(shell, routes, state, nervgear_mode, snapshot_fn, set_roots_fn);
}

sao_status_t clear(sao_ui_entity_shell_handle_t shell,
                   entity_action_routes::EntityActionRouteStore& routes,
                   EntityProviderPublicationState& state, bool nervgear_mode,
                   SetRootsFn set_roots_fn) noexcept {
    if (shell == nullptr || set_roots_fn == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        const sao_status_t status =
            publish_routes_transaction(shell, routes, {}, nervgear_mode, set_roots_fn);
        if (status != SAO_STATUS_OK)
            return status;
        state = {};
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t invoke(const entity_action_routes::EntityActionRoute& route,
                    sao_ui_entity_shell_handle_t shell, InvokeProviderFn invoke_fn,
                    GetShellSnapshotFn get_snapshot_fn, HomeFn home_fn) noexcept {
    if (invoke_fn == nullptr || route.provider_id.empty() || route.provider_generation == 0 ||
        route.action_id.empty() || !route.can_activate) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        bool should_close = !route.keep_menu_open;
        if (!route.keep_menu_open) {
            if (shell == nullptr || get_snapshot_fn == nullptr || home_fn == nullptr) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
        }
        if (should_close && route.close_menu_before) {
            const sao_status_t close_status =
                close_menu_if_visible(shell, get_snapshot_fn, home_fn);
            if (close_status != SAO_STATUS_OK)
                return close_status;
        }
        sao_status_t status = entity_provider_catalog::map_loader_status(
            invoke_fn(route.provider_id.c_str(), route.provider_generation, route.action_id.c_str(),
                      route.payload_json.c_str()));
        if (should_close && !route.close_menu_before) {
            status = first_failure(status, close_menu_if_visible(shell, get_snapshot_fn, home_fn));
        }
        return status;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

} // namespace sao::launcher::entity_provider_publication
