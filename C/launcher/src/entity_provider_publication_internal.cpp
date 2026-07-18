#include "entity_provider_publication_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>
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

constexpr std::size_t kBuiltinRootCount = 5;
constexpr std::size_t kMaximumContributionRootCount =
    SAO_UI_ENTITY_ROOT_MAX_COUNT - kBuiltinRootCount;
constexpr std::size_t kMaximumActionsPerRoot = 256;
constexpr std::size_t kMaximumContributionActions = 1024;
constexpr std::size_t kMaximumTreeUtf8Bytes = 64U * 1024U;
constexpr std::size_t kMaximumRootIdBytes = SAO_UI_ENTITY_ROOT_ID_CAPACITY - 1U;
constexpr std::size_t kMaximumLabelBytes = 4096;
constexpr std::size_t kMaximumIconBytes = 256;
constexpr std::size_t kMaximumIdentityBytes = 1024;

bool valid_utf8(std::string_view value) noexcept {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7f) {
            ++offset;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1;
            code_point = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2;
            code_point = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3;
            code_point = first & 0x07u;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            code_point = (code_point << 6u) | (next & 0x3fu);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80u) ||
                              (continuation_count == 2 && code_point < 0x800u) ||
                              (continuation_count == 3 && code_point < 0x10000u);
        if (overlong || code_point > 0x10ffffu ||
            (code_point >= 0xd800u && code_point <= 0xdfffu)) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

bool valid_string(std::string_view value, std::size_t maximum_bytes, bool required) noexcept {
    return (!required || !value.empty()) && value.size() <= maximum_bytes &&
           value.find('\0') == std::string_view::npos && valid_utf8(value);
}

sao_status_t normalize_root_contributions(const std::vector<EntityRootContributionSpec>& input,
                                          std::vector<EntityRootContributionSpec>& output) {
    if (input.size() > kMaximumContributionRootCount) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (std::any_of(input.begin(), input.end(), [](const auto& contribution) {
            return !std::isfinite(contribution.priority);
        })) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::vector<EntityRootContributionSpec> candidate = input;
    std::sort(candidate.begin(), candidate.end(), [](const auto& left, const auto& right) {
        if (left.priority != right.priority) {
            return left.priority < right.priority;
        }
        if (left.owner_id != right.owner_id) {
            return left.owner_id < right.owner_id;
        }
        if (left.contribution_id != right.contribution_id) {
            return left.contribution_id < right.contribution_id;
        }
        return left.root_id < right.root_id;
    });

    std::set<std::pair<std::string, std::string>> contribution_ids;
    std::set<std::string> root_ids{"Control", "Tools", "Plugins", "Skins", "About"};
    std::set<std::string> root_names{"Control", "Tools", "Plugins", "Skins", "About"};
    std::set<std::pair<std::string, std::string>> action_ids;
    std::size_t total_actions = 0;
    for (const auto& contribution : candidate) {
        if (!valid_string(contribution.owner_id, kMaximumIdentityBytes, true) ||
            !valid_string(contribution.contribution_id, kMaximumIdentityBytes, true) ||
            !valid_string(contribution.root_id, kMaximumRootIdBytes, true) ||
            !valid_string(contribution.name, kMaximumLabelBytes, true) ||
            !valid_string(contribution.icon, kMaximumIconBytes, false) ||
            contribution.actions.size() > kMaximumActionsPerRoot ||
            contribution.actions.size() > kMaximumContributionActions - total_actions) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (!contribution_ids.emplace(contribution.owner_id, contribution.contribution_id).second ||
            !root_ids.emplace(contribution.root_id).second ||
            !root_names.emplace(contribution.name).second) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        for (const auto& action : contribution.actions) {
            if (!valid_string(action.provider_id, kMaximumIdentityBytes, true) ||
                !valid_string(action.action_id, kMaximumIdentityBytes, true)) {
                return SAO_STATUS_ERR_INVALID_ARGUMENT;
            }
            if (!action_ids.emplace(action.provider_id, action.action_id).second) {
                return SAO_STATUS_ERR_ALREADY_EXISTS;
            }
        }
        total_actions += contribution.actions.size();
    }
    output = std::move(candidate);
    return SAO_STATUS_OK;
}

const entity_action_routes::EntityActionRoute*
find_route(const entity_action_routes::EntityActionRouteSnapshot& snapshot,
           const EntityRootContributionActionRef& action) noexcept {
    const auto found =
        std::find_if(snapshot.routes.begin(), snapshot.routes.end(), [&action](const auto& route) {
            return route.provider_id == action.provider_id && route.action_id == action.action_id;
        });
    return found == snapshot.routes.end() ? nullptr : &*found;
}

sao_status_t measure_tree_string(const char* value, std::size_t maximum_bytes, bool required,
                                 std::size_t& total_bytes) noexcept {
    if (value == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::string_view text(value);
    if (!valid_string(text, maximum_bytes, required) ||
        text.size() > kMaximumTreeUtf8Bytes - total_bytes) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    total_bytes += text.size();
    return SAO_STATUS_OK;
}

sao_status_t validate_materialized_tree(const std::vector<SaoUiEntityRootItem>& roots) noexcept {
    if (roots.size() > SAO_UI_ENTITY_ROOT_MAX_COUNT) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::set<std::string_view> root_ids;
    std::set<std::string_view> root_names;
    std::size_t total_children = 0;
    std::size_t total_bytes = 0;
    for (const auto& root : roots) {
        if (root.struct_size != sizeof(SaoUiEntityRootItem) ||
            (root.children == nullptr && root.child_count != 0) ||
            root.child_count > kMaximumActionsPerRoot ||
            root.child_count > kMaximumContributionActions - total_children) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        sao_status_t status =
            measure_tree_string(root.root_id_utf8, kMaximumRootIdBytes, true, total_bytes);
        if (status == SAO_STATUS_OK) {
            status = measure_tree_string(root.name_utf8, kMaximumLabelBytes, true, total_bytes);
        }
        if (status == SAO_STATUS_OK) {
            status = measure_tree_string(root.icon_utf8, kMaximumIconBytes, false, total_bytes);
        }
        if (status != SAO_STATUS_OK) {
            return status;
        }
        if (!root_ids.emplace(root.root_id_utf8).second ||
            !root_names.emplace(root.name_utf8).second) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        for (std::size_t index = 0; index < root.child_count; ++index) {
            const auto& child = root.children[index];
            status = measure_tree_string(child.name_utf8, kMaximumLabelBytes, true, total_bytes);
            if (status == SAO_STATUS_OK) {
                status =
                    measure_tree_string(child.icon_utf8, kMaximumIconBytes, false, total_bytes);
            }
            if (status != SAO_STATUS_OK) {
                return status;
            }
        }
        total_children += root.child_count;
    }
    return SAO_STATUS_OK;
}

sao_status_t
build_menu_items(const entity_action_routes::EntityActionRouteSnapshot& snapshot,
                 const std::set<std::pair<std::string, std::string>>& contributed_actions,
                 std::vector<SaoUiMenuItem>& out) {
    std::vector<SaoUiMenuItem> candidate;
    candidate.reserve(kFixedPluginRows.size() + snapshot.routes.size() * 2 + 1);
    candidate.insert(candidate.end(), kFixedPluginRows.begin(), kFixedPluginRows.end());
    std::string_view previous_category;
    bool has_previous_category = false;
    for (const auto& route : snapshot.routes) {
        if (contributed_actions.contains({route.provider_id, route.action_id})) {
            continue;
        }
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
    if (candidate.size() == kFixedPluginRows.size()) {
        candidate.push_back(kEmptyPluginRow);
    }
    out = std::move(candidate);
    return SAO_STATUS_OK;
}

sao_status_t set_route_snapshot(sao_ui_entity_shell_handle_t shell,
                                const entity_action_routes::EntityActionRouteSnapshot& snapshot,
                                const std::vector<EntityRootContributionSpec>& contributions,
                                bool nervgear_mode, SetRootsFn set_roots_fn) {
    std::set<std::pair<std::string, std::string>> contributed_actions;
    std::vector<std::vector<SaoUiMenuItem>> contribution_rows;
    contribution_rows.reserve(contributions.size());
    for (const auto& contribution : contributions) {
        std::vector<SaoUiMenuItem> rows;
        rows.reserve(contribution.actions.size());
        for (const auto& action : contribution.actions) {
            const auto* route = find_route(snapshot, action);
            if (route == nullptr) {
                return SAO_STATUS_ERR_NOT_FOUND;
            }
            contributed_actions.emplace(action.provider_id, action.action_id);
            rows.push_back({
                route->row_label.c_str(),
                route->row_icon.c_str(),
                route->token,
                route->can_activate,
                {false, false, false},
            });
        }
        contribution_rows.push_back(std::move(rows));
    }

    std::vector<SaoUiMenuItem> plugin_rows;
    const sao_status_t status = build_menu_items(snapshot, contributed_actions, plugin_rows);
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
    std::vector<SaoUiEntityRootItem> roots{
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
    };
    roots.reserve(kBuiltinRootCount + contributions.size());
    for (std::size_t index = 0; index < contributions.size(); ++index) {
        const auto& contribution = contributions[index];
        const auto& children = contribution_rows[index];
        roots.push_back({
            sizeof(SaoUiEntityRootItem),
            contribution.root_id.c_str(),
            contribution.name.c_str(),
            contribution.icon.c_str(),
            -1,
            false,
            {0, 0, 0},
            children.empty() ? nullptr : children.data(),
            children.size(),
        });
    }
    const sao_status_t validation_status = validate_materialized_tree(roots);
    if (validation_status != SAO_STATUS_OK) {
        return validation_status;
    }
    return set_roots_fn(shell, roots.data(), roots.size());
}

sao_status_t resync_from_routes(sao_ui_entity_shell_handle_t shell,
                                entity_action_routes::EntityActionRouteStore& routes,
                                const std::vector<EntityRootContributionSpec>& contributions,
                                bool nervgear_mode, SetRootsFn set_roots_fn) {
    constexpr std::uint32_t kMaximumResyncAttempts = 3;
    for (std::uint32_t attempt = 0; attempt < kMaximumResyncAttempts; ++attempt) {
        entity_action_routes::EntityActionRouteSnapshot before;
        sao_status_t status = routes.snapshot(before);
        if (status != SAO_STATUS_OK)
            return status;
        status = set_route_snapshot(shell, before, contributions, nervgear_mode, set_roots_fn);
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
                           const std::vector<EntityRootContributionSpec>& contributions,
                           bool nervgear_mode, SetRootsFn set_roots_fn) {
    entity_action_routes::EntityActionRouteStore::PreparedPublication publication;
    sao_status_t status = routes.prepare(rows, publication);
    if (status != SAO_STATUS_OK)
        return status;

    entity_action_routes::EntityActionRouteSnapshot candidate;
    status = publication.snapshot(candidate);
    if (status != SAO_STATUS_OK)
        return status;
    status = set_route_snapshot(shell, candidate, contributions, nervgear_mode, set_roots_fn);
    if (status != SAO_STATUS_OK)
        return status;

    status = publication.commit();
    if (status == SAO_STATUS_OK)
        return SAO_STATUS_OK;
    const sao_status_t resync_status =
        resync_from_routes(shell, routes, contributions, nervgear_mode, set_roots_fn);
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

sao_status_t
replace_root_contributions(EntityProviderPublicationState& state,
                           const std::vector<EntityRootContributionSpec>& contributions) noexcept {
    try {
        std::vector<EntityRootContributionSpec> candidate;
        const sao_status_t status = normalize_root_contributions(contributions, candidate);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        std::lock_guard lock(state.root_contribution_mutex);
        if (candidate == state.root_contributions) {
            return SAO_STATUS_OK;
        }
        if (state.root_contribution_revision == std::numeric_limits<std::uint64_t>::max()) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        state.root_contributions = std::move(candidate);
        ++state.root_contribution_revision;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t replace_root_contributions_for_owner(
    EntityProviderPublicationState& state, const std::string& owner_id,
    const std::vector<EntityRootContributionSpec>& contributions) noexcept {
    if (!valid_string(owner_id, kMaximumIdentityBytes, true) ||
        std::any_of(
            contributions.begin(), contributions.end(),
            [&owner_id](const auto& contribution) { return contribution.owner_id != owner_id; })) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(state.root_contribution_mutex);
        std::vector<EntityRootContributionSpec> merged;
        merged.reserve(state.root_contributions.size() + contributions.size());
        std::copy_if(state.root_contributions.begin(), state.root_contributions.end(),
                     std::back_inserter(merged), [&owner_id](const auto& contribution) {
                         return contribution.owner_id != owner_id;
                     });
        merged.insert(merged.end(), contributions.begin(), contributions.end());
        std::vector<EntityRootContributionSpec> candidate;
        const sao_status_t status = normalize_root_contributions(merged, candidate);
        if (status != SAO_STATUS_OK) {
            return status;
        }
        if (candidate == state.root_contributions) {
            return SAO_STATUS_OK;
        }
        if (state.root_contribution_revision == std::numeric_limits<std::uint64_t>::max()) {
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        }
        state.root_contributions = std::move(candidate);
        ++state.root_contribution_revision;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t clear_root_contributions_for_owner(EntityProviderPublicationState& state,
                                                const std::string& owner_id) noexcept {
    return replace_root_contributions_for_owner(state, owner_id, {});
}

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
        std::vector<EntityRootContributionSpec> contributions;
        std::uint64_t contribution_revision = 0;
        {
            std::lock_guard lock(state.root_contribution_mutex);
            contributions = state.root_contributions;
            contribution_revision = state.root_contribution_revision;
        }
        status = publish_routes_transaction(shell, routes, rows, contributions, nervgear_mode,
                                            set_roots_fn);
        if (status != SAO_STATUS_OK)
            return status;
        state.catalog_revision = catalog.revision;
        state.has_catalog_revision = true;
        {
            std::lock_guard lock(state.root_contribution_mutex);
            if (state.root_contribution_revision == contribution_revision) {
                state.published_root_contribution_revision = contribution_revision;
            }
        }
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
            publish_routes_transaction(shell, routes, {}, {}, nervgear_mode, set_roots_fn);
        if (status != SAO_STATUS_OK)
            return status;
        state.catalog_revision = 0;
        state.refresh_elapsed_ms = 0;
        state.has_catalog_revision = false;
        {
            std::lock_guard lock(state.root_contribution_mutex);
            state.published_root_contribution_revision = 0;
        }
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
