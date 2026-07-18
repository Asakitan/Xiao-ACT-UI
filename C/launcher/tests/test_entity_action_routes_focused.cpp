#include <catch2/catch_test_macros.hpp>

#include "entity_action_routes_internal.h"
#include "entity_provider_catalog_internal.h"
#include "entity_provider_publication_internal.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using EntityActionRoute =
    sao::launcher::entity_action_routes::EntityActionRoute;
using EntityActionRouteSnapshot =
    sao::launcher::entity_action_routes::EntityActionRouteSnapshot;
using EntityActionRouteSpec =
    sao::launcher::entity_action_routes::EntityActionRouteSpec;
using EntityActionRouteStore =
    sao::launcher::entity_action_routes::EntityActionRouteStore;
using PreparedEntityActionRoutePublication =
    sao::launcher::entity_action_routes::EntityActionRouteStore::PreparedPublication;
using OwnedEntityProviderCatalog =
    sao::launcher::entity_provider_catalog::OwnedEntityProviderCatalog;
using EntityProviderPublicationState =
    sao::launcher::entity_provider_publication::EntityProviderPublicationState;
using EntityRootContributionActionRef =
    sao::launcher::entity_provider_publication::EntityRootContributionActionRef;
using EntityRootContributionSpec =
    sao::launcher::entity_provider_publication::EntityRootContributionSpec;

namespace loader = sao::plugins::loader;

EntityActionRouteSpec make_route(std::string provider_id,
                                 std::string action_id,
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

std::int32_t token_for(const EntityActionRouteSnapshot& value,
                       const std::string& provider_id,
                       const std::string& action_id) {
    const auto found = std::find_if(
        value.routes.begin(), value.routes.end(), [&](const auto& route) {
            return route.provider_id == provider_id &&
                   route.action_id == action_id;
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
    }

    void set_row(std::size_t index, std::string provider_id,
                 std::uint64_t generation, std::string category_id,
                 std::string category_label, std::string category_icon,
                 double category_priority, std::string row_label,
                 std::string row_icon, std::string action_id,
                 std::string payload) {
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
        };
    }
};

CatalogFixture* g_catalog_fixture = nullptr;

std::int32_t SAO_PLUGINS_CALL fake_catalog_snapshot(
    loader::entity_provider_catalog_callback callback, void* user_data) {
    if (g_catalog_fixture == nullptr) return SAO_ERR_NOT_INITIALIZED;
    return callback(&g_catalog_fixture->catalog, user_data);
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
    bool publish_competitor = false;
    sao_status_t publication_status = SAO_STATUS_OK;
    std::vector<std::vector<PublishedRoot>> calls;
};

PublicationLog* g_publication_log = nullptr;

sao_status_t SAO_UI_CALL fake_set_roots(
    sao_ui_entity_shell_handle_t, const SaoUiEntityRootItem* roots,
    std::size_t count) {
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
    if (g_publication_log->publication_status != SAO_STATUS_OK) {
        return g_publication_log->publication_status;
    }
    if (g_publication_log->publish_competitor &&
        g_publication_log->calls.size() == 1 &&
        g_publication_log->competing_store != nullptr) {
        REQUIRE(g_publication_log->competing_store->publish(
                    {g_publication_log->competing_route}) == SAO_STATUS_OK);
    }
    return SAO_STATUS_OK;
}

const PublishedRoot& published_root(const std::vector<PublishedRoot>& roots,
                                    std::string_view id) {
    const auto found = std::find_if(roots.begin(), roots.end(), [&](const auto& root) {
        return root.id == id;
    });
    REQUIRE(found != roots.end());
    return *found;
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

sao_status_t SAO_UI_CALL fake_get_shell_snapshot(
    sao_ui_entity_shell_handle_t, SaoUiEntityShellSnapshot* out) {
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
    if (g_action_log == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    g_action_log->order.push_back("home");
    return g_action_log->home_status;
}

std::int32_t SAO_PLUGINS_CALL fake_invoke(
    const char* provider_id, std::uint64_t generation,
    const char* action_id, const char* payload) {
    if (g_action_log == nullptr) return SAO_ERR_NOT_INITIALIZED;
    g_action_log->order.push_back("invoke");
    g_action_log->provider_id = provider_id;
    g_action_log->generation = generation;
    g_action_log->action_id = action_id;
    g_action_log->payload = payload;
    return g_action_log->invoke_status;
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
    route.payload_json.assign(
        sao::launcher::entity_action_routes::kMaximumPayloadBytes, 'p');
    REQUIRE(store.publish({route}) == SAO_STATUS_OK);
    route.payload_json.push_back('p');
    CHECK(store.publish({route}) == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("Entity provider catalog is deeply copied sorted and collision checked",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(3);
    fixture.set_row(0, "provider-z", 30, "late", "Late", "L", 5.0,
                    "Zulu", "Z", "action-z", R"({"z":1})");
    fixture.set_row(1, "provider-b", 20, "early", "Early", "E", -1.0,
                    "Beta", "B", "action-b", R"({"b":1})");
    fixture.set_row(2, "provider-a", 10, "early", "Early", "E", -1.0,
                    "Alpha", "A", "action-a", R"({"a":1})");
    fixture.finish(77);
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(
                &fake_catalog_snapshot, catalog) == SAO_STATUS_OK);
    fixture.provider_ids[2] = "mutated-source";
    fixture.payloads[2] = "mutated-source";

    std::vector<EntityActionRouteSpec> routes;
    REQUIRE(sao::launcher::entity_provider_catalog::build_routes(
                catalog, routes) == SAO_STATUS_OK);
    REQUIRE(catalog.revision == 77);
    REQUIRE(routes.size() == 3);
    CHECK(routes[0].provider_id == "provider-a");
    CHECK(routes[0].action_id == "action-a");
    CHECK(routes[0].provider_generation == 10);
    CHECK(routes[0].payload_json == R"({"a":1})");
    CHECK(routes[1].provider_id == "provider-b");
    CHECK(routes[2].provider_id == "provider-z");

    catalog.providers[1].rows[0].category_label = "Conflicting";
    const auto before = routes;
    CHECK(sao::launcher::entity_provider_catalog::build_routes(
              catalog, routes) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(routes == before);
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider publication resyncs UI when route commit loses",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    auto baseline = make_route("baseline-provider", "baseline-action",
                               "baseline-category", "Baseline");
    baseline.provider_generation = 1;
    baseline.payload_json = "{}";
    REQUIRE(store.publish({baseline}) == SAO_STATUS_OK);

    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "candidate-provider", 9, "candidate-category",
                    "Candidate Category", "C", 1.0, "Candidate", "A",
                    "candidate-action", R"({"candidate":true})");
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
              shell, store, state, false, &fake_catalog_snapshot,
              &fake_set_roots) == SAO_STATUS_ERR_CANCELLED);
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
    const auto& winner_plugins = published_root(log.calls[1], "Plugins").children;
    REQUIRE(winner_plugins.size() == 4);
    CHECK(winner_plugins[2].name == "Category");
    CHECK(winner_plugins[3].name == "Row");
    CHECK_FALSE(state.has_catalog_revision);

    const auto committed = snapshot(store);
    REQUIRE(committed.routes.size() == 1);
    CHECK(committed.routes[0].provider_id == "winner-provider");
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
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(
                shell, store, state, true, &fake_catalog_snapshot,
                &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 1);
        const auto& plugins = published_root(log.calls[0], "Plugins").children;
        REQUIRE(plugins.size() == 3);
        CHECK(plugins[2].name ==
          "无已启用面板插件 (去 Manage 启用)");
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
    CHECK(sao::launcher::entity_provider_publication::invoke(
              route, shell, &fake_invoke, &fake_get_shell_snapshot,
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
              route, shell, &fake_invoke, &fake_get_shell_snapshot,
              &fake_home) == SAO_STATUS_ERR_HANDLE_INVALID);
        CHECK(log.order ==
            std::vector<std::string>{"invoke", "snapshot", "home"});

    log = {};
    route.keep_menu_open = true;
    log.invoke_status = -32123;
    CHECK(sao::launcher::entity_provider_publication::invoke(
              route, shell, &fake_invoke, &fake_get_shell_snapshot,
              &fake_home) == SAO_STATUS_ERR_SCRIPT_RUNTIME);
    CHECK(log.order == std::vector<std::string>{"invoke"});
    g_action_log = nullptr;
}

TEST_CASE("Entity provider catalog rejects duplicate providers and invalid UTF-8",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(2);
    fixture.set_row(0, "provider", 10, "category-a", "Category A", "A", 0.0,
                    "First", "1", "action-a", "{}");
    fixture.set_row(1, "provider", 11, "category-b", "Category B", "B", 1.0,
                    "Second", "2", "action-b", "{}");
    fixture.finish();
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    CHECK(sao::launcher::entity_provider_catalog::snapshot(
              &fake_catalog_snapshot, catalog) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    fixture.provider_ids[1] = "provider-b";
    fixture.providers[1].provider_id_utf8 = fixture.provider_ids[1].c_str();
    fixture.owner_ids[1] = std::string{"\xc0\x80", 2};
    fixture.providers[1].owner_plugin_id_utf8 = fixture.owner_ids[1].c_str();
    CHECK(sao::launcher::entity_provider_catalog::snapshot(
              &fake_catalog_snapshot, catalog) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    g_catalog_fixture = nullptr;
}

TEST_CASE("Entity provider catalog normalizes null payload to an empty object",
          "[launcher][entity_provider][focused]") {
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 10, "category", "Category", "C", 0.0,
                    "Row", "R", "action", "ignored");
    fixture.rows[0].payload_json_utf8 = nullptr;
    fixture.finish();
    g_catalog_fixture = &fixture;

    OwnedEntityProviderCatalog catalog;
    REQUIRE(sao::launcher::entity_provider_catalog::snapshot(
                &fake_catalog_snapshot, catalog) == SAO_STATUS_OK);
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
              nullptr, store, state, 5, true, nullptr, nullptr) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(state.refresh_elapsed_ms == 17);
}

TEST_CASE("Entity provider publication preserves canonical roots and clear mode",
          "[launcher][entity_provider][focused]") {
    EntityActionRouteStore store;
    CatalogFixture fixture;
    fixture.reset(1);
    fixture.set_row(0, "provider", 5, "category", "Category", "C", 0.0,
                    "Action", "A", "action", "{}");
    fixture.finish(21);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    REQUIRE(sao::launcher::entity_provider_publication::refresh(
                shell, store, state, false, &fake_catalog_snapshot,
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

    REQUIRE(sao::launcher::entity_provider_publication::clear(
                shell, store, state, true, &fake_set_roots) == SAO_STATUS_OK);
    REQUIRE(log.calls.size() == 2);
    CHECK(published_root(log.calls[1], "Control").children[1].name == "NervGear: ON");
    const auto& cleared_plugins = published_root(log.calls[1], "Plugins").children;
    REQUIRE(cleared_plugins.size() == 3);
    CHECK(cleared_plugins[2].name == "无已启用面板插件 (去 Manage 启用)");
    CHECK(snapshot(store).routes.empty());
    CHECK_FALSE(state.has_catalog_revision);
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
    fixture.set_row(0, "candidate-provider", 7, "category", "Category", "C", 0.0,
                    "Candidate", "A", "candidate-action", "{}");
    fixture.finish(22);
    g_catalog_fixture = &fixture;

    PublicationLog log;
    log.publication_status = SAO_STATUS_ERR_OS_CALL_FAILED;
    g_publication_log = &log;
    EntityProviderPublicationState state;
    const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

    CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, true, &fake_catalog_snapshot,
              &fake_set_roots) == SAO_STATUS_ERR_OS_CALL_FAILED);
    REQUIRE(log.calls.size() == 1);
    CHECK(published_root(log.calls[0], "Plugins").children.back().name == "Candidate");
    CHECK(snapshot(store) == baseline);
    CHECK_FALSE(state.has_catalog_revision);
    g_publication_log = nullptr;
    g_catalog_fixture = nullptr;
}

    TEST_CASE("Entity root contributions append stable sorted roots and survive clear",
            "[launcher][entity_provider][root_contribution][focused]") {
        EntityActionRouteStore store;
        CatalogFixture fixture;
        fixture.reset(3);
        fixture.set_row(0, "provider-c", 3, "category-c", "Category C", "C", 2.0,
                  "Third", "3", "action-c", "{}");
        fixture.set_row(1, "provider-a", 1, "category-a", "Category A", "A", 0.0,
                  "First", "1", "action-a", "{}");
        fixture.set_row(2, "provider-b", 2, "category-b", "Category B", "B", 1.0,
                  "Second", "2", "action-b", "{}");
        fixture.finish(31);
        g_catalog_fixture = &fixture;

        EntityProviderPublicationState state;
        std::vector<EntityRootContributionSpec> contributions{
          {"owner-c", "menu-c", "dynamic:c", "Dynamic C", "C", 20.0,
           {{"provider-c", "action-c"}}},
          {"owner-b", "menu-b", "dynamic:b", "Dynamic B", "B", 10.0,
           {{"provider-b", "action-b"}}},
          {"owner-a", "menu-a", "dynamic:a", "Dynamic A", "A", 10.0,
           {{"provider-a", "action-a"}}},
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
        REQUIRE(sao::launcher::entity_provider_publication::refresh(
                shell, store, state, true, &fake_catalog_snapshot,
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
        CHECK(log.calls[0][5].children[0].action_id ==
            token_for(routes, "provider-a", "action-a"));
        CHECK(log.calls[0][6].children[0].action_id ==
            token_for(routes, "provider-b", "action-b"));
        CHECK(log.calls[0][7].children[0].action_id ==
            token_for(routes, "provider-c", "action-c"));
        const auto& plugin_rows = published_root(log.calls[0], "Plugins").children;
        REQUIRE(plugin_rows.size() == 3);
        CHECK(plugin_rows[2].name == "无已启用面板插件 (去 Manage 启用)");

        REQUIRE(sao::launcher::entity_provider_publication::clear(
                shell, store, state, true, &fake_set_roots) == SAO_STATUS_OK);
        REQUIRE(log.calls.size() == 2);
        CHECK(log.calls[1].size() == 5);
        CHECK(state.root_contributions.size() == 3);
        CHECK(state.root_contribution_revision == 1);

        REQUIRE(sao::launcher::entity_provider_publication::refresh(
                shell, store, state, true, &fake_catalog_snapshot,
                &fake_set_roots) == SAO_STATUS_OK);
        REQUIRE(log.calls.size() == 3);
        CHECK(log.calls[2].size() == 8);
        g_publication_log = nullptr;
        g_catalog_fixture = nullptr;
    }

    TEST_CASE("Entity root contribution validation is atomic",
            "[launcher][entity_provider][root_contribution][focused]") {
        EntityProviderPublicationState state;
        const EntityRootContributionSpec baseline{
          "owner", "menu", "dynamic:menu", "Dynamic Menu", "M", 0.0,
          {{"provider", "action"}}};
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
            "owner-a", "menu-a", "dynamic:a", "Dynamic A", "A", 10.0,
            {{"provider-a", "action-a"}}};
        const EntityRootContributionSpec owner_b{
            "owner-b", "menu-b", "dynamic:b", "Dynamic B", "B", 20.0,
            {{"provider-b", "action-b"}}};

        REQUIRE(sao::launcher::entity_provider_publication::
                    replace_root_contributions_for_owner(state, "owner-a", {owner_a}) ==
                SAO_STATUS_OK);
        REQUIRE(sao::launcher::entity_provider_publication::
                    replace_root_contributions_for_owner(state, "owner-b", {owner_b}) ==
                SAO_STATUS_OK);
        REQUIRE(state.root_contributions.size() == 2);
        CHECK(state.root_contribution_revision == 2);

        REQUIRE(sao::launcher::entity_provider_publication::
                    replace_root_contributions_for_owner(state, "owner-a", {owner_a}) ==
                SAO_STATUS_OK);
        CHECK(state.root_contribution_revision == 2);
        REQUIRE(sao::launcher::entity_provider_publication::
                    clear_root_contributions_for_owner(state, "owner-a") == SAO_STATUS_OK);
        REQUIRE(state.root_contributions.size() == 1);
        CHECK(state.root_contributions[0] == owner_b);
        CHECK(state.root_contribution_revision == 3);
        REQUIRE(sao::launcher::entity_provider_publication::
                    clear_root_contributions_for_owner(state, "owner-a") == SAO_STATUS_OK);
        CHECK(state.root_contribution_revision == 3);
    }

    TEST_CASE("Entity root contribution with a missing route aborts publication",
            "[launcher][entity_provider][root_contribution][focused]") {
        EntityActionRouteStore store;
        const auto baseline_route = make_route("baseline-provider", "baseline-action");
        REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
        const auto baseline = snapshot(store);

        CatalogFixture fixture;
        fixture.reset(1);
        fixture.set_row(0, "provider", 8, "category", "Category", "C", 0.0,
                  "Action", "A", "action", "{}");
        fixture.finish(32);
        g_catalog_fixture = &fixture;

        EntityProviderPublicationState state;
        REQUIRE(sao::launcher::entity_provider_publication::replace_root_contributions(
                state,
                {{"owner", "menu", "dynamic:missing", "Missing", "M", 0.0,
                {{"provider", "missing-action"}}}}) == SAO_STATUS_OK);
        PublicationLog log;
        g_publication_log = &log;
        const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);

        CHECK(sao::launcher::entity_provider_publication::refresh(
              shell, store, state, true, &fake_catalog_snapshot,
              &fake_set_roots) == SAO_STATUS_ERR_NOT_FOUND);
        CHECK(log.calls.empty());
        CHECK(snapshot(store) == baseline);
        CHECK_FALSE(state.has_catalog_revision);
        g_publication_log = nullptr;
        g_catalog_fixture = nullptr;
    }

    TEST_CASE("Entity root projection enforces the complete tree child budget",
              "[launcher][entity_provider][root_contribution][focused]") {
        EntityActionRouteStore store;
        const auto baseline_route = make_route("baseline-provider", "baseline-action");
        REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
        const auto baseline = snapshot(store);

        constexpr std::size_t kRouteCount = 254;
        CatalogFixture fixture;
        fixture.reset(kRouteCount);
        for (std::size_t index = 0; index < kRouteCount; ++index) {
            fixture.set_row(index, "provider-" + std::to_string(index), index + 1,
                            "category", "Category", "C", 0.0,
                            "Row " + std::to_string(index), "R",
                            "action-" + std::to_string(index), "{}");
        }
        fixture.finish(33);
        g_catalog_fixture = &fixture;

        EntityProviderPublicationState state;
        PublicationLog log;
        g_publication_log = &log;
        const auto shell = reinterpret_cast<sao_ui_entity_shell_handle_t>(1);
        CHECK(sao::launcher::entity_provider_publication::refresh(
                  shell, store, state, false, &fake_catalog_snapshot,
                  &fake_set_roots) == SAO_STATUS_ERR_INVALID_ARGUMENT);
        CHECK(log.calls.empty());
        CHECK(snapshot(store) == baseline);
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
    const auto replacement_token =
        token_for(replaced, "provider", "replacement");
    CHECK(replacement_token > removed_token);
    CHECK(replacement_token != removed_token);

    REQUIRE(store.publish({}) == SAO_STATUS_OK);
    CHECK(snapshot(store).routes.empty());
    CHECK(store.resolve(first_token, resolved) == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(store.resolve(replacement_token, resolved) ==
          SAO_STATUS_ERR_NOT_FOUND);

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
    CHECK(store.publish({baseline_route, duplicate}) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
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
    CHECK(store.publish({oversized_string}) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    std::vector<EntityActionRouteSpec> oversized_rows(
        sao::launcher::entity_action_routes::kMaximumRows + 1);
    CHECK(store.publish(oversized_rows) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    check_rollback(store, baseline);

    std::vector<EntityActionRouteSpec> oversized_snapshot;
    oversized_snapshot.reserve(900);
    for (std::size_t index = 0; index < 900; ++index) {
        auto route = make_route("provider-" + std::to_string(index),
                                "action-" + std::to_string(index));
        route.category_id.assign(1024, 'c');
        route.category_label.assign(1024, 'l');
        route.category_icon.assign(1024, 'i');
        route.row_label.assign(1024, 'r');
        route.row_icon.assign(1024, 'o');
        oversized_snapshot.push_back(std::move(route));
    }
    CHECK(store.publish(oversized_snapshot) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
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
    CHECK(store.resolve(kFirstDynamicToken + 1, resolved) ==
          SAO_STATUS_ERR_NOT_FOUND);
}

TEST_CASE("Entity action route publication is invisible until commit",
          "[launcher][entity_action_routes][focused]") {
    EntityActionRouteStore store;
    const auto baseline_route = make_route("provider", "baseline");
    REQUIRE(store.publish({baseline_route}) == SAO_STATUS_OK);
    const auto baseline = snapshot(store);

    const auto candidate_route = make_route("provider", "candidate");
    PreparedEntityActionRoutePublication publication;
    REQUIRE(store.prepare({baseline_route, candidate_route}, publication) ==
            SAO_STATUS_OK);
    REQUIRE(publication.valid());
    CHECK(publication.changed());

    EntityActionRouteSnapshot candidate;
    REQUIRE(publication.snapshot(candidate) == SAO_STATUS_OK);
    REQUIRE(candidate.revision == baseline.revision + 1);
    REQUIRE(candidate.routes.size() == 2);
    const auto candidate_token =
        token_for(candidate, "provider", "candidate");

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
        REQUIRE(store.prepare({baseline_route, candidate_route}, publication) ==
                SAO_STATUS_OK);
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
    REQUIRE(store.prepare({baseline_route, candidate_route}, retry) ==
            SAO_STATUS_OK);
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
