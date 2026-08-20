#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"

#include "../src/plugin_manager_panel_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace Panel = sao::launcher::plugin_manager_panel;

const Json* find_action(const Json& value, std::string_view action) {
    if (value.is_object()) {
        const auto found = value.find("action");
        if (found != value.end() && found->is_string() &&
            found->get_ref<const std::string&>() == action) {
            return &value;
        }
        for (const auto& [key, child] : value.items()) {
            (void)key;
            if (const Json* match = find_action(child, action); match != nullptr)
                return match;
        }
    } else if (value.is_array()) {
        for (const Json& child : value) {
            if (const Json* match = find_action(child, action); match != nullptr)
                return match;
        }
    }
    return nullptr;
}

std::size_t compositor_layer_count(sao_ui_compositor_handle_t compositor) {
    std::size_t count = 0;
    REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK);
    return count;
}

bool wait_until(const std::function<bool()>& predicate, int attempts = 1000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

Panel::PluginSnapshot fixture_plugin() {
    return {
        .plugin_id = "fixture.plugin-id",
        .name = "Fixture Plugin",
        .version = "1.2.3",
        .language = "lua",
        .description = "Focused plugin description",
        .source_path = "C:/fixtures/plugin.json",
        .state = Panel::PluginState::loaded_active,
        .source = Panel::PluginSource::user,
        .manifest_enabled = true,
    };
}

Panel::Operations fixture_operations(std::atomic<int>& snapshot_calls) {
    Panel::Operations operations;
    operations.snapshot = [&] {
        snapshot_calls.fetch_add(1);
        Panel::Snapshot snapshot;
        snapshot.loader_available = true;
        snapshot.plugins.push_back(fixture_plugin());
        return snapshot;
    };
    return operations;
}

} // namespace

TEST_CASE("Plugin Manager JSON spec carries ids, gates busy actions, and enforces budget",
          "[launcher][plugin_manager][json][spec][budget][focused]") {
    Panel::Snapshot snapshot;
    snapshot.loader_available = true;
    snapshot.reload_all_available = true;
    snapshot.busy = true;
    snapshot.plugins.push_back(fixture_plugin());

    Json spec = Json::parse(Panel::build_spec_for_testing(snapshot));
    const Json* disable = find_action(spec, "plugin_manager.disable");
    REQUIRE(disable != nullptr);
    CHECK((*disable)["payload"] == "fixture.plugin-id");
    CHECK(disable->value("disabled", false));
    const Json* reload_all = find_action(spec, "plugin_manager.reload_all");
    REQUIRE(reload_all != nullptr);
    CHECK(reload_all->value("disabled", false));
    CHECK(spec.dump().find('#') == std::string::npos);

    snapshot.busy = false;
    spec = Json::parse(Panel::build_spec_for_testing(snapshot));
    disable = find_action(spec, "plugin_manager.disable");
    REQUIRE(disable != nullptr);
    CHECK_FALSE(disable->value("disabled", false));

    snapshot.plugins.clear();
    for (int index = 0; index < 1200; ++index) {
        Panel::PluginSnapshot plugin = fixture_plugin();
        plugin.plugin_id = "fixture." + std::to_string(index);
        plugin.name.assign(512U, 'N');
        plugin.description.assign(2048U, 'D');
        plugin.source_path.assign(2048U, 'P');
        snapshot.plugins.push_back(std::move(plugin));
    }
    const std::string bounded = Panel::build_spec_for_testing(snapshot);
    CHECK(bounded.size() <= 256U * 1024U);
    const Json compact = Json::parse(bounded);
    CHECK(compact.dump().find("exceeded the panel budget") != std::string::npos);
}

TEST_CASE("Plugin Manager workerizes loader mutations and reuses one panel",
          "[launcher][plugin_manager][worker][lifecycle][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    std::atomic<int> snapshot_calls{};
    std::atomic<int> enabled{};
    std::atomic<int> disabled{};
    std::atomic<int> reloaded{};
    std::atomic<int> reload_all{};
    std::atomic<bool> ids_valid{true};
    Panel::Operations operations = fixture_operations(snapshot_calls);
    operations.enable = [&](std::string_view id) {
        if (id != "fixture.plugin-id")
            ids_valid.store(false);
        enabled.fetch_add(1);
        return SAO_STATUS_OK;
    };
    operations.disable = [&](std::string_view id) {
        if (id != "fixture.plugin-id")
            ids_valid.store(false);
        disabled.fetch_add(1);
        return SAO_STATUS_OK;
    };
    operations.reload = [&](std::string_view id) {
        if (id != "fixture.plugin-id")
            ids_valid.store(false);
        reloaded.fetch_add(1);
        return SAO_STATUS_OK;
    };
    operations.reload_all = [&] {
        reload_all.fetch_add(1);
        return SAO_STATUS_OK;
    };
    Panel::Owner owner(compositor, std::move(operations));

    REQUIRE(owner.open() == SAO_STATUS_OK);
    const sao_ui_panel_handle_t first = owner.panel_handle();
    REQUIRE(first != nullptr);
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.enable",
                                              "\"fixture.plugin-id\"") == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.disable",
                                              "\"fixture.plugin-id\"") == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.reload",
                                              "\"fixture.plugin-id\"") == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.reload_all") == SAO_STATUS_OK);
    REQUIRE(wait_until([&] {
        return enabled.load() == 1 && disabled.load() == 1 && reloaded.load() == 1 &&
               reload_all.load() == 1;
    }));
    REQUIRE(wait_until([&] { return snapshot_calls.load() >= 5; }));
    CHECK(ids_valid.load());

    REQUIRE(owner.close() == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_visible());
    sao_status_t reopen_status = SAO_UI_PANEL_STATUS_ERR_BUSY;
    REQUIRE(wait_until([&] {
        reopen_status = owner.open();
        return reopen_status != SAO_UI_PANEL_STATUS_ERR_BUSY;
    }));
    REQUIRE(reopen_status == SAO_STATUS_OK);
    CHECK(owner.panel_handle() == first);
    CHECK(compositor_layer_count(compositor) == 1);

    sao_status_t offline_status = SAO_UI_PANEL_STATUS_ERR_BUSY;
    REQUIRE(wait_until([&] {
        offline_status = owner.take_offline();
        return offline_status != SAO_UI_PANEL_STATUS_ERR_BUSY;
    }));
    REQUIRE(offline_status == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_registered());
    CHECK(compositor_layer_count(compositor) == 0);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager loader worker keeps retirement retryable while busy",
          "[launcher][plugin_manager][worker][concurrency][teardown][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    std::atomic<int> snapshot_calls{};
    std::atomic<bool> entered{};
    std::atomic<bool> release{};
    Panel::Operations operations = fixture_operations(snapshot_calls);
    operations.enable = [&](std::string_view) {
        entered.store(true);
        while (!release.load())
            std::this_thread::sleep_for(1ms);
        return SAO_STATUS_OK;
    };
    Panel::Owner owner(compositor, std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);

    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.enable",
                                              "\"fixture.plugin-id\"") == SAO_STATUS_OK);
    REQUIRE(wait_until([&] { return entered.load(); }));
    CHECK(owner.take_offline() == SAO_UI_PANEL_STATUS_ERR_BUSY);
    CHECK(owner.is_registered());

    release.store(true);
    REQUIRE(wait_until([&] { return snapshot_calls.load() >= 2; }));
    sao_status_t retirement = SAO_UI_PANEL_STATUS_ERR_BUSY;
    REQUIRE(wait_until([&] {
        retirement = owner.take_offline();
        return retirement != SAO_UI_PANEL_STATUS_ERR_BUSY;
    }));
    CHECK(retirement == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager refresh worker rejects foreign-thread teardown",
          "[launcher][plugin_manager][worker][owner_thread][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    Panel::Owner* owner_address = nullptr;
    std::atomic<int> snapshot_calls{};
    std::atomic<bool> reenter{};
    std::atomic<sao_status_t> nested_status{SAO_STATUS_OK};
    Panel::Operations operations;
    operations.snapshot = [&] {
        snapshot_calls.fetch_add(1);
        if (reenter.load() && owner_address != nullptr)
            nested_status.store(owner_address->take_offline());
        Panel::Snapshot snapshot;
        snapshot.loader_available = true;
        snapshot.plugins.push_back(fixture_plugin());
        return snapshot;
    };
    Panel::Owner owner(compositor, std::move(operations));
    owner_address = &owner;
    REQUIRE(owner.open() == SAO_STATUS_OK);

    REQUIRE(wait_until([&] { return snapshot_calls.load() >= 1; }));
    reenter.store(true);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.refresh") == SAO_STATUS_OK);
    REQUIRE(wait_until([&] { return snapshot_calls.load() >= 2; }));
    CHECK(nested_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(owner.is_registered());

    sao_status_t offline_status = SAO_UI_PANEL_STATUS_ERR_BUSY;
    REQUIRE(wait_until([&] {
        offline_status = owner.take_offline();
        return offline_status != SAO_UI_PANEL_STATUS_ERR_BUSY;
    }));
    REQUIRE(offline_status == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager retirement rolls back and succeeds on retry",
          "[launcher][plugin_manager][retirement][rollback][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    std::atomic<int> snapshot_calls{};
    Panel::Owner owner(compositor, fixture_operations(snapshot_calls));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    const sao_ui_panel_handle_t panel = owner.panel_handle();

    owner.fail_next_unregister_for_testing(SAO_STATUS_ERR_OS_CALL_FAILED);
    sao_status_t first_retirement = SAO_UI_PANEL_STATUS_ERR_BUSY;
    REQUIRE(wait_until([&] {
        first_retirement = owner.take_offline();
        return first_retirement != SAO_UI_PANEL_STATUS_ERR_BUSY;
    }));
    CHECK(first_retirement == SAO_STATUS_ERR_OS_CALL_FAILED);
    CHECK(owner.is_registered());
    CHECK(owner.panel_handle() == panel);
    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_registered());
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager rejects malformed JSON and foreign-thread mutations",
          "[launcher][plugin_manager][json][owner_thread][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    std::atomic<int> snapshot_calls{};
    Panel::Owner owner(compositor, fixture_operations(snapshot_calls));
    REQUIRE(owner.open() == SAO_STATUS_OK);

    CHECK(owner.dispatch_action_for_testing("plugin_manager.enable", "{") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(owner.dispatch_action_for_testing("plugin_manager.enable", R"({"id":"x"})") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const std::string oversized_id(257U, 'x');
    CHECK(owner.dispatch_action_for_testing("plugin_manager.enable",
                                            Json(oversized_id).dump()) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    std::string embedded_nul = "\"fixture.plugin-id\"";
    embedded_nul.push_back('\0');
    CHECK(owner.dispatch_action_for_testing(
              "plugin_manager.enable",
              std::string_view(embedded_nul.data(), embedded_nul.size())) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);
    const std::string oversized_payload(4097U, 'x');
    CHECK(owner.dispatch_action_for_testing("plugin_manager.enable", oversized_payload) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    std::atomic<sao_status_t> open_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> teardown_status{SAO_STATUS_OK};
    std::thread foreign([&] {
        open_status.store(owner.open());
        teardown_status.store(owner.take_offline());
    });
    foreign.join();
    CHECK(open_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(teardown_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(owner.is_registered());

    sao_status_t final_retirement = SAO_UI_PANEL_STATUS_ERR_BUSY;
    REQUIRE(wait_until([&] {
        final_retirement = owner.take_offline();
        return final_retirement != SAO_UI_PANEL_STATUS_ERR_BUSY;
    }));
    REQUIRE(final_retirement == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}
