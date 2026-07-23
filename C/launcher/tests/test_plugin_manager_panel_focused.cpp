#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"

#include "../src/plugin_manager_panel_internal.h"

#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

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
        for (const auto& child : value) {
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

} // namespace

TEST_CASE("Plugin Manager descriptor is a solid movable compositor panel",
          "[launcher][plugin_manager][panel][descriptor][focused]") {
    const SaoPanelDescriptor descriptor = Panel::descriptor_for_testing();

    REQUIRE(descriptor.panel_id_utf8 != nullptr);
    CHECK(std::string_view(descriptor.panel_id_utf8) == Panel::kPanelId);
    REQUIRE(descriptor.title_utf8 != nullptr);
    CHECK(std::string_view(descriptor.title_utf8) == "Plugin Manager");
    CHECK(descriptor.anchor == SAO_UI_PANEL_ANCHOR_CENTER);
    CHECK(descriptor.movable);
    CHECK(descriptor.resizable);
    CHECK(descriptor.show_titlebar);
    CHECK(descriptor.show_close_button);
    CHECK_FALSE(descriptor.visible);
    CHECK_FALSE(descriptor.modal);
    CHECK_FALSE(descriptor.overlay_style);
    CHECK(descriptor.auto_scroll);
    CHECK(descriptor.initial_opacity == 1.0F);
    REQUIRE(descriptor.theme_override_json_utf8 != nullptr);

    const Json theme = Json::parse(descriptor.theme_override_json_utf8);
    CHECK(theme["colors"]["APP_BG"] == "#0b1018");
    CHECK(theme["colors"]["APP_CARD"] == "#131b27");
}

TEST_CASE("Plugin Manager lifecycle helpers map stable UI behavior",
          "[launcher][plugin_manager][state][focused]") {
    CHECK(Panel::plugin_state_label(Panel::PluginState::loaded_active) == "Enabled / 已启用");
    CHECK(Panel::plugin_state_label(Panel::PluginState::failed) == "Failed / 失败");
    CHECK(Panel::plugin_state_is_enabled(Panel::PluginState::loaded_active));
    CHECK_FALSE(Panel::plugin_state_is_enabled(Panel::PluginState::loaded_disabled));

    for (const Panel::PluginState state : {
             Panel::PluginState::validating,
             Panel::PluginState::resolving_dependencies,
             Panel::PluginState::bootstrapping,
             Panel::PluginState::loading,
             Panel::PluginState::unloading,
             Panel::PluginState::enabling,
             Panel::PluginState::disabling,
         }) {
        CHECK(Panel::plugin_state_is_transitioning(state));
        CHECK_FALSE(Panel::plugin_state_allows_reload(state));
    }

    for (const Panel::PluginState state : {
             Panel::PluginState::discovered,
             Panel::PluginState::loaded_disabled,
             Panel::PluginState::unloaded,
             Panel::PluginState::failed,
             Panel::PluginState::loaded_active,
         }) {
        CHECK(Panel::plugin_state_allows_enable(state));
    }
    CHECK_FALSE(Panel::plugin_state_allows_enable(Panel::PluginState::unknown));
    CHECK(Panel::plugin_state_allows_reload(Panel::PluginState::discovered));
    CHECK(Panel::plugin_state_allows_reload(Panel::PluginState::failed));
}

TEST_CASE("Plugin Manager spec exposes unavailable state and injected Reload All gating",
          "[launcher][plugin_manager][spec][unavailable][focused]") {
    Panel::Snapshot snapshot;
    snapshot.error_message = "Plugin loader unavailable / 插件加载器不可用。";

    const Json spec = Json::parse(Panel::build_spec_for_testing(snapshot));
    CHECK(spec["version"] == 1);
    CHECK(spec["surface"] == "solid");
    CHECK(spec.dump().find("Plugin loader unavailable") != std::string::npos);
    CHECK(spec.dump().find("插件加载器不可用") != std::string::npos);

    const Json* reload_all = find_action(spec, "plugin_manager.reload_all");
    REQUIRE(reload_all != nullptr);
    CHECK(reload_all->value("disabled", false));

    snapshot.reload_all_available = true;
    const Json injected = Json::parse(Panel::build_spec_for_testing(snapshot));
    reload_all = find_action(injected, "plugin_manager.reload_all");
    REQUIRE(reload_all != nullptr);
    CHECK_FALSE(reload_all->value("disabled", false));
}

TEST_CASE("Plugin Manager plugin actions carry the plugin id as the payload",
          "[launcher][plugin_manager][spec][payload][focused]") {
    Panel::Snapshot snapshot;
    snapshot.loader_available = true;
    snapshot.reload_all_available = true;
    snapshot.plugins.push_back({
        .plugin_id = "fixture.plugin-id",
        .name = "Fixture Plugin",
        .version = "1.2.3",
        .language = "lua",
        .description = "Focused plugin description",
        .source_path = "C:/fixtures/plugin.json",
        .state = Panel::PluginState::loaded_active,
        .source = Panel::PluginSource::user,
        .manifest_enabled = true,
    });

    const Json spec = Json::parse(Panel::build_spec_for_testing(snapshot));
    CHECK(spec.dump().find("Fixture Plugin") != std::string::npos);
    CHECK(spec.dump().find("1.2.3") != std::string::npos);
    CHECK(spec.dump().find("lua") != std::string::npos);
    CHECK(spec.dump().find("C:/fixtures/plugin.json") != std::string::npos);

    const Json* disable = find_action(spec, "plugin_manager.disable");
    REQUIRE(disable != nullptr);
    REQUIRE(disable->contains("payload"));
    CHECK((*disable)["payload"] == "fixture.plugin-id");

    const Json* reload = find_action(spec, "plugin_manager.reload");
    REQUIRE(reload != nullptr);
    REQUIRE(reload->contains("payload"));
    CHECK((*reload)["payload"] == "fixture.plugin-id");
    CHECK_FALSE(reload->value("disabled", false));
}

TEST_CASE("Plugin Manager production constructor enables injected Reload All exactly once",
          "[launcher][plugin_manager][panel][reload_all][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    REQUIRE(compositor != nullptr);

    int reload_all_calls = 0;
    Panel::Owner owner(compositor, [&] {
        ++reload_all_calls;
        return SAO_STATUS_OK;
    });

    REQUIRE(owner.open() == SAO_STATUS_OK);
    CHECK(reload_all_calls == 0);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.reload_all") == SAO_STATUS_OK);
    CHECK(reload_all_calls == 1);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager headless owner opens once reuses and closes cleanly",
          "[launcher][plugin_manager][panel][headless][lifecycle][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    REQUIRE(compositor != nullptr);
    REQUIRE(compositor_layer_count(compositor) == 0);

    int snapshot_calls = 0;
    int reload_all_calls = 0;
    std::vector<std::string> enabled;
    std::vector<std::string> disabled;
    std::vector<std::string> reloaded;
    Panel::Operations operations;
    operations.snapshot = [&] {
        ++snapshot_calls;
        Panel::Snapshot snapshot;
        snapshot.loader_available = true;
        snapshot.plugins.push_back({
            .plugin_id = "fixture.plugin-id",
            .name = "Fixture Plugin",
            .version = "1.2.3",
            .language = "lua",
            .description = "Injected loader-free snapshot",
            .source_path = "C:/fixtures/plugin.json",
            .state = Panel::PluginState::loaded_active,
            .source = Panel::PluginSource::user,
            .manifest_enabled = true,
        });
        return snapshot;
    };
    operations.enable = [&](std::string_view plugin_id) {
        enabled.emplace_back(plugin_id);
        return SAO_STATUS_OK;
    };
    operations.disable = [&](std::string_view plugin_id) {
        disabled.emplace_back(plugin_id);
        return SAO_STATUS_OK;
    };
    operations.reload = [&](std::string_view plugin_id) {
        reloaded.emplace_back(plugin_id);
        return SAO_STATUS_OK;
    };
    operations.reload_all = [&] {
        ++reload_all_calls;
        return SAO_STATUS_OK;
    };
    Panel::Owner owner(compositor, std::move(operations));

    REQUIRE(owner.open() == SAO_STATUS_OK);
    CHECK(snapshot_calls == 1);
    REQUIRE(owner.is_registered());
    REQUIRE(owner.is_visible());
    const sao_ui_panel_handle_t first = owner.panel_handle();
    REQUIRE(first != nullptr);
    CHECK(compositor_layer_count(compositor) == 1);

    SaoPanelDescriptor descriptor{};
    REQUIRE(sao_ui_panel_get_descriptor(first, &descriptor) == SAO_STATUS_OK);
    CHECK(std::string_view(descriptor.panel_id_utf8) == Panel::kPanelId);
    CHECK(std::string_view(descriptor.title_utf8) == "Plugin Manager");

    REQUIRE(owner.open() == SAO_STATUS_OK);
    CHECK(snapshot_calls == 2);
    CHECK(owner.panel_handle() == first);
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.reload_all") == SAO_STATUS_OK);
    CHECK(reload_all_calls == 1);
    CHECK(snapshot_calls == 3);
    CHECK(owner.panel_handle() == first);

    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.enable", "\"fixture.plugin-id\"") ==
            SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.disable", "\"fixture.plugin-id\"") ==
            SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.reload", "\"fixture.plugin-id\"") ==
            SAO_STATUS_OK);
    CHECK(enabled == std::vector<std::string>{"fixture.plugin-id"});
    CHECK(disabled == std::vector<std::string>{"fixture.plugin-id"});
    CHECK(reloaded == std::vector<std::string>{"fixture.plugin-id"});
    CHECK(snapshot_calls == 6);

    REQUIRE(owner.dispatch_event_for_testing(SAO_UI_PANEL_EVENT_CLOSE) == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_visible());
    CHECK(owner.is_registered());
    CHECK(owner.panel_handle() == first);
    CHECK(compositor_layer_count(compositor) == 1);
    REQUIRE(owner.open() == SAO_STATUS_OK);
    CHECK(owner.is_visible());
    CHECK(owner.panel_handle() == first);
    CHECK(snapshot_calls == 7);

    REQUIRE(owner.close() == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_visible());
    REQUIRE(owner.open() == SAO_STATUS_OK);
    CHECK(owner.is_visible());
    CHECK(owner.panel_handle() == first);
    CHECK(snapshot_calls == 8);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_registered());
    CHECK_FALSE(owner.is_visible());
    CHECK(owner.panel_handle() == nullptr);
    CHECK(compositor_layer_count(compositor) == 0);
    sao_ui_panel_handle_t missing = nullptr;
    CHECK(sao_ui_panel_find_by_id(compositor, Panel::kPanelId.data(), &missing) ==
          SAO_STATUS_ERR_NOT_FOUND);
    CHECK(missing == nullptr);
    CHECK(owner.take_offline() == SAO_STATUS_OK);

    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager rejects foreign-thread lifecycle mutations before state changes",
          "[launcher][plugin_manager][panel][owner_thread][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    REQUIRE(compositor != nullptr);

    Panel::Operations operations;
    operations.snapshot = [] {
        Panel::Snapshot snapshot;
        snapshot.loader_available = true;
        return snapshot;
    };
    Panel::Owner owner(compositor, std::move(operations));
    REQUIRE(owner.open() == SAO_STATUS_OK);
    const sao_ui_panel_handle_t panel = owner.panel_handle();
    REQUIRE(panel != nullptr);

    sao_status_t open_status = SAO_STATUS_OK;
    sao_status_t close_status = SAO_STATUS_OK;
    sao_status_t refresh_status = SAO_STATUS_OK;
    sao_status_t operations_status = SAO_STATUS_OK;
    sao_status_t action_status = SAO_STATUS_OK;
    sao_status_t teardown_status = SAO_STATUS_OK;
    std::thread foreign([&] {
        open_status = owner.open();
        close_status = owner.close();
        refresh_status = owner.refresh();
        operations_status = owner.set_operations({});
        action_status = owner.dispatch_action_for_testing("plugin_manager.refresh");
        teardown_status = owner.take_offline();
    });
    foreign.join();

    CHECK(open_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(close_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(refresh_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(operations_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(action_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(teardown_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(owner.is_registered());
    CHECK(owner.is_visible());
    CHECK(owner.panel_handle() == panel);
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("Plugin Manager callback teardown is BUSY then succeeds on explicit retry",
          "[launcher][plugin_manager][panel][teardown][reentrant][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    REQUIRE(compositor != nullptr);

    Panel::Owner* owner_address = nullptr;
    sao_status_t nested_teardown = SAO_STATUS_OK;
    int snapshot_calls = 0;
    Panel::Operations operations;
    operations.snapshot = [&] {
        ++snapshot_calls;
        Panel::Snapshot snapshot;
        snapshot.loader_available = true;
        snapshot.plugins.push_back({
            .plugin_id = "fixture.plugin-id",
            .name = "Fixture Plugin",
            .version = "1",
            .language = "native",
            .state = Panel::PluginState::loaded_active,
            .source = Panel::PluginSource::built_in,
            .manifest_enabled = true,
        });
        return snapshot;
    };
    operations.reload = [&](std::string_view plugin_id) {
        CHECK(plugin_id == "fixture.plugin-id");
        REQUIRE(owner_address != nullptr);
        nested_teardown = owner_address->take_offline();
        return SAO_STATUS_OK;
    };
    Panel::Owner owner(compositor, std::move(operations));
    owner_address = &owner;

    REQUIRE(owner.open() == SAO_STATUS_OK);
    const sao_ui_panel_handle_t panel = owner.panel_handle();
    REQUIRE(panel != nullptr);
    REQUIRE(owner.dispatch_action_for_testing("plugin_manager.reload", "\"fixture.plugin-id\"") ==
            SAO_STATUS_OK);
    CHECK(nested_teardown == SAO_UI_PANEL_STATUS_ERR_BUSY);
    CHECK(snapshot_calls == 2);
    CHECK(owner.is_registered());
    CHECK(owner.is_visible());
    CHECK(owner.panel_handle() == panel);
    CHECK(compositor_layer_count(compositor) == 1);

    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    CHECK_FALSE(owner.is_registered());
    CHECK(owner.panel_handle() == nullptr);
    CHECK(compositor_layer_count(compositor) == 0);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}
