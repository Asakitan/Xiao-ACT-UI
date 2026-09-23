#include "plugin_tabs_publication_internal.h"

#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sao::launcher::plugin_tabs_publication {
namespace {

namespace Loader = sao::plugins::loader;
using Route = entity_action_routes::EntityActionRoute;
using Root = entity_provider_publication::EntityRootContributionSpec;

bool valid_text(const std::string& value) noexcept {
    return value.size() <= entity_action_routes::kMaximumStringBytes &&
           value.find('\0') == std::string::npos &&
           entity_provider_catalog::detail::valid_utf8(value);
}

std::string menu_text(const nlohmann::json& menu, const char* key) {
    const auto it = menu.find(key);
    if (it == menu.end() || !it->is_string())
        return {};
    const auto& text = it->get_ref<const std::string&>();
    return valid_text(text) ? text : std::string{};
}

struct Tab {
    std::string id;
    std::string name;
    std::string icon;
    std::string category;
    uint8_t status{SAO_UI_PLUGIN_TAB_STATUS_UNKNOWN};
    std::vector<std::string> labels;
    std::vector<std::string> icons;
    std::vector<SaoUiPluginTabAction> actions;
};

void read_metadata(const Loader::plugin_manifest& manifest,
                   const Loader::manifest_locale_resolver& localized, Tab& tab) {
    tab.id = manifest.plugin_id;
    tab.name = valid_text(manifest.name) ? manifest.name : std::string{};
    tab.name = localized.text({"name"}, tab.name, entity_action_routes::kMaximumStringBytes);
    std::string menu_name;
    if (manifest.sao_menu_json.size() <= 16 * 1024 && !manifest.sao_menu_json.empty()) {
        try {
            std::size_t nodes = 0;
            const auto menu = nlohmann::json::parse(
                manifest.sao_menu_json,
                [&nodes](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                    if (depth > 16 || ++nodes > 1024)
                        throw std::invalid_argument("menu metadata budget");
                    return true;
                });
            if (menu.is_object()) {
                tab.icon = menu_text(menu, "icon_text");
                if (tab.icon.empty())
                    tab.icon = menu_text(menu, "icon");
                tab.category = menu_text(menu, "category");
                menu_name = menu_text(menu, "name");
            }
        } catch (const nlohmann::json::exception&) {
        } catch (const std::invalid_argument&) {
        }
    }
    tab.category = localized.text({"sao_menu", "category"}, tab.category,
                                  entity_action_routes::kMaximumStringBytes);
    menu_name = localized.text({"sao_menu", "name"}, menu_name,
                               entity_action_routes::kMaximumStringBytes);
    if (!menu_name.empty())
        tab.name = std::move(menu_name);
    else if (!tab.category.empty())
        tab.name = tab.category;
    if (tab.name.empty())
        tab.name = tab.id;
}

struct Row {
    const Route* route;
    std::string group;
};

std::string bounded_text(std::string value, std::size_t maximum) {
    if (value.size() > maximum) {
        auto end = maximum;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0u) == 0x80u)
            --end;
        value.resize(end);
    }
    return value;
}

sao_status_t publish_items(sao_ui_plugin_tabs_handle_t tabs,
                          const entity_action_routes::EntityActionRouteStore& routes,
                          const entity_provider_publication::EntityProviderPublicationState& state,
                          bool loader_bound, std::string_view locale) {
    if (!loader_bound)
        return sao_ui_plugin_tabs_set_items(tabs, nullptr, 0);
    const auto registry = Loader::sao_plugins_registry_instance();
    if (registry == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;

    entity_action_routes::EntityActionRouteSnapshot snapshot;
    sao_status_t status = routes.snapshot(snapshot);
    if (status != SAO_STATUS_OK)
        return status;
    if (snapshot.revision != state.published_route_revision)
        return SAO_STATUS_ERR_CANCELLED;

    std::vector<Root> roots;
    {
        std::lock_guard lock(state.root_contribution_mutex);
        if (state.root_contribution_revision != state.published_root_contribution_revision)
            return SAO_STATUS_ERR_CANCELLED;
        roots = state.root_contributions;
    }
    for (const auto& source : state.published_catalog.root_contributions) {
        Root root;
        root.owner_id = source.owner_plugin_id;
        root.contribution_id = source.contribution_id;
        root.root_id = source.root_id;
        root.name = source.name;
        root.icon = source.icon;
        root.priority = source.priority;
        for (const auto& action : source.actions)
            root.actions.push_back({action.provider_id, action.action_id});
        roots.push_back(std::move(root));
    }
    std::sort(roots.begin(), roots.end(), [](const Root& left, const Root& right) {
        if (left.priority != right.priority)
            return left.priority < right.priority;
        if (left.owner_id != right.owner_id)
            return left.owner_id < right.owner_id;
        return left.contribution_id < right.contribution_id;
    });

    std::unordered_map<std::string, const entity_provider_catalog::OwnedEntityProvider*> providers;
    for (const auto& provider : state.published_catalog.providers)
        providers.emplace(provider.provider_id, &provider);
    std::vector<Tab> owned;
    for (const auto& manifest : Loader::snapshot_manifests(registry)) {
        const auto plugin = Loader::sao_plugins_registry_find(registry, manifest.plugin_id.c_str());
        const auto lifecycle = plugin == nullptr ? Loader::lifecycle_state::unknown
                                                : Loader::sao_plugins_lifecycle_state(plugin);
        if (lifecycle != Loader::lifecycle_state::loaded_active &&
            lifecycle != Loader::lifecycle_state::loaded_disabled)
            continue;
        if (manifest.plugin_id.empty() || !valid_text(manifest.plugin_id) ||
            manifest.plugin_id.size() >= sizeof(SaoUiPluginTabsSnapshot{}.selected_plugin_id_utf8))
            continue;
        Tab tab;
        const Loader::manifest_locale_resolver localized(manifest.locales_json, locale);
        read_metadata(manifest, localized, tab);
        const bool active = lifecycle == Loader::lifecycle_state::loaded_active;
        tab.status = active ? SAO_UI_PLUGIN_TAB_STATUS_ACTIVE : SAO_UI_PLUGIN_TAB_STATUS_DISABLED;
        std::vector<const Route*> owner_routes;
        for (const auto& route : snapshot.routes) {
            const auto provider = providers.find(route.provider_id);
            if (provider != providers.end() && provider->second->owner_plugin_id == tab.id &&
                provider->second->generation == route.provider_generation)
                owner_routes.push_back(&route);
        }
        std::stable_sort(owner_routes.begin(), owner_routes.end(), [](const Route* left,
                                                                     const Route* right) {
            if (left->category_priority != right->category_priority)
                return left->category_priority < right->category_priority;
            if (left->category_id != right->category_id)
                return left->category_id < right->category_id;
            return left->provider_id < right->provider_id;
        });
        std::vector<Row> rows;
        std::unordered_set<std::int32_t> included;
        for (const auto& root : roots) {
            if (root.owner_id != tab.id)
                continue;
            if (tab.icon.empty())
                tab.icon = root.icon;
            for (const auto& ref : root.actions) {
                const auto found = std::find_if(owner_routes.begin(), owner_routes.end(),
                                               [&](const Route* route) {
                    return route->provider_id == ref.provider_id &&
                           route->action_id == ref.action_id;
                });
                if (found != owner_routes.end() && included.insert((*found)->token).second) {
                    auto group = root.name.empty() ? root.root_id : root.name;
                    if (!(*found)->category_label.empty() && (*found)->category_label != group)
                        group += " / " + (*found)->category_label;
                    rows.push_back({*found, std::move(group)});
                }
            }
        }
        for (const auto* route : owner_routes) {
            if (included.insert(route->token).second)
                rows.push_back({route, !route->category_label.empty() ? route->category_label
                    : (!tab.category.empty() ? tab.category : route->category_id)});
            if (tab.icon.empty())
                tab.icon = route->category_icon;
        }
        std::unordered_set<std::string> groups;
        for (const auto& row : rows)
            groups.insert(row.group);
        tab.labels.reserve(rows.size());
        tab.icons.reserve(rows.size());
        tab.actions.reserve(rows.size());
        for (const auto& row : rows) {
            const auto& route = *row.route;
            const auto label = localized.text({"sao_menu", "actions", route.action_id, "label"},
                                               route.row_label,
                                               entity_action_routes::kMaximumStringBytes);
            tab.labels.push_back(bounded_text(groups.size() > 1 && !row.group.empty()
                ? row.group + " / " + label : label,
                entity_action_routes::kMaximumStringBytes));
            tab.icons.push_back(bounded_text(route.row_icon, 128));
            SaoUiPluginTabAction action{};
            action.struct_size = sizeof(action);
            action.action_id = route.token;
            action.enabled = active && route.can_activate && route.invocation_allowed();
            action.keep_open = route.keep_menu_open;
            action.close_before = route.close_menu_before;
            tab.actions.push_back(action);
        }
        tab.icon = bounded_text(std::move(tab.icon), 128);
        owned.push_back(std::move(tab));
    }
    std::sort(owned.begin(), owned.end(), [](const Tab& left, const Tab& right) {
        return left.id < right.id;
    });
    std::vector<SaoUiPluginTab> items;
    items.reserve(owned.size());
    for (auto& tab : owned) {
        for (std::size_t index = 0; index < tab.actions.size(); ++index) {
            tab.actions[index].name_utf8 = tab.labels[index].c_str();
            tab.actions[index].icon_utf8 = tab.icons[index].c_str();
        }
        SaoUiPluginTab item{};
        item.struct_size = sizeof(item);
        item.plugin_id_utf8 = tab.id.c_str();
        item.name_utf8 = tab.name.c_str();
        item.icon_utf8 = tab.icon.c_str();
        item.actions = tab.actions.empty() ? nullptr : tab.actions.data();
        item.action_count = tab.actions.size();
        item.enabled = true;
        item.status = tab.status;
        items.push_back(item);
    }
    return sao_ui_plugin_tabs_set_items(tabs, items.empty() ? nullptr : items.data(), items.size());
}

}

sao_status_t publish(sao_ui_plugin_tabs_handle_t tabs,
                     const entity_action_routes::EntityActionRouteStore& routes,
                     const entity_provider_publication::EntityProviderPublicationState& state,
                     bool loader_bound, std::string_view locale) noexcept {
    if (tabs == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    sao_status_t status = SAO_STATUS_ERR_UNKNOWN;
    try {
        status = publish_items(tabs, routes, state, loader_bound, locale);
    } catch (...) {
        status = SAO_STATUS_ERR_UNKNOWN;
    }
    if (status != SAO_STATUS_OK) {
        const auto clear_status = sao_ui_plugin_tabs_set_items(tabs, nullptr, 0);
        if (clear_status != SAO_STATUS_OK)
            (void)sao_ui_plugin_tabs_set_visible(tabs, false);
    }
    return status;
}

}
