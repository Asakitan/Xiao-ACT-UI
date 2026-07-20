#include <catch2/catch_test_macros.hpp>

#include "entity_action_routes_internal.h"
#include "entity_provider_catalog_internal.h"
#include "entity_provider_publication_internal.h"
#include "tool_launch_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using EntityActionRoute = sao::launcher::entity_action_routes::EntityActionRoute;
using EntityActionRouteSnapshot = sao::launcher::entity_action_routes::EntityActionRouteSnapshot;
using EntityActionRouteSpec = sao::launcher::entity_action_routes::EntityActionRouteSpec;
using EntityActionRouteStore = sao::launcher::entity_action_routes::EntityActionRouteStore;
using PreparedEntityActionRoutePublication =
    sao::launcher::entity_action_routes::EntityActionRouteStore::PreparedPublication;
using OwnedEntityProviderCatalog =
    sao::launcher::entity_provider_catalog::OwnedEntityProviderCatalog;
using EntityProviderPublicationState =
    sao::launcher::entity_provider_publication::EntityProviderPublicationState;
using ControlPublicationStatus =
    sao::launcher::entity_provider_publication::ControlPublicationStatus;
using PluginRuntimePublicationStatus =
    sao::launcher::entity_provider_publication::PluginRuntimePublicationStatus;
using EntityRootContributionActionRef =
    sao::launcher::entity_provider_publication::EntityRootContributionActionRef;
using EntityRootContributionSpec =
    sao::launcher::entity_provider_publication::EntityRootContributionSpec;

namespace loader = sao::plugins::loader;

EntityActionRouteSpec make_route(std::string provider_id, std::string action_id,
                                 std::string category_id = "category",
                                 std::string row_label = "Row") {
    EntityActionRouteSpec route;
    route.provider_id = std::move(provider_id);
    route.category_id = std::move(category_id);
    route.category_label = "Category";
    route.category_icon = "category-icon";
    route.row_label = std::move(row_label);
    route.row_icon = "row-icon";
    route.action_id = std::move(action_id);
    route.can_activate = true;
    route.keep_menu_open = false;
    route.close_menu_before = true;
    return route;
}

EntityActionRouteSnapshot snapshot(const EntityActionRouteStore& store) {
    EntityActionRouteSnapshot value;
    REQUIRE(store.snapshot(value) == SAO_STATUS_OK);
    return value;
}

std::int32_t token_for(const EntityActionRouteSnapshot& value, const std::string& provider_id,
                       const std::string& action_id) {
    const auto found =
        std::find_if(value.routes.begin(), value.routes.end(), [&](const auto& route) {
            return route.provider_id == provider_id && route.action_id == action_id;
        });
    REQUIRE(found != value.routes.end());
    return found->token;
}

void check_rollback(const EntityActionRouteStore& store,
                    const EntityActionRouteSnapshot& expected) {
    const auto actual = snapshot(store);
    CHECK(actual == expected);
    for (const auto& route : expected.routes) {
        EntityActionRoute resolved;
        REQUIRE(store.resolve(route.token, resolved) == SAO_STATUS_OK);
        CHECK(resolved == route);
    }
}

struct CatalogFixture {
    std::vector<std::string> provider_ids;
    std::vector<std::string> owner_ids;
    std::vector<std::string> category_ids;
    std::vector<std::string> category_labels;
    std::vector<std::string> category_icons;
    std::vector<std::string> row_labels;
    std::vector<std::string> row_icons;
    std::vector<std::string> action_ids;
    std::vector<std::string> payloads;
    std::vector<loader::entity_menu_row> rows;
    std::vector<loader::entity_provider_view> providers;
    std::string root_owner_id;
    std::string root_contribution_id;
    std::string root_id;
    std::string root_name;
    std::string root_icon;
    loader::entity_root_action_ref_view root_action{};
    loader::entity_root_contribution_view root{};
    bool has_root = false;
    loader::entity_provider_catalog_view catalog{};

    void reset(std::size_t count) {
        provider_ids.resize(count);
        owner_ids.resize(count);
        category_ids.resize(count);
        category_labels.resize(count);
        category_icons.resize(count);
        row_labels.resize(count);
        row_icons.resize(count);
        action_ids.resize(count);
        payloads.resize(count);
        rows.resize(count);
        providers.resize(count);
        has_root = false;
    }

    void set_root(std::string owner_id, std::string contribution_id, std::string id,
                  std::string name, std::string icon, double priority, std::size_t provider_index) {
        root_owner_id = std::move(owner_id);
        root_contribution_id = std::move(contribution_id);
        root_id = std::move(id);
        root_name = std::move(name);
        root_icon = std::move(icon);
        root_action = {
            sizeof(loader::entity_root_action_ref_view),
            provider_ids[provider_index].c_str(),
            action_ids[provider_index].c_str(),
        };
        root = {
            sizeof(loader::entity_root_contribution_view),
            root_owner_id.c_str(),
            root_contribution_id.c_str(),
            root_id.c_str(),
            root_name.c_str(),
            root_icon.c_str(),
            priority,
            1,
            &root_action,
        };
        has_root = true;
    }

    void set_row(std::size_t index, std::string provider_id, std::uint64_t generation,
                 std::string category_id, std::string category_label, std::string category_icon,
                 double category_priority, std::string row_label, std::string row_icon,
                 std::string action_id, std::string payload) {
        provider_ids[index] = std::move(provider_id);
        owner_ids[index] = "owner-" + std::to_string(index);
        category_ids[index] = std::move(category_id);
        category_labels[index] = std::move(category_label);
        category_icons[index] = std::move(category_icon);
        row_labels[index] = std::move(row_label);
        row_icons[index] = std::move(row_icon);
        action_ids[index] = std::move(action_id);
        payloads[index] = std::move(payload);
        rows[index] = {
            sizeof(loader::entity_menu_row),
            category_ids[index].c_str(),
            category_labels[index].c_str(),
            category_icons[index].c_str(),
            category_priority,
            row_labels[index].c_str(),
            row_icons[index].c_str(),
            action_ids[index].c_str(),
            payloads[index].c_str(),
            1,
            0,
            0,
            {},
        };
        providers[index] = {
            sizeof(loader::entity_provider_view),
            provider_ids[index].c_str(),
            owner_ids[index].c_str(),
            generation,
            10 + index,
            1,
            &rows[index],
        };
    }

    void finish(std::uint64_t revision = 1) {
        catalog = {
            sizeof(loader::entity_provider_catalog_view),
            revision,
            static_cast<std::uint32_t>(providers.size()),
            providers.empty() ? nullptr : providers.data(),
            has_root ? 1U : 0U,
            has_root ? &root : nullptr,
        };
    }
};

CatalogFixture* g_catalog_fixture = nullptr;

std::int32_t SAO_PLUGINS_CALL
fake_catalog_snapshot(loader::entity_provider_catalog_callback callback, void* user_data) {
    if (g_catalog_fixture == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    return callback(&g_catalog_fixture->catalog, user_data);
}

OwnedEntityProviderCatalog make_catalog_sentinel() {
    OwnedEntityProviderCatalog catalog;
    catalog.revision = 9001;
    catalog.providers.resize(1);
    catalog.providers[0].provider_id = "sentinel-provider";
    catalog.providers[0].owner_plugin_id = "sentinel-owner";
    catalog.providers[0].generation = 77;
    catalog.providers[0].revision = 88;
    catalog.root_contributions.resize(1);
    catalog.root_contributions[0].owner_plugin_id = "sentinel-owner";
    catalog.root_contributions[0].contribution_id = "sentinel-contribution";
    catalog.root_contributions[0].root_id = "sentinel-root";
    catalog.root_contributions[0].name = "Sentinel Root";
    catalog.root_contributions[0].actions.resize(1);
    catalog.root_contributions[0].actions[0].provider_id = "sentinel-provider";
    catalog.root_contributions[0].actions[0].action_id = "sentinel-action";
    return catalog;
}

void check_catalog_snapshot_rejected_transactionally(CatalogFixture& fixture) {
    g_catalog_fixture = &fixture;
    auto catalog = make_catalog_sentinel();
    const auto before = catalog;
    CHECK(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(catalog == before);
    g_catalog_fixture = nullptr;
}

struct PublishedMenuRow {
    std::string name;
    std::string icon;
    std::int32_t action_id = 0;
    bool can_activate = false;
};

struct PublishedRoot {
    std::size_t struct_size = 0;
    std::string id;
    std::string name;
    std::string icon;
    std::int32_t action_id = 0;
    bool can_activate = false;
    std::vector<PublishedMenuRow> children;
};

struct PublicationLog {
    EntityActionRouteStore* competing_store = nullptr;
    EntityActionRouteSpec competing_route;
    std::vector<std::pair<std::size_t, EntityActionRouteSpec>> competing_publications;
    bool publish_competitor = false;
    std::int32_t route_probe_token = 0;
    std::vector<sao_status_t> route_probe_statuses;
    sao_status_t publication_status = SAO_STATUS_OK;
    std::vector<sao_status_t> publication_statuses;
    std::size_t publication_attempt = 0;
    std::vector<std::vector<PublishedRoot>> calls;
};

PublicationLog* g_publication_log = nullptr;

sao_status_t SAO_UI_CALL fake_set_roots(sao_ui_entity_shell_handle_t,
                                        const SaoUiEntityRootItem* roots, std::size_t count) {
    if (g_publication_log == nullptr || (count != 0 && roots == nullptr)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::vector<PublishedRoot> copied;
    copied.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (roots[index].struct_size != sizeof(SaoUiEntityRootItem)) {
            return SAO_STATUS_ERR_ABI_MISMATCH;
        }
        if (roots[index].child_count != 0 && roots[index].children == nullptr) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        PublishedRoot root;
        root.struct_size = roots[index].struct_size;
        root.id = roots[index].root_id_utf8 == nullptr ? "" : roots[index].root_id_utf8;
        root.name = roots[index].name_utf8 == nullptr ? "" : roots[index].name_utf8;
        root.icon = roots[index].icon_utf8 == nullptr ? "" : roots[index].icon_utf8;
        root.action_id = roots[index].action_id;
        root.can_activate = roots[index].can_activate;
        root.children.reserve(roots[index].child_count);
        for (std::size_t child = 0; child < roots[index].child_count; ++child) {
            root.children.push_back({
                roots[index].children[child].name_utf8 == nullptr
                    ? ""
                    : roots[index].children[child].name_utf8,
                roots[index].children[child].icon_utf8 == nullptr
                    ? ""
                    : roots[index].children[child].icon_utf8,
                roots[index].children[child].action_id,
                roots[index].children[child].can_activate,
            });
        }
        copied.push_back(std::move(root));
    }
    g_publication_log->calls.push_back(std::move(copied));
    if (g_publication_log->competing_store != nullptr &&
        g_publication_log->route_probe_token != 0) {
        EntityActionRoute route;
        g_publication_log->route_probe_statuses.push_back(
            g_publication_log->competing_store->resolve(g_publication_log->route_probe_token,
                                                        route));
    }
    const sao_status_t publication_status =
        g_publication_log->publication_attempt < g_publication_log->publication_statuses.size()
            ? g_publication_log->publication_statuses[g_publication_log->publication_attempt++]
            : g_publication_log->publication_status;
    if (publication_status != SAO_STATUS_OK) {
        return publication_status;
    }
    const auto competing_publication =
        std::find_if(g_publication_log->competing_publications.begin(),
                     g_publication_log->competing_publications.end(), [&](const auto& item) {
                         return item.first == g_publication_log->calls.size();
                     });
    if (competing_publication != g_publication_log->competing_publications.end() &&
        g_publication_log->competing_store != nullptr) {
        REQUIRE(g_publication_log->competing_store->publish({competing_publication->second}) ==
                SAO_STATUS_OK);
    }
    if (g_publication_log->publish_competitor && g_publication_log->calls.size() == 1 &&
        g_publication_log->competing_store != nullptr) {
        REQUIRE(g_publication_log->competing_store->publish({g_publication_log->competing_route}) ==
                SAO_STATUS_OK);
    }
    return SAO_STATUS_OK;
}

const PublishedRoot& published_root(const std::vector<PublishedRoot>& roots, std::string_view id) {
    const auto found =
        std::find_if(roots.begin(), roots.end(), [&](const auto& root) { return root.id == id; });
    REQUIRE(found != roots.end());
    return *found;
}

bool all_actions_disabled(const std::vector<PublishedRoot>& roots) {
    return std::all_of(roots.begin(), roots.end(), [](const auto& root) {
        return !root.can_activate &&
               std::all_of(root.children.begin(), root.children.end(),
                           [](const auto& child) { return !child.can_activate; });
    });
}

struct ActionLog {
    bool menu_visible = true;
    sao_status_t snapshot_status = SAO_STATUS_OK;
    sao_status_t home_status = SAO_STATUS_OK;
    std::int32_t invoke_status = SAO_OK;
    std::string provider_id;
    std::uint64_t generation = 0;
    std::string action_id;
    std::string payload;
    std::vector<std::string> order;
};

ActionLog* g_action_log = nullptr;

sao_status_t SAO_UI_CALL fake_get_shell_snapshot(sao_ui_entity_shell_handle_t,
                                                 SaoUiEntityShellSnapshot* out) {
    if (g_action_log == nullptr || out == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    g_action_log->order.push_back("snapshot");
    if (g_action_log->snapshot_status == SAO_STATUS_OK) {
        *out = {};
        out->menu_visible = g_action_log->menu_visible;
    }
    return g_action_log->snapshot_status;
}

sao_status_t SAO_UI_CALL fake_home(sao_ui_entity_shell_handle_t) {
    if (g_action_log == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    g_action_log->order.push_back("home");
    return g_action_log->home_status;
}

std::int32_t SAO_PLUGINS_CALL fake_invoke(const char* provider_id, std::uint64_t generation,
                                          const char* action_id, const char* payload) {
    if (g_action_log == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    g_action_log->order.push_back("invoke");
    g_action_log->provider_id = provider_id;
    g_action_log->generation = generation;
    g_action_log->action_id = action_id;
    g_action_log->payload = payload;
    return g_action_log->invoke_status;
}

struct BlockingActionProbe {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    std::atomic_int calls{0};
};

BlockingActionProbe* g_blocking_action_probe = nullptr;

std::int32_t SAO_PLUGINS_CALL blocking_invoke(const char*, std::uint64_t, const char*,
                                              const char*) {
    auto* probe = g_blocking_action_probe;
    if (probe == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    probe->calls.fetch_add(1, std::memory_order_acq_rel);
    std::unique_lock lock(probe->mutex);
    probe->entered = true;
    probe->condition.notify_all();
    probe->condition.wait(lock, [probe] { return probe->release; });
    return SAO_OK;
}

enum class ReentryOperation : std::uint8_t {
    invoke,
    clear,
};

struct ReentryActionProbe {
    const EntityActionRoute* route = nullptr;
    EntityActionRouteStore* store = nullptr;
    EntityProviderPublicationState* state = nullptr;
    sao_ui_entity_shell_handle_t shell = nullptr;
    ReentryOperation operation = ReentryOperation::invoke;
    sao_status_t nested_status = SAO_STATUS_OK;
    int calls = 0;
};

ReentryActionProbe* g_reentry_action_probe = nullptr;

std::int32_t SAO_PLUGINS_CALL reentrant_invoke(const char*, std::uint64_t, const char*,
                                               const char*) {
    auto* probe = g_reentry_action_probe;
    if (probe == nullptr || probe->route == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    ++probe->calls;
    if (probe->operation == ReentryOperation::invoke) {
        probe->nested_status = sao::launcher::entity_provider_publication::invoke(
            *probe->route, probe->shell, &reentrant_invoke, nullptr, nullptr);
    } else {
        probe->nested_status = sao::launcher::entity_provider_publication::clear(
            probe->shell, *probe->store, *probe->state, false, &fake_set_roots);
    }
    return SAO_OK;
}

} // namespace

TEST_CASE("Entity action route tokens survive reorder and metadata updates",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    auto first = make_route("provider-a", "action-a", "category-a", "First");
    auto second = make_route("provider-a", "action-b", "category-b", "Second");
    auto third = make_route("provider-b", "action-c", "category-c", "Third");
    const std::vector<EntityActionRouteSpec> initial{first, second, third};

    REQUIRE(store.publish(initial) == SAO_STATUS_OK);
    const auto before = snapshot(store);
    REQUIRE(before.revision == 1);
    REQUIRE(before.routes.size() == 3);
    CHECK(before.routes[0].provider_id == "provider-a");
    CHECK(before.routes[0].category_id == "category-a");
    CHECK(before.routes[0].row_label == "First");
    CHECK(before.routes[1].provider_id == "provider-a");
    CHECK(before.routes[1].category_id == "category-b");
    CHECK(before.routes[2].provider_id == "provider-b");
    const auto first_token = token_for(before, "provider-a", "action-a");
    const auto second_token = token_for(before, "provider-a", "action-b");
    const auto third_token = token_for(before, "provider-b", "action-c");

    first.category_label = "Updated A";
    first.row_label = "Updated First";
    first.row_icon = "updated-icon";
    second.keep_menu_open = true;
    third.can_activate = false;
    REQUIRE(store.publish({third, first, second}) == SAO_STATUS_OK);
    const auto after = snapshot(store);

    REQUIRE(after.revision == 2);
    REQUIRE(after.routes.size() == 3);
    CHECK(after.routes[0].provider_id == "provider-b");
    CHECK(after.routes[1].provider_id == "provider-a");
    CHECK(after.routes[1].category_label == "Updated A");
    CHECK(after.routes[1].row_label == "Updated First");
    CHECK(after.routes[2].category_id == "category-b");
    CHECK(after.routes[2].keep_menu_open);
    CHECK(token_for(after, "provider-a", "action-a") == first_token);
    CHECK(token_for(after, "provider-a", "action-b") == second_token);
    CHECK(token_for(after, "provider-b", "action-c") == third_token);

    first.row_label = "mutated-after-publish";
    third.provider_id = "mutated-after-publish";
    const auto copied = snapshot(store);
    CHECK(copied.routes[0].provider_id == "provider-b");
    CHECK(copied.routes[1].row_label == "Updated First");
}

TEST_CASE("Entity action route revision changes only with snapshot semantics",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    CHECK(snapshot(store).revision == 0);
    REQUIRE(store.publish({}) == SAO_STATUS_OK);
    CHECK(snapshot(store).revision == 0);

    auto route = make_route("provider", "action");
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    const auto first = snapshot(store);
    REQUIRE(first.revision == 1);
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    CHECK(snapshot(store) == first);

    route.row_label = "Changed";
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    auto changed = snapshot(store);
    CHECK(changed.revision == 2);

    route.provider_generation = 44;
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    changed = snapshot(store);
    CHECK(changed.revision == 3);
    CHECK(changed.routes[0].provider_generation == 44);

    route.payload_json = R"({"enabled":true})";
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    changed = snapshot(store);
    CHECK(changed.revision == 4);
    CHECK(changed.routes[0].payload_json == R"({"enabled":true})");

    route.category_priority = -2.5;
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    changed = snapshot(store);
    CHECK(changed.revision == 5);
    CHECK(changed.routes[0].category_priority == -2.5);
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    CHECK(snapshot(store) == changed);
}

TEST_CASE("Entity action route payload has its loader snapshot limit",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    auto route = make_route("provider", "action");
    route.payload_json.assign(sao::launcher::entity_action_routes::kMaximumPayloadBytes, 'p');
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    route.payload_json.push_back('p');
    CHECK(store.publish({route}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("Entity provider catalog is deeply copied sorted and collision checked",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(3);
    fixture.set_row(0, "provider-z", 30, "late", "Late", "L", 5.0, "Zulu", "Z", "action-z",
                    R"({"z":1})");
    fixture.set_row(1, "provider-b", 20, "early", "Early", "E", -1.0, "Beta", "B", "action-b",
                    R"({"b":1})");
    fixture.set_row(2, "provider-a", 10, "early", "Early", "E", -1.0, "Alpha", "A", "action-a",
                    R"({"a":1})");
    fixture.set_root("owner-z", "menu-z", "plugin:z", "Dynamic Z", "Z", 4.5, 0);
    fixture.finish(77);
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
            SAO_STATUS_OK);
    fixture.provider_ids[2] = "mutated-source";
    fixture.payloads[2] = "mutated-source";
    fixture.root_name = "mutated-source";

    std::vector<EntityActionRouteSpec> routes;
    REQUIRE(sao::launcher::entity_provider_catalog::build_routes(catalog, routes) == SAO_STATUS_OK);
    REQUIRE(catalog.revision == 77);
    REQUIRE(catalog.root_contributions.size() == 1);
    CHECK(catalog.root_contributions[0].name == "Dynamic Z");
    CHECK(catalog.root_contributions[0].priority == 4.5);
    REQUIRE(catalog.root_contributions[0].actions.size() == 1);
    CHECK(catalog.root_contributions[0].actions[0].provider_id == "provider-z");
    CHECK(catalog.root_contributions[0].actions[0].action_id == "action-z");
    REQUIRE(routes.size() == 3);
    CHECK(routes[0].provider_id == "provider-a");
    CHECK(routes[0].action_id == "action-a");
    CHECK(routes[0].provider_generation == 10);
    CHECK(routes[0].payload_json == R"({"a":1})");
    CHECK(routes[1].provider_id == "provider-b");
    CHECK(routes[2].provider_id == "provider-z");

    catalog.providers[1].rows[0].category_label = "Conflicting";
    const auto before = routes;
    CHECK(sao::launcher::entity_provider_catalog::build_routes(catalog, routes) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(routes == before);
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider catalog accepts required output ABI prefixes and future metadata",
          "[launcher][entity_provider][focused]") {
    REQUIRE(loader::kEntityProviderCatalogViewRequiredPrefixSize == 48);
    REQUIRE(loader::kEntityProviderViewRequiredPrefixSize == 56);
    REQUIRE(loader::kEntityRootContributionViewRequiredPrefixSize == 72);
    REQUIRE(loader::kEntityRootActionRefViewRequiredPrefixSize == 24);
    REQUIRE(loader::kEntityMenuRowRequiredPrefixSize == 75);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 42, "category", "Category", "C", 3.5, "Row", "R", "action",
                    R"({"value":1})");
    fixture.set_root("owner", "contribution", "dynamic:root", "Dynamic Root", "D", 4.5, 0);
    fixture.finish(123);
    fixture.catalog.struct_size = loader::kEntityProviderCatalogViewRequiredPrefixSize;
    fixture.providers[0].struct_size = loader::kEntityProviderViewRequiredPrefixSize;
    fixture.root.struct_size = loader::kEntityRootContributionViewRequiredPrefixSize;
    fixture.root_action.struct_size = loader::kEntityRootActionRefViewRequiredPrefixSize;
    fixture.rows[0].struct_size = 75;
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog prefix_catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot,
                                                             prefix_catalog) == SAO_STATUS_OK);
    REQUIRE(prefix_catalog.providers.size() == 1);
    REQUIRE(prefix_catalog.providers[0].rows.size() == 1);
    REQUIRE(prefix_catalog.root_contributions.size() == 1);
    REQUIRE(prefix_catalog.root_contributions[0].actions.size() == 1);
    CHECK(prefix_catalog.revision == 123);
    CHECK(prefix_catalog.providers[0].provider_id == "provider");
    CHECK(prefix_catalog.providers[0].rows[0].row_label == "Row");
    CHECK(prefix_catalog.root_contributions[0].name == "Dynamic Root");
    CHECK(prefix_catalog.root_contributions[0].actions[0].action_id == "action");

    fixture.catalog.struct_size = sizeof(loader::entity_provider_catalog_view) + 64;
    fixture.providers[0].struct_size = sizeof(loader::entity_provider_view) + 64;
    fixture.root.struct_size = sizeof(loader::entity_root_contribution_view) + 64;
    fixture.root_action.struct_size = sizeof(loader::entity_root_action_ref_view) + 64;
    fixture.rows[0].struct_size = sizeof(loader::entity_menu_row) + 64;
    OwnedEntityProviderCatalog future_catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot,
                                                             future_catalog) == SAO_STATUS_OK);
    fixture.provider_ids[0] = "mutated-provider";
    fixture.row_labels[0] = "mutated-row";
    fixture.root_name = "mutated-root";
    fixture.action_ids[0] = "mutated-action";

    REQUIRE(future_catalog.providers.size() == 1);
    REQUIRE(future_catalog.providers[0].rows.size() == 1);
    REQUIRE(future_catalog.root_contributions.size() == 1);
    REQUIRE(future_catalog.root_contributions[0].actions.size() == 1);
    CHECK(future_catalog.providers[0].provider_id == "provider");
    CHECK(future_catalog.providers[0].rows[0].row_label == "Row");
    CHECK(future_catalog.root_contributions[0].name == "Dynamic Root");
    CHECK(future_catalog.root_contributions[0].actions[0].action_id == "action");
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider catalog preserves fixed-stride multi-element output views",
          "[launcher][entity_provider][abi][fixed-stride][focused]") {
    CatalogFixture fixture;
    fixture.reset(2);
    fixture.set_row(0, "provider-a", 41, "category-a", "Category A", "A", 1.0, "Row A", "A",
                    "action-a", "{}");
    fixture.set_row(1, "provider-b", 42, "category-b", "Category B", "B", 2.0, "Row B", "B",
                    "action-b", "{}");
    fixture.finish(125);
    fixture.catalog.struct_size = loader::kEntityProviderCatalogViewRequiredPrefixSize;
    for (auto& provider : fixture.providers)
        provider.struct_size = loader::kEntityProviderViewRequiredPrefixSize;
    for (auto& row : fixture.rows)
        row.struct_size = loader::kEntityMenuRowRequiredPrefixSize;
    fixture.providers[0].row_count = 2;
    fixture.providers[0].rows = fixture.rows.data();
    std::vector<loader::entity_root_action_ref_view> root_actions = {
        {loader::kEntityRootActionRefViewRequiredPrefixSize, "provider-a", "action-a"},
        {loader::kEntityRootActionRefViewRequiredPrefixSize, "provider-b", "action-b"},
        {loader::kEntityRootActionRefViewRequiredPrefixSize, "provider-b", "action-b"},
    };
    std::vector<loader::entity_root_contribution_view> roots = {
        {loader::kEntityRootContributionViewRequiredPrefixSize, "owner-a", "contribution-a",
         "root-a", "Root A", "A", 1.0, 2, root_actions.data()},
        {loader::kEntityRootContributionViewRequiredPrefixSize, "owner-b", "contribution-b",
         "root-b", "Root B", "B", 2.0, 1, root_actions.data() + 2},
    };
    fixture.catalog.root_contribution_count = static_cast<std::uint32_t>(roots.size());
    fixture.catalog.root_contributions = roots.data();
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
            SAO_STATUS_OK);
    REQUIRE(catalog.providers.size() == 2);
    REQUIRE(catalog.providers[0].rows.size() == 2);
    REQUIRE(catalog.providers[1].rows.size() == 1);
    CHECK(catalog.providers[0].provider_id == "provider-a");
    CHECK(catalog.providers[0].rows[0].action_id == "action-a");
    CHECK(catalog.providers[0].rows[1].action_id == "action-b");
    CHECK(catalog.providers[1].provider_id == "provider-b");
    CHECK(catalog.providers[1].rows[0].action_id == "action-b");
    REQUIRE(catalog.root_contributions.size() == 2);
    REQUIRE(catalog.root_contributions[0].actions.size() == 2);
    CHECK(catalog.root_contributions[0].root_id == "root-a");
    CHECK(catalog.root_contributions[0].actions[1].provider_id == "provider-b");
    CHECK(catalog.root_contributions[0].actions[1].action_id == "action-b");
    REQUIRE(catalog.root_contributions[1].actions.size() == 1);
    CHECK(catalog.root_contributions[1].root_id == "root-b");
    CHECK(catalog.root_contributions[1].actions[0].provider_id == "provider-b");
    CHECK(catalog.root_contributions[1].actions[0].action_id == "action-b");
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider catalog rejects truncated output ABI prefixes transactionally",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 42, "category", "Category", "C", 3.5, "Row", "R", "action",
                    "{}");
    fixture.set_root("owner", "contribution", "dynamic:root", "Dynamic Root", "D", 4.5, 0);
    fixture.finish(123);

    SECTION("catalog prefix minus one") {
        fixture.catalog.struct_size = loader::kEntityProviderCatalogViewRequiredPrefixSize - 1;
        check_catalog_snapshot_rejected_transactionally(fixture);
    }
    SECTION("provider prefix minus one") {
        fixture.providers[0].struct_size = loader::kEntityProviderViewRequiredPrefixSize - 1;
        check_catalog_snapshot_rejected_transactionally(fixture);
    }
    SECTION("root contribution prefix minus one") {
        fixture.root.struct_size = loader::kEntityRootContributionViewRequiredPrefixSize - 1;
        check_catalog_snapshot_rejected_transactionally(fixture);
    }
    SECTION("root action prefix minus one") {
        fixture.root_action.struct_size = loader::kEntityRootActionRefViewRequiredPrefixSize - 1;
        check_catalog_snapshot_rejected_transactionally(fixture);
    }
    SECTION("row size 74") {
        fixture.rows[0].struct_size = 74;
        check_catalog_snapshot_rejected_transactionally(fixture);
    }
}

TEST_CASE("Entity provider catalog keeps wide input until route publication validation",
          "[launcher][entity_provider][budget][focused]") {
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 42, "category", "Category", "C", 3.5,
                    std::string(sao::launcher::entity_action_routes::kMaximumStringBytes + 1, 'r'),
                    "R", "action", "{}");
    fixture.finish(124);
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
            SAO_STATUS_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK(catalog.providers[0].rows[0].row_label.size() ==
          sao::launcher::entity_action_routes::kMaximumStringBytes + 1);

    std::vector<EntityActionRouteSpec> routes{make_route("sentinel", "sentinel")};
    const auto before = routes;
    CHECK(sao::launcher::entity_provider_catalog::build_routes(catalog, routes) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(routes == before);
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider publication does not resync active UI when route commit loses",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    auto baseline =
        make_route("baseline-provider", "baseline-action", "baseline-category", "Baseline");
    baseline.provider_generation = 1;
    baseline.payload_json = "{}";
    REQUIRE(store.publish({baseline}) == SAO_STATUS_OK);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 9, "candidate-category", "Candidate Category", "C",
                    1.0, "Candidate", "A", "candidate-action", R"({"candidate":true})");
    fixture.finish(8);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.competing_store = &store;
    log.competing_route = make_route("winner-provider", "winner-action");
    log.publish_competitor = true;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_CANCELLED);
    REQUIRE(log.calls.size() == 2);
    REQUIRE(log.calls[0].size() == 5);
    CHECK(log.calls[0][0].id == "Control");
    CHECK(log.calls[0][1].id == "Tools");
    CHECK(log.calls[0][2].id == "Plugins");
    CHECK(log.calls[0][3].id == "Skins");
    CHECK(log.calls[0][4].id == "About");
    CHECK(published_root(log.calls[0], "Control").children[1].name == "NervGear: OFF");
    const auto& candidate_plugins = published_root(log.calls[0], "Plugins").children;
    REQUIRE(candidate_plugins.size() == 4);
    CHECK(candidate_plugins[0].name == "插件管理面板 Manage");
    CHECK(candidate_plugins[1].name == "重载全部插件 Reload");
    CHECK(candidate_plugins[2].name == "Candidate Category");
    CHECK(candidate_plugins[3].name == "Candidate");
    CHECK_FALSE(candidate_plugins[2].can_activate);
    const auto& fail_closed = log.calls[1];
    CHECK(std::none_of(fail_closed.begin(), fail_closed.end(),
                       [](const auto& root) { return root.can_activate; }));
    for (const auto& root : fail_closed) {
        CHECK(std::none_of(root.children.begin(), root.children.end(),
                           [](const auto& row) { return row.can_activate; }));
    }
    CHECK_FALSE(state.has_catalog_revision);

    const auto committed = snapshot(store);
    CHECK(committed.routes.empty());
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider fail-closed retries a competing clear publication",
          "[launcher][entity_provider][transaction][concurrency][focused]") {
    EntityActionRouteStore store;
    auto baseline = make_route("baseline-provider", "baseline-action");
    baseline.provider_generation = 1;
    baseline.payload_json = "{}";
    REQUIRE(store.publish({baseline}) == SAO_STATUS_OK);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 9, "candidate-category", "Candidate Category", "C",
                    1.0, "Candidate", "A", "candidate-action", "{}");
    fixture.finish(9);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.competing_store = &store;
    log.competing_publications = {
        {1, make_route("first-winner", "first-action")},
        {2, make_route("second-winner", "second-action")},
    };
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_CANCELLED);
    REQUIRE(log.calls.size() == 3);
    const auto& fail_closed = log.calls.back();
    CHECK(std::none_of(fail_closed.begin(), fail_closed.end(),
                       [](const auto& root) { return root.can_activate; }));
    for (const auto& root : fail_closed) {
        CHECK(std::none_of(root.children.begin(), root.children.end(),
                           [](const auto& row) { return row.can_activate; }));
    }
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider fail-closed retry is bounded for a cancelled UI sink",
          "[launcher][entity_provider][transaction][cancelled][focused]") {
    EntityActionRouteStore store;
    auto baseline = make_route("baseline-provider", "baseline-action");
    baseline.provider_generation = 1;
    baseline.payload_json = "{}";
    REQUIRE(store.publish({baseline}) == SAO_STATUS_OK);
    const auto before = snapshot(store);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 9, "candidate-category", "Candidate Category", "C",
                    1.0, "Candidate", "A", "candidate-action", "{}");
    fixture.finish(10);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.publication_statuses = {
        SAO_STATUS_ERR_CANCELLED,
        SAO_STATUS_ERR_CANCELLED,
        SAO_STATUS_ERR_CANCELLED,
        SAO_STATUS_OK,
    };
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_CANCELLED);
    CHECK(log.calls.size() == 3);
    CHECK(log.publication_attempt == 3);
    CHECK(snapshot(store) == before);
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider fail-closed gate survives persistent root sink failure and recovers",
          "[launcher][entity_provider][transaction][fail_closed][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 7, "category", "Category", "C", 0.0, "Before", "R", "action",
                    "{}");
    fixture.finish(11);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto published = snapshot(store);
    REQUIRE(published.routes.size() == 1);
    const auto stable_token = published.routes[0].token;
    const auto old_route = published.routes[0];

    fixture.providers[0].generation = 8;
    fixture.row_labels[0] = "After failure";
    fixture.rows[0].row_label_utf8 = fixture.row_labels[0].c_str();
    fixture.catalog.revision = 12;
    log = {};
    log.competing_store = &store;
    log.route_probe_token = stable_token;
    log.publication_status = SAO_STATUS_ERR_OS_CALL_FAILED;

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(log.calls.size() == 2);
    CHECK(all_actions_disabled(log.calls[1]));
    CHECK(log.route_probe_statuses ==
          std::vector<sao_status_t>{SAO_STATUS_ERR_NOT_FOUND, SAO_STATUS_ERR_NOT_FOUND});
    CHECK(snapshot(store).routes == published.routes);
    EntityActionRoute resolved;
    CHECK(store.resolve(stable_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);
    ActionLog blocked_action;
    g_action_log = &blocked_action;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              old_route, shell, &fake_invoke, &fake_get_shell_snapshot, &fake_home) ==
          SAO_STATUS_ERR_NOT_FOUND);
    CHECK(blocked_action.order.empty());
    CHECK_FALSE(state.has_catalog_revision);
    CHECK(state.catalog_revision == 0);

    log = {};
    log.competing_store = &store;
    log.route_probe_token = stable_token;
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    CHECK(log.route_probe_statuses == std::vector<sao_status_t>{SAO_STATUS_ERR_NOT_FOUND});
    REQUIRE(store.resolve(stable_token, resolved) == SAO_STATUS_OK);
    CHECK(resolved.provider_generation == 8);
    CHECK(resolved.row_label == "After failure");
    ActionLog recovered_action;
    g_action_log = &recovered_action;
    CHECK(sao::launcher::entity_provider_publication::invoke(resolved, shell, &fake_invoke,
                                                             &fake_get_shell_snapshot,
                                                             &fake_home) == SAO_STATUS_OK);
    CHECK(std::find(recovered_action.order.begin(), recovered_action.order.end(), "invoke") !=
          recovered_action.order.end());
    g_action_log = nullptr;
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider fail-closed gate survives persistent competing writers and recovers",
          "[launcher][entity_provider][transaction][concurrency][fail_closed][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 17, "category", "Category", "C", 0.0, "Before", "R", "action",
                    "{}");
    fixture.finish(21);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto published = snapshot(store);
    REQUIRE(published.routes.size() == 1);
    const auto stable_token = published.routes[0].token;
    const auto old_route = published.routes[0];

    fixture.providers[0].generation = 18;
    fixture.row_labels[0] = "After competition";
    fixture.rows[0].row_label_utf8 = fixture.row_labels[0].c_str();
    fixture.catalog.revision = 22;
    log = {};
    log.competing_store = &store;
    log.route_probe_token = stable_token;
    for (std::size_t call = 1; call <= 12; ++call) {
        auto writer =
            make_route("provider", "action", "writer-category", "Writer " + std::to_string(call));
        writer.provider_generation = 100 + call;
        writer.payload_json = "{}";
        log.competing_publications.emplace_back(call, std::move(writer));
    }

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_CANCELLED);
    REQUIRE(log.calls.size() == 3);
    REQUIRE(log.route_probe_statuses.size() == log.calls.size());
    CHECK(std::all_of(log.route_probe_statuses.begin(), log.route_probe_statuses.end(),
                      [](sao_status_t status) { return status == SAO_STATUS_ERR_NOT_FOUND; }));
    CHECK(std::all_of(log.calls.begin() + 1, log.calls.end(),
                      [](const auto& roots) { return all_actions_disabled(roots); }));
    const auto competing = snapshot(store);
    REQUIRE(competing.routes.size() == 1);
    CHECK(competing.routes[0].token == stable_token);
    CHECK(competing.routes[0].row_label == "Writer 3");
    EntityActionRoute resolved;
    CHECK(store.resolve(stable_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);
    ActionLog blocked_action;
    g_action_log = &blocked_action;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              old_route, shell, &fake_invoke, &fake_get_shell_snapshot, &fake_home) ==
          SAO_STATUS_ERR_NOT_FOUND);
    CHECK(blocked_action.order.empty());
    CHECK_FALSE(state.has_catalog_revision);

    log = {};
    log.competing_store = &store;
    log.route_probe_token = stable_token;
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    CHECK(log.route_probe_statuses == std::vector<sao_status_t>{SAO_STATUS_ERR_NOT_FOUND});
    REQUIRE(store.resolve(stable_token, resolved) == SAO_STATUS_OK);
    CHECK(resolved.provider_generation == 18);
    CHECK(resolved.row_label == "After competition");
    ActionLog recovered_action;
    g_action_log = &recovered_action;
    CHECK(sao::launcher::entity_provider_publication::invoke(resolved, shell, &fake_invoke,
                                                             &fake_get_shell_snapshot,
                                                             &fake_home) == SAO_STATUS_OK);
    CHECK(std::find(recovered_action.order.begin(), recovered_action.order.end(), "invoke") !=
          recovered_action.order.end());
    g_action_log = nullptr;
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider fail-closed retries one cancelled UI sink publication",
          "[launcher][entity_provider][transaction][cancelled][focused]") {
    EntityActionRouteStore store;
    auto baseline = make_route("baseline-provider", "baseline-action");
    baseline.provider_generation = 1;
    baseline.payload_json = "{}";
    REQUIRE(store.publish({baseline}) == SAO_STATUS_OK);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 9, "candidate-category", "Candidate Category", "C",
                    1.0, "Candidate", "A", "candidate-action", "{}");
    fixture.finish(10);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.publication_statuses = {
        SAO_STATUS_ERR_CANCELLED,
        SAO_STATUS_ERR_CANCELLED,
        SAO_STATUS_OK,
    };
    g_publication_log = &log;
    EntityProviderPublicationState state;
    state.has_catalog_revision = true;
    state.catalog_revision = 99;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_CANCELLED);
    CHECK(log.calls.size() == 3);
    CHECK(log.publication_attempt == 3);
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    CHECK(state.catalog_revision == 0);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider publication shows its placeholder only when empty",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(0);
    fixture.finish(12);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    state.streaming_mode = true;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, true,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    const auto& control = published_root(log.calls[0], "Control").children;
    REQUIRE(control.size() == 8);
    CHECK(control[3].name == "Streaming Mode: ON");
    const auto& plugins = published_root(log.calls[0], "Plugins").children;
    REQUIRE(plugins.size() == 3);
    CHECK(plugins[2].name == "无已启用面板插件");
    CHECK_FALSE(plugins[2].can_activate);
    CHECK(state.has_catalog_revision);
    CHECK(state.catalog_revision == 12);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider action honors close timing and maps loader statuses",
          "[launcher][entity_provider][focused]") {
    EntityActionRoute route;
    route.provider_id = "provider";
    route.provider_generation = 42;
    route.action_id = "action";
    route.payload_json = R"({"value":7})";
    route.can_activate = true;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    ActionLog log;
    g_action_log = &log;
    route.close_menu_before = true;
    log.invoke_status = loader::SAO_PLUGINS_ERR_BUSY;
    CHECK(sao::launcher::entity_provider_publication::invoke(route, shell, &fake_invoke,
                                                             &fake_get_shell_snapshot,
                                                             &fake_home) == SAO_STATUS_ERR_TIMEOUT);
    CHECK(log.order == std::vector<std::string>{"snapshot", "home", "invoke"});
    CHECK(log.provider_id == "provider");
    CHECK(log.generation == 42);
    CHECK(log.action_id == "action");
    CHECK(log.payload == R"({"value":7})");

    log = {};
    route.close_menu_before = false;
    log.invoke_status = SAO_ERR_HANDLE_INVALID;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              route, shell, &fake_invoke, &fake_get_shell_snapshot, &fake_home) ==
          SAO_STATUS_ERR_HANDLE_INVALID);
    CHECK(log.order == std::vector<std::string>{"invoke", "snapshot", "home"});

    log = {};
    route.keep_menu_open = true;
    log.invoke_status = -32123;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              route, shell, &fake_invoke, &fake_get_shell_snapshot, &fake_home) ==
          SAO_STATUS_ERR_SCRIPT_RUNTIME);
    CHECK(log.order == std::vector<std::string>{"invoke"});
    g_action_log = nullptr;
}

TEST_CASE("Entity route rundown blocks copied routes and drains active callbacks",
          "[launcher][entity_provider][invocation][rundown][concurrency][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 31, "category", "Category", "C", 0.0, "Action", "A",
                    "action", "{}");
    fixture.finish(301);
    g_catalog_fixture = &fixture;

    PublicationLog publication_log;
    g_publication_log = &publication_log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    auto copied_route = snapshot(store).routes.front();
    copied_route.keep_menu_open = true;

    BlockingActionProbe probe;
    g_blocking_action_probe = &probe;
    std::atomic<sao_status_t> invoke_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread invoke_thread([&] {
        invoke_status.store(sao::launcher::entity_provider_publication::invoke(
                                copied_route, shell, &blocking_invoke, nullptr, nullptr),
                            std::memory_order_release);
    });
    {
        std::unique_lock lock(probe.mutex);
        REQUIRE(probe.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&probe] { return probe.entered; }));
    }

    std::atomic_bool clear_finished{false};
    std::atomic<sao_status_t> clear_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread clear_thread([&] {
        clear_status.store(sao::launcher::entity_provider_publication::clear(
                               shell, store, state, false, &fake_set_roots),
                           std::memory_order_release);
        clear_finished.store(true, std::memory_order_release);
    });

    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (copied_route.invocation_allowed() && std::chrono::steady_clock::now() < close_deadline) {
        std::this_thread::yield();
    }
    REQUIRE_FALSE(copied_route.invocation_allowed());
    CHECK_FALSE(clear_finished.load(std::memory_order_acquire));

    ActionLog blocked;
    g_action_log = &blocked;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              copied_route, shell, &fake_invoke, &fake_get_shell_snapshot, &fake_home) ==
          SAO_STATUS_ERR_NOT_FOUND);
    CHECK(blocked.order.empty());
    CHECK(probe.calls.load(std::memory_order_acquire) == 1);

    {
        std::lock_guard lock(probe.mutex);
        probe.release = true;
    }
    probe.condition.notify_all();
    invoke_thread.join();
    clear_thread.join();

    CHECK(invoke_status.load(std::memory_order_acquire) == SAO_STATUS_OK);
    CHECK(clear_status.load(std::memory_order_acquire) == SAO_STATUS_OK);
    CHECK(clear_finished.load(std::memory_order_acquire));
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(copied_route.invocation_allowed());

    g_action_log = nullptr;
    g_blocking_action_probe = nullptr;
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity route invocation and clear reentry return busy without invalidating authority",
          "[launcher][entity_provider][invocation][reentry][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 32, "category", "Category", "C", 0.0, "Action", "A",
                    "action", "{}");
    fixture.finish(302);
    g_catalog_fixture = &fixture;

    PublicationLog publication_log;
    g_publication_log = &publication_log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    auto route = snapshot(store).routes.front();
    route.keep_menu_open = true;

    ReentryActionProbe probe{&route, &store, &state, shell};
    g_reentry_action_probe = &probe;
    REQUIRE(sao::launcher::entity_provider_publication::invoke(
                route, shell, &reentrant_invoke, nullptr, nullptr) == SAO_STATUS_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.nested_status == SAO_STATUS_ERR_TIMEOUT);
    CHECK(route.invocation_allowed());

    probe.operation = ReentryOperation::clear;
    REQUIRE(sao::launcher::entity_provider_publication::invoke(
                route, shell, &reentrant_invoke, nullptr, nullptr) == SAO_STATUS_OK);
    CHECK(probe.calls == 2);
    CHECK(probe.nested_status == SAO_STATUS_ERR_TIMEOUT);
    CHECK(route.invocation_allowed());
    EntityActionRoute resolved;
    CHECK(store.resolve(route.token, resolved) == SAO_STATUS_OK);

    REQUIRE(sao::launcher::entity_provider_publication::clear(shell, store, state, false,
                                                              &fake_set_roots) == SAO_STATUS_OK);
    CHECK_FALSE(route.invocation_allowed());
    CHECK(sao::launcher::entity_provider_publication::invoke(
              route, shell, &reentrant_invoke, nullptr, nullptr) == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(probe.calls == 2);

    g_reentry_action_probe = nullptr;
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity refresh permanently retires copied routes even when semantics are unchanged",
          "[launcher][entity_provider][invocation][refresh][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 33, "category", "Category", "C", 0.0, "Action", "A",
                    "action", "{}");
    fixture.finish(303);
    g_catalog_fixture = &fixture;

    PublicationLog publication_log;
    g_publication_log = &publication_log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    auto stale = snapshot(store).routes.front();
    stale.keep_menu_open = true;

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    auto current = snapshot(store).routes.front();
    current.keep_menu_open = true;
    CHECK(current.token == stale.token);
    CHECK_FALSE(stale.invocation_allowed());
    CHECK(current.invocation_allowed());

    ActionLog log;
    g_action_log = &log;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              stale, shell, &fake_invoke, nullptr, nullptr) == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(log.order.empty());
    CHECK(sao::launcher::entity_provider_publication::invoke(
              current, shell, &fake_invoke, nullptr, nullptr) == SAO_STATUS_OK);
    CHECK(log.order == std::vector<std::string>{"invoke"});

    g_action_log = nullptr;
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider catalog rejects duplicate providers and invalid UTF-8",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(2);
    fixture.set_row(0, "provider", 10, "category-a", "Category A", "A", 0.0, "First", "1",
                    "action-a", "{}");
    fixture.set_row(1, "provider", 11, "category-b", "Category B", "B", 1.0, "Second", "2",
                    "action-b", "{}");
    fixture.finish();
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    CHECK(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    fixture.provider_ids[1] = "provider-b";
    fixture.providers[1].provider_id_utf8 = fixture.provider_ids[1].c_str();
    fixture.owner_ids[1] = std::string{"\xc0\x80", 2};
    fixture.providers[1].owner_plugin_id_utf8 = fixture.owner_ids[1].c_str();
    CHECK(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider catalog normalizes null payload to an empty object",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 10, "category", "Category", "C", 0.0, "Row", "R", "action",
                    "ignored");
    fixture.rows[0].payload_json_utf8 = nullptr;
    fixture.finish();
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(&fake_catalog_snapshot, catalog) ==
            SAO_STATUS_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK(catalog.providers[0].rows[0].payload_json == "{}");
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider poll validates dependencies before changing timer state",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    EntityProviderPublicationState state;
    state.refresh_elapsed_ms = 17;
    CHECK(sao::launcher::entity_provider_publication::poll(
              nullptr, store, state, 5, true, nullptr, nullptr) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(state.refresh_elapsed_ms == 17);
}

TEST_CASE("Entity provider publication snapshots restored generation after failed reload",
          "[launcher][entity_provider][reload][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 7, "category", "Category", "C", 0.0, "Before", "R", "action",
                    "{}");
    fixture.finish(22);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto before = snapshot(store);
    REQUIRE(before.routes.size() == 1);
    REQUIRE(before.routes[0].provider_generation == 7);
    REQUIRE(state.catalog_revision == 22);

    fixture.providers[0].generation = 8;
    fixture.row_labels[0] = "Restored";
    fixture.rows[0].row_label_utf8 = fixture.row_labels[0].c_str();
    fixture.catalog.revision = 23;
    state.topmost = true;
    state.builtin_authority.topmost = true;
    state.builtin_authority.topmost_status =
        sao::launcher::entity_provider_publication::TopmostPublicationStatus::ready;

    REQUIRE(sao::launcher::entity_provider_publication::poll(
                shell, store, state, sao::launcher::entity_provider_publication::kRefreshIntervalMs,
                false, &fake_catalog_snapshot, &fake_set_roots) == SAO_STATUS_OK);
    const auto restored = snapshot(store);
    REQUIRE(restored.routes.size() == 1);
    CHECK(restored.revision == before.revision + 1);
    CHECK(restored.routes[0].provider_generation == 8);
    CHECK(restored.routes[0].row_label == "Restored");
    CHECK(state.catalog_revision == 23);
    REQUIRE(log.calls.size() == 2);
    CHECK(published_root(log.calls.back(), "Control").children[0].name == "置顶: ON");
    CHECK(published_root(log.calls.back(), "Plugins").children.back().name == "Restored");

    state.builtin_authority.plugin_runtime = sao::launcher::entity_provider_publication::
        PluginRuntimePublicationStatus::degraded_internal;
    REQUIRE(sao::launcher::entity_provider_publication::poll(
                shell, store, state, sao::launcher::entity_provider_publication::kRefreshIntervalMs,
                false, &fake_catalog_snapshot, &fake_set_roots) == SAO_STATUS_OK);
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    const auto& degraded_plugins = published_root(log.calls.back(), "Plugins").children;
    REQUIRE(degraded_plugins.size() == 4);
    CHECK_FALSE(degraded_plugins[0].can_activate);
    CHECK_FALSE(degraded_plugins[1].can_activate);
    CHECK(degraded_plugins[2].name == "Plugin Runtime: DEGRADED/INTERNAL");
    CHECK(degraded_plugins[3].name == "无已启用面板插件");
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider same catalog revision content replacement characterizes v1 limitation",
          "[launcher][entity-provider][same-revision][focused]") {
    constexpr std::uint64_t kCatalogRevision = 214;
    constexpr std::uint64_t kProviderRevision = 14;
    constexpr std::uint64_t kGeneration = 21;

    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "same-revision-provider", kGeneration, "same-revision-category",
                    "Same Revision", "S", 0.0, "Label A", "A", "same-revision-action",
                    R"({"content":"A"})");
    fixture.providers[0].revision = kProviderRevision;
    fixture.finish(kCatalogRevision);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto first = snapshot(store);
    REQUIRE(first.routes.size() == 1);
    const auto first_token = first.routes[0].token;
    CHECK(first.routes[0].provider_id == "same-revision-provider");
    CHECK(first.routes[0].provider_generation == kGeneration);
    CHECK(first.routes[0].action_id == "same-revision-action");
    CHECK(first.routes[0].row_label == "Label A");
    CHECK(first.routes[0].payload_json == R"({"content":"A"})");
    REQUIRE(state.has_catalog_revision);
    CHECK(state.catalog_revision == kCatalogRevision);
    REQUIRE(state.published_catalog.providers.size() == 1);
    REQUIRE(state.published_catalog.providers[0].rows.size() == 1);
    CHECK(state.published_catalog.revision == kCatalogRevision);
    CHECK(state.published_catalog.providers[0].provider_id == "same-revision-provider");
    CHECK(state.published_catalog.providers[0].generation == kGeneration);
    CHECK(state.published_catalog.providers[0].revision == kProviderRevision);
    CHECK(state.published_catalog.providers[0].rows[0].action_id == "same-revision-action");
    CHECK(state.published_catalog.providers[0].rows[0].row_label == "Label A");
    CHECK(state.published_catalog.providers[0].rows[0].payload_json == R"({"content":"A"})");
    REQUIRE(log.calls.size() == 1);
    CHECK(published_root(log.calls[0], "Plugins").children.back().name == "Label A");

    // Characterization of the v1 limitation: equal catalog and provider revisions do not make
    // their associated content immutable across refreshes.
    fixture.row_labels[0] = "Label B";
    fixture.rows[0].row_label_utf8 = fixture.row_labels[0].c_str();
    fixture.payloads[0] = R"({"content":"B"})";
    fixture.rows[0].payload_json_utf8 = fixture.payloads[0].c_str();

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto second = snapshot(store);
    REQUIRE(second.routes.size() == 1);
    CHECK(state.catalog_revision == kCatalogRevision);
    CHECK(second.revision == first.revision + 1);
    CHECK(second.routes[0].token == first_token);
    CHECK(second.routes[0].provider_id == first.routes[0].provider_id);
    CHECK(second.routes[0].provider_generation == first.routes[0].provider_generation);
    CHECK(second.routes[0].category_id == first.routes[0].category_id);
    CHECK(second.routes[0].action_id == first.routes[0].action_id);
    CHECK(second.routes[0].row_label == "Label B");
    CHECK(second.routes[0].payload_json == R"({"content":"B"})");

    EntityActionRoute active;
    REQUIRE(store.resolve(first_token, active) == SAO_STATUS_OK);
    CHECK(active.provider_id == "same-revision-provider");
    CHECK(active.provider_generation == kGeneration);
    CHECK(active.action_id == "same-revision-action");
    CHECK(active.row_label == "Label B");
    CHECK(active.payload_json == R"({"content":"B"})");

    REQUIRE(state.published_catalog.providers.size() == 1);
    const auto& published_provider = state.published_catalog.providers[0];
    REQUIRE(published_provider.rows.size() == 1);
    CHECK(state.published_catalog.revision == kCatalogRevision);
    CHECK(published_provider.revision == kProviderRevision);
    CHECK(published_provider.provider_id == "same-revision-provider");
    CHECK(published_provider.generation == kGeneration);
    CHECK(published_provider.rows[0].action_id == "same-revision-action");
    CHECK(published_provider.rows[0].row_label == "Label B");
    CHECK(published_provider.rows[0].payload_json == R"({"content":"B"})");
    REQUIRE(log.calls.size() == 2);
    CHECK(published_root(log.calls.back(), "Plugins").children.back().name == "Label B");

    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider publication preserves canonical roots and clear mode",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 5, "category", "Category", "C", 0.0, "Action", "A", "action",
                    "{}");
    fixture.finish(21);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    const auto& roots = log.calls[0];
    REQUIRE(roots.size() == 5);
    const auto& control = published_root(roots, "Control");
    CHECK(control.struct_size == sizeof(SaoUiEntityRootItem));
    CHECK(control.name == "Control");
    CHECK(control.icon == "C");
    CHECK(control.action_id == 10);
    CHECK(control.can_activate);
    REQUIRE(control.children.size() == 8);
    CHECK(control.children[0].action_id == SAO_UI_ENTITY_ACTION_TOGGLE_TOPMOST);
    CHECK(control.children[1].name == "NervGear: OFF");
    CHECK(control.children[1].action_id == SAO_UI_ENTITY_ACTION_TOGGLE_NERVGEAR);
    CHECK(control.children[2].action_id == -1);
    CHECK(control.children[3].action_id == SAO_UI_ENTITY_ACTION_TOGGLE_STREAMING_MODE);
    CHECK(control.children[4].action_id == SAO_UI_ENTITY_ACTION_SET_FISHEYE_PROCEDURAL);
    CHECK(control.children[5].action_id == SAO_UI_ENTITY_ACTION_SET_FISHEYE_LIVE);
    CHECK(control.children[6].action_id == -1);
    CHECK(control.children[7].action_id == SAO_UI_ENTITY_ACTION_SAVE_SETTINGS);

    const auto& tools = published_root(roots, "Tools");
    CHECK(tools.name == "Tools");
    CHECK(tools.icon == "T");
    CHECK(tools.action_id == 11);
    REQUIRE(tools.children.size() == 3);
    CHECK(tools.children[0].action_id == SAO_UI_ENTITY_ACTION_OPEN_AI_EDITOR);
    CHECK(tools.children[1].action_id == SAO_UI_ENTITY_ACTION_OPEN_WORKSHOP);
    CHECK(tools.children[2].action_id == SAO_UI_ENTITY_ACTION_OPEN_PROCESS_SELECTOR);

    const auto& plugins = published_root(roots, "Plugins");
    CHECK(plugins.name == "Plugins");
    CHECK(plugins.icon == "P");
    CHECK(plugins.action_id == 12);
    CHECK(plugins.can_activate);

    const auto& skins = published_root(roots, "Skins");
    CHECK(skins.name == "Skins");
    CHECK(skins.icon == "S");
    CHECK(skins.action_id == 13);
    REQUIRE(skins.children.size() == 2);
    CHECK(skins.children[0].action_id == SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT);
    CHECK(skins.children[1].action_id == SAO_UI_ENTITY_ACTION_SET_ALL_DARK);

    const auto& about = published_root(roots, "About");
    CHECK(about.name == "About");
    CHECK(about.icon == "?");
    CHECK(about.action_id == SAO_UI_ENTITY_ACTION_OPEN_ABOUT);
    CHECK(about.can_activate);
    CHECK(about.children.empty());

    REQUIRE(sao::launcher::entity_provider_publication::clear(shell, store, state, true,
                                                              &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 2);
    CHECK(published_root(log.calls[1], "Control").children[1].name == "NervGear: ON");
    const auto& cleared_plugins = published_root(log.calls[1], "Plugins").children;
    REQUIRE(cleared_plugins.size() == 3);
    CHECK(cleared_plugins[2].name == "无已启用面板插件");
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity builtins publish only activatable launcher-owned authorities",
          "[launcher][entity_provider][authority][python_runtime][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(0);
    fixture.finish(24);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    state.topmost = true;
    state.builtin_authority.topmost = true;
    state.builtin_authority.topmost_status =
        sao::launcher::entity_provider_publication::TopmostPublicationStatus::ready;
    state.builtin_authority.nervgear = true;
    state.builtin_authority.streaming = true;
    state.builtin_authority.save_settings = true;
    state.builtin_authority.ai_editor = true;
    state.builtin_authority.reload_plugins = true;
    state.builtin_authority.theme = true;
    state.builtin_authority.controls =
        sao::launcher::entity_provider_publication::ControlPublicationStatus::degraded_internal;
    state.builtin_authority.python_runtime = sao::launcher::entity_provider_publication::
        PythonRuntimePublicationStatus::degraded_unconfigured;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, true,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    const auto& control = published_root(log.calls[0], "Control").children;
    REQUIRE(control.size() == 8);
    CHECK(control[0].name == "置顶: ON");
    CHECK_FALSE(control[0].can_activate);
    CHECK_FALSE(control[1].can_activate);
    CHECK_FALSE(control[3].can_activate);
    CHECK_FALSE(control[4].can_activate);
    CHECK_FALSE(control[5].can_activate);
    CHECK_FALSE(control[7].can_activate);

    const auto& tools = published_root(log.calls[0], "Tools").children;
    REQUIRE(tools.size() == 3);
    CHECK(tools[0].can_activate);
    CHECK_FALSE(tools[1].can_activate);
    CHECK_FALSE(tools[2].can_activate);

    const auto& plugins = published_root(log.calls[0], "Plugins").children;
    REQUIRE(plugins.size() == 5);
    CHECK_FALSE(plugins[0].can_activate);
    CHECK(plugins[1].can_activate);
    CHECK(plugins[2].name == "Launcher Controls: DEGRADED/INTERNAL");
    CHECK_FALSE(plugins[2].can_activate);
    CHECK(plugins[3].name == "Python Runtime: DEGRADED (未配置 python_home)");
    CHECK_FALSE(plugins[3].can_activate);
    CHECK(plugins[4].name == "无已启用面板插件");
    CHECK(std::none_of(plugins.begin(), plugins.end(), [](const auto& row) {
        return row.action_id == SAO_UI_ENTITY_ACTION_PLUGIN_STATUS;
    }));

    const auto& skins = published_root(log.calls[0], "Skins").children;
    CHECK(skins[0].can_activate);
    CHECK(skins[1].can_activate);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity AI Editor row follows compiled launcher ABI capability",
          "[launcher][entity_provider][authority][ai_editor][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(0);
    fixture.finish(26);
    g_catalog_fixture = &fixture;
    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    state.builtin_authority.ai_editor =
        sao::launcher::tool_launch::ai_editor_capability_available();
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto& ai_editor = published_root(log.calls.back(), "Tools").children[0];
    CHECK(ai_editor.can_activate == sao::launcher::tool_launch::ai_editor_capability_available());
#if !defined(SAO_LAUNCHER_HAS_AI_EDITOR_ABI)
    CHECK_FALSE(ai_editor.can_activate);
#endif
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity topmost stays disabled when platform authority is not observable",
          "[launcher][entity_provider][authority][topmost][degraded][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(0);
    fixture.finish(25);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    state.topmost = true;
    state.builtin_authority.topmost = true;
    state.builtin_authority.topmost_status = sao::launcher::entity_provider_publication::
        TopmostPublicationStatus::degraded_authority_unavailable;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, true,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    const auto& topmost = published_root(log.calls[0], "Control").children[0];
    CHECK(topmost.name == "置顶: DEGRADED (平台 authority 不可观测)");
    CHECK_FALSE(topmost.can_activate);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider root publication failure leaves routes unchanged",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("baseline-provider", "baseline-action");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
    const auto baseline = snapshot(store);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 7, "category", "Category", "C", 0.0, "Candidate", "A",
                    "candidate-action", "{}");
    fixture.finish(22);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.publication_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, true, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(log.calls.size() == 2);
    CHECK(published_root(log.calls[0], "Plugins").children.back().name == "Candidate");
    CHECK(snapshot(store) == baseline);
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity publication failure replaces active rows with fail-closed roots",
          "[launcher][entity_provider][publication][fail_closed][focused]") {
    EntityActionRouteStore store;
    auto baseline_route = make_route("baseline-provider", "baseline-action");
    baseline_route.provider_generation = 1;
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 7, "category", "Category", "C", 0.0, "Candidate", "A",
                    "candidate-action", "{}");
    fixture.finish(27);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.publication_statuses = {SAO_STATUS_ERR_OS_CALL_FAILED, SAO_STATUS_OK};
    g_publication_log = &log;
    EntityProviderPublicationState state;
    state.builtin_authority.topmost = true;
    state.builtin_authority.nervgear = true;
    state.builtin_authority.streaming = true;
    state.builtin_authority.save_settings = true;
    state.builtin_authority.ai_editor = true;
    state.builtin_authority.reload_plugins = true;
    state.builtin_authority.theme = true;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, true, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(log.calls.size() == 2);
    const auto& fail_closed = log.calls.back();
    CHECK(std::none_of(fail_closed.begin(), fail_closed.end(),
                       [](const auto& root) { return root.can_activate; }));
    for (const auto& root : fail_closed) {
        CHECK(std::none_of(root.children.begin(), root.children.end(),
                           [](const auto& row) { return row.can_activate; }));
    }
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity root contributions append stable sorted roots and survive clear",
          "[launcher][entity_provider][root_contribution][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(3);
    fixture.set_row(0, "provider-c", 3, "category-c", "Category C", "C", 2.0, "Third", "3",
                    "action-c", "{}");
    fixture.set_row(1, "provider-a", 1, "category-a", "Category A", "A", 0.0, "First", "1",
                    "action-a", "{}");
    fixture.set_row(2, "provider-b", 2, "category-b", "Category B", "B", 1.0, "Second", "2",
                    "action-b", "{}");
    fixture.finish(31);
    g_catalog_fixture = &fixture;

    EntityProviderPublicationState state;
    std::vector<EntityRootContributionSpec> contributions{
        {"owner-c", "menu-c", "dynamic:c", "Dynamic C", "C", 20.0, {{"provider-c", "action-c"}}},
        {"owner-b", "menu-b", "dynamic:b", "Dynamic B", "B", 10.0, {{"provider-b", "action-b"}}},
        {"owner-a", "menu-a", "dynamic:a", "Dynamic A", "A", 10.0, {{"provider-a", "action-a"}}},
    };
    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions(
                state, contributions) == SAO_STATUS_OK);
    CHECK(state.root_contribution_revision == 1);
    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions(
                state, contributions) == SAO_STATUS_OK);
    CHECK(state.root_contribution_revision == 1);

    PublicationLog log;
    g_publication_log = &log;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, true,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    REQUIRE(log.calls[0].size() == 8);
    CHECK(log.calls[0][5].id == "dynamic:a");
    CHECK(log.calls[0][6].id == "dynamic:b");
    CHECK(log.calls[0][7].id == "dynamic:c");
    REQUIRE(log.calls[0][5].children.size() == 1);
    REQUIRE(log.calls[0][6].children.size() == 1);
    REQUIRE(log.calls[0][7].children.size() == 1);
    const auto routes = snapshot(store);
    CHECK(log.calls[0][5].children[0].action_id == token_for(routes, "provider-a", "action-a"));
    CHECK(log.calls[0][6].children[0].action_id == token_for(routes, "provider-b", "action-b"));
    CHECK(log.calls[0][7].children[0].action_id == token_for(routes, "provider-c", "action-c"));
    const auto& plugin_rows = published_root(log.calls[0], "Plugins").children;
    REQUIRE(plugin_rows.size() == 3);
    CHECK(plugin_rows[2].name == "无已启用面板插件");

    REQUIRE(sao::launcher::entity_provider_publication::clear(shell, store, state, true,
                                                              &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[1].size() == 5);
    CHECK(state.root_contributions.size() == 3);
    CHECK(state.root_contribution_revision == 1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, true,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 3);
    CHECK(log.calls[2].size() == 8);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Loader root catalog projects into D1 and deduplicates Plugins routes",
          "[launcher][entity_provider][root_contribution][loader][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "python/menu", 51, "python-tools", "Python Tools", "P", 10.5, "Run", "▶",
                    "run", R"({"value":1})");
    fixture.set_root("python", "menu-tools", "plugin:python-tools", "工具 α", "⚙", 10.5, 0);
    fixture.finish(41);
    g_catalog_fixture = &fixture;

    EntityProviderPublicationState state;
    PublicationLog log;
    g_publication_log = &log;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    REQUIRE(log.calls[0].size() == 6);
    const auto& dynamic = published_root(log.calls[0], "plugin:python-tools");
    CHECK(dynamic.name == "工具 α");
    CHECK(dynamic.icon == "⚙");
    REQUIRE(dynamic.children.size() == 1);
    const auto routes = snapshot(store);
    CHECK(dynamic.children[0].action_id == token_for(routes, "python/menu", "run"));
    const auto& plugins = published_root(log.calls[0], "Plugins").children;
    REQUIRE(plugins.size() == 3);
    CHECK(plugins[2].name == "无已启用面板插件");
    CHECK(state.root_contributions.empty());

    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity root contribution validation is atomic",
          "[launcher][entity_provider][root_contribution][focused]") {
    EntityProviderPublicationState state;
    const EntityRootContributionSpec baseline{
        "owner", "menu", "dynamic:menu", "Dynamic Menu", "M", 0.0, {{"provider", "action"}}};
    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions(
                state, {baseline}) == SAO_STATUS_OK);
    const auto before = state.root_contributions;
    const auto revision = state.root_contribution_revision;

    auto duplicate_name = baseline;
    duplicate_name.owner_id = "other-owner";
    duplicate_name.contribution_id = "other-menu";
    duplicate_name.root_id = "dynamic:other";
    CHECK(sao::launcher::entity_provider_publication::replace_root_contributions(
              state, {baseline, duplicate_name}) == SAO_STATUS_ERR_ALREADY_EXISTS);
    CHECK(state.root_contributions == before);
    CHECK(state.root_contribution_revision == revision);

    auto duplicate_action = baseline;
    duplicate_action.contribution_id = "second-menu";
    duplicate_action.root_id = "dynamic:second";
    duplicate_action.name = "Second Menu";
    CHECK(sao::launcher::entity_provider_publication::replace_root_contributions(
              state, {baseline, duplicate_action}) == SAO_STATUS_ERR_ALREADY_EXISTS);
    CHECK(state.root_contributions == before);

    auto reserved = baseline;
    reserved.root_id = "Plugins";
    CHECK(sao::launcher::entity_provider_publication::replace_root_contributions(
              state, {reserved}) == SAO_STATUS_ERR_ALREADY_EXISTS);
    CHECK(state.root_contributions == before);

    auto nan_priority = baseline;
    nan_priority.priority = std::numeric_limits<double>::quiet_NaN();
    CHECK(sao::launcher::entity_provider_publication::replace_root_contributions(
              state, {nan_priority}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(state.root_contributions == before);
    CHECK(state.root_contribution_revision == revision);
}

TEST_CASE("Entity root contribution owners replace and clear independently",
          "[launcher][entity_provider][root_contribution][focused]") {
    EntityProviderPublicationState state;
    const EntityRootContributionSpec owner_a{
        "owner-a", "menu-a", "dynamic:a", "Dynamic A", "A", 10.0, {{"provider-a", "action-a"}}};
    const EntityRootContributionSpec owner_b{
        "owner-b", "menu-b", "dynamic:b", "Dynamic B", "B", 20.0, {{"provider-b", "action-b"}}};

    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions_for_owner(
                state, "owner-a", {owner_a}) == SAO_STATUS_OK);
    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions_for_owner(
                state, "owner-b", {owner_b}) == SAO_STATUS_OK);
    REQUIRE(state.root_contributions.size() == 2);
    CHECK(state.root_contribution_revision == 2);

    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions_for_owner(
                state, "owner-a", {owner_a}) == SAO_STATUS_OK);
    CHECK(state.root_contribution_revision == 2);
    REQUIRE(sao::launcher::entity_provider_publication::clear_root_contributions_for_owner(
                state, "owner-a") == SAO_STATUS_OK);
    REQUIRE(state.root_contributions.size() == 1);
    CHECK(state.root_contributions[0] == owner_b);
    CHECK(state.root_contribution_revision == 3);
    REQUIRE(sao::launcher::entity_provider_publication::clear_root_contributions_for_owner(
                state, "owner-a") == SAO_STATUS_OK);
    CHECK(state.root_contribution_revision == 3);
}

TEST_CASE("Entity root contribution with a missing route aborts publication",
          "[launcher][entity_provider][root_contribution][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("baseline-provider", "baseline-action");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 8, "category", "Category", "C", 0.0, "Action", "A", "action",
                    "{}");
    fixture.finish(32);
    g_catalog_fixture = &fixture;

    EntityProviderPublicationState state;
    REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions(
                state, {{"owner",
                         "menu",
                         "dynamic:missing",
                         "Missing",
                         "M",
                         0.0,
                         {{"provider", "missing-action"}}}}) == SAO_STATUS_OK);
    PublicationLog log;
    g_publication_log = &log;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, true, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_NOT_FOUND);
    REQUIRE(log.calls.size() == 1);
    CHECK(std::none_of(log.calls[0].begin(), log.calls[0].end(),
                       [](const auto& root) { return root.can_activate; }));
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity root projection enforces the complete tree child budget",
          "[launcher][entity_provider][root_contribution][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("baseline-provider", "baseline-action");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);

    constexpr std::size_t kRouteCount = 254;
    CatalogFixture fixture;
    fixture.reset(kRouteCount);
    for (std::size_t index = 0; index < kRouteCount; ++index) {
        fixture.set_row(index, "provider-" + std::to_string(index), index + 1, "category",
                        "Category", "C", 0.0, "Row " + std::to_string(index), "R",
                        "action-" + std::to_string(index), "{}");
    }
    fixture.finish(33);
    g_catalog_fixture = &fixture;

    EntityProviderPublicationState state;
    PublicationLog log;
    g_publication_log = &log;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(log.calls.size() == 1);
    CHECK(std::none_of(log.calls[0].begin(), log.calls[0].end(),
                       [](const auto& root) { return root.can_activate; }));
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity Plugins projection accepts its exact 256-child envelope",
          "[launcher][entity_provider][budget][focused]") {
    constexpr std::size_t kRouteCount = 253;
    CatalogFixture fixture;
    fixture.reset(kRouteCount);
    for (std::size_t index = 0; index < kRouteCount; ++index) {
        fixture.set_row(index, "provider-" + std::to_string(index), index + 1, "category",
                        "Category", "C", 0.0, "Row " + std::to_string(index), "R",
                        "action-" + std::to_string(index), "{}");
    }
    fixture.finish(34);
    g_catalog_fixture = &fixture;

    EntityActionRouteStore store;
    EntityProviderPublicationState state;
    PublicationLog log;
    g_publication_log = &log;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    CHECK(published_root(log.calls[0], "Plugins").children.size() == 256);
    CHECK(snapshot(store).routes.size() == kRouteCount);
    CHECK(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity publication preflights the exact complete-tree child envelope",
          "[launcher][entity_provider][root_contribution][budget][focused]") {
    CatalogFixture fixture;
    EntityProviderPublicationState state;
    std::uint64_t revision = 40;
    const auto configure_candidate = [&](std::size_t route_count) {
        fixture.reset(route_count);
        std::vector<EntityRootContributionSpec> contributions;
        contributions.reserve((route_count + 255) / 256);
        for (std::size_t index = 0; index < route_count; ++index) {
            fixture.set_row(index, "provider-" + std::to_string(index), index + 1, "category",
                            "Category", "C", 0.0, "Row " + std::to_string(index), "R",
                            "action-" + std::to_string(index), "{}");
            if (index % 256 == 0) {
                const auto root_index = contributions.size();
                contributions.push_back({
                    "owner-" + std::to_string(root_index),
                    "contribution-" + std::to_string(root_index),
                    "dynamic:" + std::to_string(root_index),
                    "Dynamic " + std::to_string(root_index),
                    "D",
                    static_cast<double>(root_index),
                    {},
                });
                contributions.back().actions.reserve(256);
            }
            contributions.back().actions.push_back(
                {fixture.provider_ids[index], fixture.action_ids[index]});
        }
        fixture.finish(revision++);
        REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions(
                    state, contributions) == SAO_STATUS_OK);
    };

    configure_candidate(1008);
    g_catalog_fixture = &fixture;
    EntityActionRouteStore store;
    PublicationLog log;
    g_publication_log = &log;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
    REQUIRE(sao::launcher::entity_provider_publication::refresh(shell, store, state, false,
                                                                &fake_catalog_snapshot,
                                                                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
    CHECK(snapshot(store).routes.size() == 1008);
    CHECK(state.has_catalog_revision);
    CHECK(std::accumulate(log.calls[0].begin(), log.calls[0].end(), std::size_t{0},
                          [](std::size_t total, const auto& root) {
                              return total + root.children.size();
                          }) == 1024);

    configure_candidate(1009);
    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, false, &fake_catalog_snapshot, &fake_set_roots) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    REQUIRE(log.calls.size() == 2);
    CHECK(std::none_of(log.calls[1].begin(), log.calls[1].end(),
                       [](const auto& root) { return root.can_activate; }));
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity action route removal leaves stale tokens reserved",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto first = make_route("provider", "first");
    const auto removed = make_route("provider", "removed");
    REQUIRE(store.publish({first, removed}) == SAO_STATUS_OK);
    const auto initial = snapshot(store);
    const auto first_token = token_for(initial, "provider", "first");
    const auto removed_token = token_for(initial, "provider", "removed");

    REQUIRE(store.publish({first}) == SAO_STATUS_OK);
    EntityActionRoute resolved;
    CHECK(store.resolve(removed_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(store.resolve(first_token, resolved) == SAO_STATUS_OK);

    const auto replacement = make_route("provider", "replacement");
    REQUIRE(store.publish({first, replacement}) == SAO_STATUS_OK);
    const auto replaced = snapshot(store);
    const auto replacement_token = token_for(replaced, "provider", "replacement");
    CHECK(replacement_token > removed_token);
    CHECK(replacement_token != removed_token);

    REQUIRE(store.publish({}) == SAO_STATUS_OK);
    CHECK(snapshot(store).routes.empty());
    CHECK(store.resolve(first_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(store.resolve(replacement_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);

    REQUIRE(store.publish({first}) == SAO_STATUS_OK);
    const auto republished = snapshot(store);
    CHECK(token_for(republished, "provider", "first") > replacement_token);
    CHECK(store.resolve(first_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);
}

TEST_CASE("Entity action route publish rejects invalid candidates atomically",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("provider", "baseline");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
    const auto baseline = snapshot(store);

    auto duplicate = baseline_route;
    duplicate.row_label = "Duplicate identity";
    CHECK(store.publish({baseline_route, duplicate}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    auto empty_provider = make_route("", "action");
    CHECK(store.publish({empty_provider}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    auto empty_action = make_route("provider", "");
    CHECK(store.publish({empty_action}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    auto invalid_utf8 = make_route("provider", "invalid-utf8");
    invalid_utf8.row_label = std::string{"\xc0\x80", 2};
    CHECK(store.publish({invalid_utf8}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    auto embedded_nul = make_route("provider", "embedded-nul");
    embedded_nul.category_id = std::string{"category\0suffix", 15};
    CHECK(store.publish({embedded_nul}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    auto oversized_string = make_route("provider", "oversized-string");
    oversized_string.category_label.assign(
        sao::launcher::entity_action_routes::kMaximumStringBytes + 1, 'x');
    CHECK(store.publish({oversized_string}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    std::vector<EntityActionRouteSpec> oversized_rows(
        sao::launcher::entity_action_routes::kMaximumRows + 1);
    CHECK(store.publish(oversized_rows) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    std::vector<EntityActionRouteSpec> oversized_snapshot;
    oversized_snapshot.reserve(900);
    for (std::size_t index = 0; index < 900; ++index) {
        auto route =
            make_route("provider-" + std::to_string(index), "action-" + std::to_string(index));
        route.category_id.assign(1024, 'c');
        route.category_label.assign(1024, 'l');
        route.category_icon.assign(1024, 'i');
        route.row_label.assign(1024, 'r');
        route.row_icon.assign(1024, 'o');
        oversized_snapshot.push_back(std::move(route));
    }
    CHECK(store.publish(oversized_snapshot) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    const auto after_failures = make_route("provider", "after-failures");
    REQUIRE(store.publish({baseline_route, after_failures}) == SAO_STATUS_OK);
    const auto committed = snapshot(store);
    CHECK(token_for(committed, "provider", "after-failures") ==
          token_for(baseline, "provider", "baseline") + 1);
}

TEST_CASE("Entity action route token exhaustion rolls back publication",
          "[launcher][entity_action_routes][focused]") {
    using namespace sao::launcher::entity_action_routes;
    EntityActionRouteStore store(kFirstDynamicToken, kFirstDynamicToken + 1);
    const auto first = make_route("provider", "first");
    const auto second = make_route("provider", "second");
    REQUIRE(store.publish({first, second}) == SAO_STATUS_OK);
    REQUIRE(store.publish({first}) == SAO_STATUS_OK);
    const auto before_exhaustion = snapshot(store);

    const auto third = make_route("provider", "third");
    CHECK(store.publish({first, third}) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    check_rollback(store, before_exhaustion);

    EntityActionRoute resolved;
    CHECK(store.resolve(kFirstDynamicToken + 1, resolved) == SAO_STATUS_ERR_NOT_FOUND);
}

TEST_CASE("Entity action route publication is invisible until commit",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("provider", "baseline");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
    const auto baseline = snapshot(store);

    const auto candidate_route = make_route("provider", "candidate");
    PreparedEntityActionRoutePublication publication;
    REQUIRE(store.prepare({baseline_route, candidate_route}, publication) == SAO_STATUS_OK);
    REQUIRE(publication.valid());
    CHECK(publication.changed());

    EntityActionRouteSnapshot candidate;
    REQUIRE(publication.snapshot(candidate) == SAO_STATUS_OK);
    REQUIRE(candidate.revision == baseline.revision + 1);
    REQUIRE(candidate.routes.size() == 2);
    const auto candidate_token = token_for(candidate, "provider", "candidate");

    CHECK(snapshot(store) == baseline);
    EntityActionRoute resolved;
    CHECK(store.resolve(candidate_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);

    REQUIRE(publication.commit() == SAO_STATUS_OK);
    CHECK_FALSE(publication.valid());
    CHECK(snapshot(store) == candidate);
    REQUIRE(store.resolve(candidate_token, resolved) == SAO_STATUS_OK);
    CHECK(resolved.action_id == "candidate");
    CHECK(publication.commit() == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("Entity action route abort restores exact state and token cursor",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("provider", "baseline");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
    const auto baseline = snapshot(store);

    const auto candidate_route = make_route("provider", "candidate");
    std::int32_t aborted_token = 0;
    {
        PreparedEntityActionRoutePublication publication;
        REQUIRE(store.prepare({baseline_route, candidate_route}, publication) == SAO_STATUS_OK);
        EntityActionRouteSnapshot candidate;
        REQUIRE(publication.snapshot(candidate) == SAO_STATUS_OK);
        aborted_token = token_for(candidate, "provider", "candidate");
        publication.abort();
        CHECK_FALSE(publication.valid());
    }

    CHECK(snapshot(store) == baseline);
    EntityActionRoute resolved;
    CHECK(store.resolve(aborted_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);

    PreparedEntityActionRoutePublication retry;
    REQUIRE(store.prepare({baseline_route, candidate_route}, retry) == SAO_STATUS_OK);
    EntityActionRouteSnapshot retried;
    REQUIRE(retry.snapshot(retried) == SAO_STATUS_OK);
    CHECK(token_for(retried, "provider", "candidate") == aborted_token);
    REQUIRE(retry.commit() == SAO_STATUS_OK);
}

TEST_CASE("Entity action route prepared semantic no-op preserves revision",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto route = make_route("provider", "action");
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    const auto baseline = snapshot(store);

    PreparedEntityActionRoutePublication publication;
    REQUIRE(store.prepare({route}, publication) == SAO_STATUS_OK);
    REQUIRE(publication.valid());
    CHECK_FALSE(publication.changed());
    EntityActionRouteSnapshot candidate;
    REQUIRE(publication.snapshot(candidate) == SAO_STATUS_OK);
    CHECK(candidate == baseline);
    REQUIRE(publication.commit() == SAO_STATUS_OK);
    CHECK(snapshot(store) == baseline);
}

TEST_CASE("Entity action route transaction rejects commit after store teardown",
          "[launcher][entity_action_routes][focused]") {
    PreparedEntityActionRoutePublication publication;
    {
        EntityActionRouteStore store;
        const auto route = make_route("provider", "action");
        REQUIRE(store.prepare({route}, publication) == SAO_STATUS_OK);
        REQUIRE(publication.valid());
    }

    CHECK(publication.commit() == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK_FALSE(publication.valid());
}

TEST_CASE("Entity action route transaction rejects stale prepared candidate",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto baseline = make_route("provider", "baseline");
    REQUIRE(store.publish({baseline}) == SAO_STATUS_OK);

    PreparedEntityActionRoutePublication stale;
    const auto stale_row = make_route("provider", "stale");
    REQUIRE(store.prepare({baseline, stale_row}, stale) == SAO_STATUS_OK);

    const auto winner = make_route("provider", "winner");
    REQUIRE(store.publish({baseline, winner}) == SAO_STATUS_OK);
    const auto committed = snapshot(store);

    CHECK(stale.commit() == SAO_STATUS_ERR_CANCELLED);
    CHECK_FALSE(stale.valid());
    CHECK(snapshot(store) == committed);
    CHECK(token_for(committed, "provider", "winner") ==
          token_for(committed, "provider", "baseline") + 1);
}
