#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#ifdef SAO_STATUS_OK
#undef SAO_STATUS_OK
#endif

#include "entity_provider_publication_internal.h"

#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/csharp_host/cs_loader_adapter.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/ui/entity_shell.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef SAO_CSHARP_ENTITY_A_DLL
#error SAO_CSHARP_ENTITY_A_DLL must be provided by CMake
#endif
#ifndef SAO_CSHARP_ENTITY_A_RUNTIMECONFIG
#error SAO_CSHARP_ENTITY_A_RUNTIMECONFIG must be provided by CMake
#endif
#ifndef SAO_CSHARP_ENTITY_B_DLL
#error SAO_CSHARP_ENTITY_B_DLL must be provided by CMake
#endif
#ifndef SAO_CSHARP_ENTITY_B_RUNTIMECONFIG
#error SAO_CSHARP_ENTITY_B_RUNTIMECONFIG must be provided by CMake
#endif

extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_live_context_count(void);

namespace {

namespace fs = std::filesystem;
namespace loader = sao::plugins::loader;
namespace catalog = sao::launcher::entity_provider_catalog;
namespace publication = sao::launcher::entity_provider_publication;
namespace routes = sao::launcher::entity_action_routes;

constexpr std::string_view kPluginId = "csharp_managed_entity_fixture";
constexpr std::string_view kProviderId = "csharp_managed_entity_fixture/managed-menu";
constexpr std::string_view kContributionId = "managed-root";
constexpr std::string_view kRootId = "plugin:csharp-managed";
constexpr std::string_view kActionId = "managed-action";
constexpr std::string_view kCategoryId = "managed-tools";
constexpr std::string_view kReentryTopic = "csharp_entity_reentrant_unload";

constexpr std::uint32_t kMouseMove = 0x0200;
constexpr std::uint32_t kLeftButtonDown = 0x0201;
constexpr std::uint32_t kLeftButtonUp = 0x0202;
constexpr std::uint32_t kMouseWheel = 0x020A;
constexpr std::int32_t kMenuPad = 40;
constexpr std::int32_t kRootSlotHeight = 70;
constexpr std::int32_t kRootSlotCenterX = 75;
constexpr std::int32_t kChildCenterX = 180;
constexpr std::int32_t kChildCenterY = 22;
constexpr std::size_t kManagedLauncherSdkContextCount = 2;

std::string path_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void write_text(const fs::path& path, const std::string& content) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    REQUIRE_FALSE(error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

void copy_required_file(const fs::path& source, const fs::path& destination) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    REQUIRE_FALSE(error);
    REQUIRE(fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error));
    REQUIRE_FALSE(error);
}

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(std::string_view label) {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / (std::string("sao_launcher_csharp_entity_") +
                                             std::string(label) + "_" + std::to_string(suffix));
        std::error_code error;
        REQUIRE(fs::create_directories(root_, error));
        REQUIRE_FALSE(error);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    const fs::path& root() const noexcept {
        return root_;
    }

  private:
    fs::path root_;
};

struct ManagedFixtureSource {
    fs::path dll;
    fs::path runtimeconfig;
    std::string assembly_name;
};

bool managed_fixture_available(const ManagedFixtureSource& fixture_a,
                               const ManagedFixtureSource& fixture_b, std::string& reason) {
    bool hostfxr_available = false;
    const auto host_status =
        sao::plugins::csharp_host::sao_plugins_cshost_is_available(&hostfxr_available);
    if (host_status != SAO_OK) {
        reason = "hostfxr capability probe failed with status " + std::to_string(host_status);
        return false;
    }
    if (!hostfxr_available) {
        reason = "hostfxr runtime capability is unavailable";
        return false;
    }
    for (const auto* fixture : {&fixture_a, &fixture_b}) {
        std::error_code error;
        if (!fs::is_regular_file(fixture->dll, error) || error) {
            reason = "managed fixture DLL is missing: " + path_utf8(fixture->dll);
            return false;
        }
        error.clear();
        if (!fs::is_regular_file(fixture->runtimeconfig, error) || error) {
            reason =
                "managed fixture runtimeconfig is missing: " + path_utf8(fixture->runtimeconfig);
            return false;
        }
    }
    return true;
}

fs::path stage_managed_plugin(const TemporaryDirectory& tree, std::string_view phase,
                              const ManagedFixtureSource& fixture) {
    const auto plugin_root = tree.root() / phase;
    const auto staged_dll = plugin_root / (fixture.assembly_name + ".dll");
    const auto staged_runtimeconfig = plugin_root / (fixture.assembly_name + ".runtimeconfig.json");
    copy_required_file(fixture.dll, staged_dll);
    copy_required_file(fixture.runtimeconfig, staged_runtimeconfig);

    const auto source_deps = fixture.dll.parent_path() / (fixture.assembly_name + ".deps.json");
    std::error_code error;
    if (fs::is_regular_file(source_deps, error) && !error) {
        copy_required_file(source_deps, plugin_root / source_deps.filename());
    }

    const nlohmann::json manifest = {
        {"id", kPluginId},
        {"name", "Managed Entity launcher fixture"},
        {"version", "1.0.0"},
        {"language", "csharp"},
        {"entry", staged_dll.filename().string()},
        {"managed_type", "SaoAuto.Plugins.HelloCsharp.HelloPlugin, " + fixture.assembly_name},
        {"runtimeconfig", staged_runtimeconfig.filename().string()},
        {"enabled", true},
    };
    const auto manifest_path = plugin_root / "plugin.json";
    write_text(manifest_path, manifest.dump());
    return manifest_path;
}

fs::path write_provider_configuration(const TemporaryDirectory& tree, std::string_view phase,
                                      const fs::path& manifest_path) {
    const nlohmann::json configuration = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = tree.root() / (std::string(phase) + "_provider.json");
    write_text(config_path, configuration.dump());
    return config_path;
}

class LauncherRegistrySession final {
  public:
    LauncherRegistrySession() = default;

    ~LauncherRegistrySession() {
        if (registry_ != nullptr) {
            (void)sao_plugins_shutdown(registry_);
        }
        (void)sao::launcher::loadLauncherProviderConfiguration(nullptr, nullptr);
    }

    LauncherRegistrySession(const LauncherRegistrySession&) = delete;
    LauncherRegistrySession& operator=(const LauncherRegistrySession&) = delete;

    void discover_and_activate(const TemporaryDirectory& tree, const fs::path& config_path) {
        REQUIRE(registry_ == nullptr);
        REQUIRE(sao::launcher::loadLauncherProviderConfiguration(
                    tree.root().c_str(), config_path.c_str()) == SAO_STATUS_OK);
        REQUIRE(sao_plugins_discover(nullptr, &registry_) == SAO_STATUS_OK);
        REQUIRE(registry_ != nullptr);
        REQUIRE(sao_plugins_activate_autostart(registry_) == SAO_STATUS_OK);

        handle_ = loader::sao_plugins_registry_find(loader::sao_plugins_registry_instance(),
                                                    kPluginId.data());
        REQUIRE(handle_ != nullptr);
        REQUIRE(loader::sao_plugins_lifecycle_state(handle_) ==
                loader::lifecycle_state::loaded_active);
        REQUIRE(loader::sao_plugins_lifecycle_get_context(handle_, &context_) == SAO_OK);
        REQUIRE(context_ != nullptr);

        sao_plugins_status_snapshot_t status{};
        status.struct_size = sizeof(status);
        REQUIRE(sao_plugins_status_snapshot(registry_, &status) == SAO_STATUS_OK);
        CHECK(status.discovered_count == 1);
        CHECK(status.loaded_count == 1);
        CHECK(status.enabled_count == 1);
        CHECK(status.deferred_count == 0);
    }

    void shutdown() {
        REQUIRE(registry_ != nullptr);
        REQUIRE(sao_plugins_shutdown(registry_) == SAO_STATUS_OK);
        registry_ = nullptr;
        handle_ = nullptr;
        context_ = nullptr;
    }

    loader::plugin_handle_t handle() const noexcept {
        return handle_;
    }

    loader::plugin_context_t* context() const noexcept {
        return context_;
    }

  private:
    sao_plugins_registry* registry_ = nullptr;
    loader::plugin_handle_t handle_ = nullptr;
    loader::plugin_context_t* context_ = nullptr;
};

class EventSubscription final {
  public:
    EventSubscription() = default;

    ~EventSubscription() {
        if (context_ != nullptr && token_ != 0) {
            (void)loader::sao_plugins_ctx_unsubscribe(context_, token_);
        }
    }

    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    void subscribe(loader::plugin_context_t* context, const char* topic,
                   loader::event_callback_fn callback, void* user_data) {
        REQUIRE(context_ == nullptr);
        REQUIRE(context != nullptr);
        REQUIRE(loader::sao_plugins_ctx_subscribe(context, topic, callback, user_data, &token_) ==
                SAO_OK);
        REQUIRE(token_ != 0);
        context_ = context;
    }

    void unsubscribe() {
        REQUIRE(context_ != nullptr);
        REQUIRE(token_ != 0);
        REQUIRE(loader::sao_plugins_ctx_unsubscribe(context_, token_) == SAO_OK);
        context_ = nullptr;
        token_ = 0;
    }

  private:
    loader::plugin_context_t* context_ = nullptr;
    std::uint32_t token_ = 0;
};

struct UiActionContext {
    routes::EntityActionRouteStore* route_store = nullptr;
    sao_ui_entity_shell_handle_t shell = nullptr;
    publication::OwnedEntityActionResult result;
    std::size_t callback_count = 0;
    sao_status_t last_status = SAO_STATUS_OK;
};

sao_status_t SAO_UI_CALL invoke_managed_action_from_ui(SaoUiEntityAction action, void* user_data) {
    auto* context = static_cast<UiActionContext*>(user_data);
    if (context == nullptr || context->route_store == nullptr || context->shell == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    ++context->callback_count;
    routes::EntityActionRoute route;
    const auto resolve_status =
        context->route_store->resolve(static_cast<std::int32_t>(action), route);
    if (resolve_status != SAO_STATUS_OK) {
        context->last_status = resolve_status;
        return resolve_status;
    }
    context->last_status = publication::invoke_v2(
        route, context->shell, &loader::sao_plugins_entity_provider_invoke_v2,
        &sao_ui_entity_shell_get_snapshot, &sao_ui_entity_shell_home, &context->result);
    return context->last_status;
}

class HeadlessEntityShell final {
  public:
    ~HeadlessEntityShell() {
        if (shell_ != nullptr) {
            if (online_) {
                (void)sao_ui_entity_shell_take_offline(shell_);
            }
            (void)sao_ui_entity_shell_try_destroy(shell_);
        }
    }

    HeadlessEntityShell(const HeadlessEntityShell&) = delete;
    HeadlessEntityShell& operator=(const HeadlessEntityShell&) = delete;
    HeadlessEntityShell() = default;

    void create(UiActionContext& context) {
        REQUIRE(shell_ == nullptr);
        SaoUiEntityShellConfig config{};
        config.width = 420;
        config.height = 460;
        config.origin_x = 100;
        config.origin_y = 200;
        config.action_fn = &invoke_managed_action_from_ui;
        config.action_user_data = &context;
        REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell_) == SAO_STATUS_OK);
        REQUIRE(shell_ != nullptr);
        context.shell = shell_;
    }

    void bring_online() {
        REQUIRE(shell_ != nullptr);
        REQUIRE(sao_ui_entity_shell_bring_online(shell_) == SAO_STATUS_OK);
        online_ = true;
    }

    void stop_input() {
        REQUIRE(shell_ != nullptr);
        REQUIRE(sao_ui_entity_shell_take_offline(shell_) == SAO_STATUS_OK);
        online_ = false;
    }

    void destroy() {
        REQUIRE(shell_ != nullptr);
        if (online_) {
            stop_input();
        }
        REQUIRE(sao_ui_entity_shell_try_destroy(shell_) == SAO_STATUS_OK);
        shell_ = nullptr;
    }

    sao_ui_entity_shell_handle_t get() const noexcept {
        return shell_;
    }

  private:
    sao_ui_entity_shell_handle_t shell_ = nullptr;
    bool online_ = false;
};

catalog::OwnedEntityProviderCatalog snapshot_real_catalog() {
    catalog::OwnedEntityProviderCatalog snapshot;
    REQUIRE(catalog::snapshot_v2(&loader::sao_plugins_entity_provider_snapshot_v2, snapshot) ==
            SAO_STATUS_OK);
    return snapshot;
}

routes::EntityActionRouteSnapshot snapshot_routes(routes::EntityActionRouteStore& store) {
    routes::EntityActionRouteSnapshot snapshot;
    REQUIRE(store.snapshot(snapshot) == SAO_STATUS_OK);
    return snapshot;
}

const catalog::OwnedEntityProvider*
find_provider(const catalog::OwnedEntityProviderCatalog& snapshot) {
    const auto found =
        std::find_if(snapshot.providers.begin(), snapshot.providers.end(),
                     [](const auto& provider) { return provider.provider_id == kProviderId; });
    return found == snapshot.providers.end() ? nullptr : &*found;
}

const catalog::OwnedEntityRootContribution*
find_root(const catalog::OwnedEntityProviderCatalog& snapshot) {
    const auto found =
        std::find_if(snapshot.root_contributions.begin(), snapshot.root_contributions.end(),
                     [](const auto& root) { return root.contribution_id == kContributionId; });
    return found == snapshot.root_contributions.end() ? nullptr : &*found;
}

sao_status_t click_point(sao_ui_entity_shell_handle_t shell, std::int32_t x, std::int32_t y) {
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, x, y, -1, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, x, y, 0, 0) == SAO_STATUS_OK);
    return sao_ui_entity_shell_handle_mouse(shell, kLeftButtonUp, x, y, 0, 0);
}

void select_managed_root(sao_ui_entity_shell_handle_t shell) {
    SaoUiEntityShellSnapshot shell_snapshot{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &shell_snapshot) == SAO_STATUS_OK);
    if (!shell_snapshot.menu_visible) {
        REQUIRE(sao_ui_entity_shell_home(shell) == SAO_STATUS_OK);
    }
    REQUIRE(sao_ui_entity_shell_tick(shell, 450) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &shell_snapshot) == SAO_STATUS_OK);
    REQUIRE(shell_snapshot.menu_visible);

    const auto root_x = shell_snapshot.origin_x + shell_snapshot.menu_x + kRootSlotCenterX;
    const auto first_root_y =
        shell_snapshot.origin_y + shell_snapshot.menu_y + kMenuPad + kRootSlotHeight / 2;
    REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseWheel, root_x, first_root_y, -1, -120) ==
            SAO_STATUS_OK);

    SaoUiEntityRootSnapshot root_snapshot{};
    REQUIRE(sao_ui_entity_shell_get_root_snapshot(shell, &root_snapshot) == SAO_STATUS_OK);
    REQUIRE(root_snapshot.root_count == 6);
    REQUIRE(root_snapshot.first_visible_root_index == 1);
    REQUIRE(root_snapshot.visible_root_count == 5);

    constexpr std::int32_t kManagedPhysicalRootSlot = 4;
    const auto managed_root_y = shell_snapshot.origin_y + shell_snapshot.menu_y + kMenuPad +
                                kManagedPhysicalRootSlot * kRootSlotHeight + kRootSlotHeight / 2;
    REQUIRE(click_point(shell, root_x, managed_root_y) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 1000) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(shell, 200) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_root_snapshot(shell, &root_snapshot) == SAO_STATUS_OK);
    CHECK(std::string_view(root_snapshot.active_root_id_utf8) == kRootId);
}

sao_status_t click_managed_child(sao_ui_entity_shell_handle_t shell) {
    SaoUiEntityShellSnapshot snapshot{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &snapshot) == SAO_STATUS_OK);
    const auto child_x = snapshot.origin_x + snapshot.menu_x + kChildCenterX;
    const auto child_y = snapshot.origin_y + snapshot.menu_y + kMenuPad + kChildCenterY;
    return click_point(shell, child_x, child_y);
}

struct ReentrantUnloadProbe {
    loader::plugin_handle_t plugin = nullptr;
    sao_ui_entity_shell_handle_t shell = nullptr;
    std::uint32_t callback_count = 0;
    std::int32_t unload_status = SAO_OK;
    loader::lifecycle_state lifecycle_after = loader::lifecycle_state::unknown;
    bool menu_visible_on_entry = false;
};

void reentrant_unload_callback(const char*, const char*, void* user_data) {
    auto& probe = *static_cast<ReentrantUnloadProbe*>(user_data);
    ++probe.callback_count;
    SaoUiEntityShellSnapshot snapshot{};
    if (probe.shell != nullptr &&
        sao_ui_entity_shell_get_snapshot(probe.shell, &snapshot) == SAO_STATUS_OK) {
        probe.menu_visible_on_entry = snapshot.menu_visible;
    }
    probe.unload_status = loader::sao_plugins_lifecycle_unload(probe.plugin);
    probe.lifecycle_after = loader::sao_plugins_lifecycle_state(probe.plugin);
}

struct DirectResultProbe {
    std::uint32_t callback_count = 0;
};

std::int32_t SAO_PLUGINS_CALL direct_result_callback(const loader::entity_action_result_v2*,
                                                     void* user_data) {
    ++static_cast<DirectResultProbe*>(user_data)->callback_count;
    return SAO_OK;
}

void check_real_generation_invalid(std::uint64_t generation) {
    DirectResultProbe probe;
    CHECK(loader::sao_plugins_entity_provider_invoke_v2(
              kProviderId.data(), generation, kActionId.data(), "{}", &direct_result_callback,
              &probe) == SAO_ERR_HANDLE_INVALID);
    CHECK(probe.callback_count == 0);
}

void check_stale_route_rejected(const routes::EntityActionRoute& route,
                                sao_ui_entity_shell_handle_t shell) {
    publication::OwnedEntityActionResult result{true, true, true, "sentinel"};
    const auto before = result;
    CHECK(publication::invoke_v2(route, shell, &loader::sao_plugins_entity_provider_invoke_v2,
                                 &sao_ui_entity_shell_get_snapshot, &sao_ui_entity_shell_home,
                                 &result) == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(result == before);
}

void check_entity_catalog_empty() {
    const auto snapshot = snapshot_real_catalog();
    CHECK(snapshot.providers.empty());
    CHECK(snapshot.root_contributions.empty());
}

void check_loader_registry_empty() {
    CHECK(loader::snapshot_manifests(loader::sao_plugins_registry_instance()).empty());
}

void check_csharp_adapter_empty() {
    sao::plugins::csharp_host::cs_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao::plugins::csharp_host::sao_plugins_cshost_register_loader_adapter(
                nullptr, &owner) == SAO_OK);
    REQUIRE(owner != nullptr);
    CHECK(sao::plugins::csharp_host::sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao::plugins::csharp_host::sao_plugins_cshost_unregister_loader_adapter(owner) ==
            SAO_OK);
}

} // namespace

TEST_CASE("C# managed Entity provider uses real launcher provider and headless publication UI "
          "component integration",
          "[launcher][plugins][csharp][entity][v2][headless][integration]") {
    const ManagedFixtureSource fixture_a{
        fs::u8path(SAO_CSHARP_ENTITY_A_DLL),
        fs::u8path(SAO_CSHARP_ENTITY_A_RUNTIMECONFIG),
        "HelloPluginEntityA",
    };
    const ManagedFixtureSource fixture_b{
        fs::u8path(SAO_CSHARP_ENTITY_B_DLL),
        fs::u8path(SAO_CSHARP_ENTITY_B_RUNTIMECONFIG),
        "HelloPluginEntityB",
    };
    std::string capability_reason;
    if (!managed_fixture_available(fixture_a, fixture_b, capability_reason)) {
        SKIP("CAPABILITY_SKIP: " << capability_reason);
    }

    const auto baseline_sdk_contexts = sao_sdk_test_live_context_count();
    TemporaryDirectory tree("ab_lifecycle");
    const auto manifest_a = stage_managed_plugin(tree, "entity_a", fixture_a);
    const auto manifest_b = stage_managed_plugin(tree, "entity_b", fixture_b);
    const auto config_a = write_provider_configuration(tree, "entity_a", manifest_a);
    const auto config_b = write_provider_configuration(tree, "entity_b", manifest_b);

    routes::EntityActionRouteStore route_store;
    publication::EntityProviderPublicationState publication_state;
    UiActionContext ui{&route_store};
    HeadlessEntityShell shell;
    shell.create(ui);
    shell.bring_online();
    LauncherRegistrySession registry;

    registry.discover_and_activate(tree, config_a);
    CHECK(sao_sdk_test_live_context_count() ==
          baseline_sdk_contexts + kManagedLauncherSdkContextCount);

    auto catalog_a_initial = snapshot_real_catalog();
    REQUIRE(catalog_a_initial.abi_version == loader::kEntitySnapshotAbiVersion2);
    REQUIRE(catalog_a_initial.content_token != loader::kInvalidEntitySnapshotContentToken);
    CHECK(catalog_a_initial.provider_stride_bytes == sizeof(loader::entity_provider_view_v2));
    CHECK(catalog_a_initial.root_contribution_stride_bytes ==
          sizeof(loader::entity_root_contribution_view_v2));
    const auto* provider_a_initial = find_provider(catalog_a_initial);
    const auto* root_a_initial = find_root(catalog_a_initial);
    REQUIRE(provider_a_initial != nullptr);
    REQUIRE(root_a_initial != nullptr);
    REQUIRE(provider_a_initial->rows.size() == 1);
    REQUIRE(root_a_initial->actions.size() == 1);
    CHECK(provider_a_initial->owner_plugin_id == kPluginId);
    CHECK(provider_a_initial->generation != 0);
    CHECK(provider_a_initial->snapshot_abi_version == loader::kEntitySnapshotAbiVersion2);
    CHECK(provider_a_initial->revision == 1);
    CHECK(provider_a_initial->content_token != loader::kInvalidEntitySnapshotContentToken);
    CHECK(provider_a_initial->row_stride_bytes == sizeof(loader::entity_menu_row_v2));
    CHECK(provider_a_initial->rows[0].action_id == kActionId);
    CHECK(provider_a_initial->rows[0].category_id == kCategoryId);
    CHECK(provider_a_initial->rows[0].payload_json == R"({"action_count":0})");
    CHECK(provider_a_initial->rows[0].can_activate);
    CHECK_FALSE(provider_a_initial->rows[0].keep_menu_open);
    CHECK_FALSE(provider_a_initial->rows[0].close_menu_before);
    CHECK(root_a_initial->owner_plugin_id == kPluginId);
    CHECK(root_a_initial->contribution_id == kContributionId);
    CHECK(root_a_initial->root_id == kRootId);
    CHECK(root_a_initial->actions[0].provider_id == kProviderId);
    CHECK(root_a_initial->actions[0].action_id == kActionId);

    const auto generation_a = provider_a_initial->generation;
    const auto provider_revision_a_initial = provider_a_initial->revision;
    const auto provider_token_a_initial = provider_a_initial->content_token;
    const auto catalog_token_a_initial = catalog_a_initial.content_token;

    REQUIRE(publication::refresh(shell.get(), route_store, publication_state, false,
                                 &loader::sao_plugins_entity_provider_snapshot_v2,
                                 &loader::sao_plugins_entity_provider_snapshot,
                                 &sao_ui_entity_shell_set_roots) == SAO_STATUS_OK);
    CHECK(publication_state.published_catalog == catalog_a_initial);
    auto routes_a_initial = snapshot_routes(route_store);
    REQUIRE(routes_a_initial.routes.size() == 1);
    const auto route_a_initial = routes_a_initial.routes.front();
    CHECK(routes::is_dynamic_token(route_a_initial.token));
    CHECK(route_a_initial.provider_id == kProviderId);
    CHECK(route_a_initial.provider_generation == generation_a);
    CHECK(route_a_initial.action_id == kActionId);
    CHECK(route_a_initial.category_id == kCategoryId);
    CHECK(route_a_initial.payload_json == R"({"action_count":0})");
    CHECK_FALSE(route_a_initial.keep_menu_open);
    CHECK_FALSE(route_a_initial.close_menu_before);

    SaoUiEntityRootSnapshot root_snapshot{};
    REQUIRE(sao_ui_entity_shell_get_root_snapshot(shell.get(), &root_snapshot) == SAO_STATUS_OK);
    CHECK(root_snapshot.root_count == 6);

    ReentrantUnloadProbe reentry{registry.handle(), shell.get()};
    EventSubscription subscription;
    subscription.subscribe(registry.context(), kReentryTopic.data(), &reentrant_unload_callback,
                           &reentry);

    select_managed_root(shell.get());
    SaoUiEntityShellSnapshot shell_snapshot{};
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell.get(), &shell_snapshot) == SAO_STATUS_OK);
    REQUIRE(shell_snapshot.menu_visible);
    REQUIRE(click_managed_child(shell.get()) == SAO_STATUS_OK);

    CHECK(reentry.callback_count == 1);
    CHECK(reentry.unload_status == loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(reentry.lifecycle_after == loader::lifecycle_state::loaded_active);
    CHECK(reentry.menu_visible_on_entry);
    CHECK(loader::sao_plugins_lifecycle_state(registry.handle()) ==
          loader::lifecycle_state::loaded_active);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell.get(), &shell_snapshot) == SAO_STATUS_OK);
    CHECK_FALSE(shell_snapshot.menu_visible);
    CHECK(shell_snapshot.action_count == 1);
    CHECK(shell_snapshot.last_status == SAO_STATUS_OK);
    CHECK(ui.callback_count == 1);
    CHECK(ui.last_status == SAO_STATUS_OK);
    CHECK(ui.result.callback_received);
    CHECK(ui.result.handled);
    CHECK_FALSE(ui.result.has_result);
    CHECK(ui.result.result_json.empty());

    auto catalog_a_after_action = snapshot_real_catalog();
    const auto* provider_a_after_action = find_provider(catalog_a_after_action);
    REQUIRE(provider_a_after_action != nullptr);
    REQUIRE(provider_a_after_action->rows.size() == 1);
    CHECK(provider_a_after_action->generation == generation_a);
    CHECK(provider_a_after_action->revision == provider_revision_a_initial + 1);
    CHECK(provider_a_after_action->content_token != provider_token_a_initial);
    CHECK(catalog_a_after_action.content_token != catalog_token_a_initial);
    CHECK(provider_a_after_action->rows[0].row_label == "Managed Fixture 1");
    CHECK(provider_a_after_action->rows[0].payload_json == R"({"action_count":1})");
    CHECK(find_root(catalog_a_after_action) != nullptr);

    REQUIRE(publication::refresh(shell.get(), route_store, publication_state, false,
                                 &loader::sao_plugins_entity_provider_snapshot_v2,
                                 &loader::sao_plugins_entity_provider_snapshot,
                                 &sao_ui_entity_shell_set_roots) == SAO_STATUS_OK);
    auto routes_a_after_action = snapshot_routes(route_store);
    REQUIRE(routes_a_after_action.routes.size() == 1);
    const auto route_a = routes_a_after_action.routes.front();
    CHECK(routes_a_after_action.revision > routes_a_initial.revision);
    CHECK(route_a.token == route_a_initial.token);
    CHECK(route_a.provider_id == route_a_initial.provider_id);
    CHECK(route_a.action_id == route_a_initial.action_id);
    CHECK(route_a.provider_generation == generation_a);
    CHECK(route_a.row_label == "Managed Fixture 1");
    CHECK(route_a.payload_json == R"({"action_count":1})");
    CHECK(publication_state.published_catalog == catalog_a_after_action);

    subscription.unsubscribe();
    shell.stop_input();
    REQUIRE(route_store.close_invocation_gate() == SAO_STATUS_OK);
    REQUIRE(publication::clear(shell.get(), route_store, publication_state, false,
                               &sao_ui_entity_shell_set_roots) == SAO_STATUS_OK);
    CHECK(snapshot_routes(route_store).routes.empty());
    CHECK(publication_state.published_catalog.providers.empty());
    CHECK(publication_state.published_catalog.root_contributions.empty());
    CHECK_FALSE(route_a.invocation_allowed());
    check_stale_route_rejected(route_a, shell.get());

    registry.shutdown();
    CHECK(sao_sdk_test_live_context_count() == baseline_sdk_contexts);
    check_entity_catalog_empty();
    check_loader_registry_empty();
    check_csharp_adapter_empty();
    check_real_generation_invalid(generation_a);
    check_stale_route_rejected(route_a, shell.get());

    registry.discover_and_activate(tree, config_b);
    CHECK(sao_sdk_test_live_context_count() ==
          baseline_sdk_contexts + kManagedLauncherSdkContextCount);
    auto catalog_b_initial = snapshot_real_catalog();
    const auto* provider_b_initial = find_provider(catalog_b_initial);
    const auto* root_b_initial = find_root(catalog_b_initial);
    REQUIRE(provider_b_initial != nullptr);
    REQUIRE(root_b_initial != nullptr);
    REQUIRE(provider_b_initial->rows.size() == 1);
    REQUIRE(root_b_initial->actions.size() == 1);
    CHECK(provider_b_initial->provider_id == kProviderId);
    CHECK(provider_b_initial->owner_plugin_id == kPluginId);
    CHECK(provider_b_initial->generation != generation_a);
    CHECK(provider_b_initial->rows[0].action_id == kActionId);
    CHECK(provider_b_initial->rows[0].payload_json == R"({"action_count":0})");
    CHECK(root_b_initial->root_id == kRootId);
    CHECK(root_b_initial->actions[0].provider_id == kProviderId);
    CHECK(root_b_initial->actions[0].action_id == kActionId);
    const auto generation_b = provider_b_initial->generation;

    REQUIRE(publication::refresh(shell.get(), route_store, publication_state, false,
                                 &loader::sao_plugins_entity_provider_snapshot_v2,
                                 &loader::sao_plugins_entity_provider_snapshot,
                                 &sao_ui_entity_shell_set_roots) == SAO_STATUS_OK);
    auto routes_b_initial = snapshot_routes(route_store);
    REQUIRE(routes_b_initial.routes.size() == 1);
    const auto route_b = routes_b_initial.routes.front();
    CHECK(route_b.provider_id == route_a.provider_id);
    CHECK(route_b.action_id == route_a.action_id);
    CHECK(route_b.provider_generation == generation_b);
    CHECK(route_b.provider_generation != route_a.provider_generation);
    CHECK(route_b.invocation_allowed());
    check_real_generation_invalid(generation_a);
    check_stale_route_rejected(route_a, shell.get());

    ui.result = {};
    shell.bring_online();
    select_managed_root(shell.get());
    REQUIRE(click_managed_child(shell.get()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_get_snapshot(shell.get(), &shell_snapshot) == SAO_STATUS_OK);
    CHECK_FALSE(shell_snapshot.menu_visible);
    CHECK(shell_snapshot.action_count == 2);
    CHECK(shell_snapshot.last_status == SAO_STATUS_OK);
    CHECK(ui.callback_count == 2);
    CHECK(ui.last_status == SAO_STATUS_OK);
    CHECK(ui.result.callback_received);
    CHECK(ui.result.handled);
    CHECK_FALSE(ui.result.has_result);
    CHECK(ui.result.result_json.empty());

    shell.stop_input();
    REQUIRE(route_store.close_invocation_gate() == SAO_STATUS_OK);
    REQUIRE(publication::clear(shell.get(), route_store, publication_state, false,
                               &sao_ui_entity_shell_set_roots) == SAO_STATUS_OK);
    CHECK(snapshot_routes(route_store).routes.empty());
    CHECK_FALSE(route_b.invocation_allowed());
    check_stale_route_rejected(route_b, shell.get());

    registry.shutdown();
    CHECK(sao_sdk_test_live_context_count() == baseline_sdk_contexts);
    check_entity_catalog_empty();
    check_loader_registry_empty();
    check_real_generation_invalid(generation_a);
    check_real_generation_invalid(generation_b);
    check_stale_route_rejected(route_a, shell.get());
    check_stale_route_rejected(route_b, shell.get());
    check_csharp_adapter_empty();

    shell.destroy();
    CHECK(sao_sdk_test_live_context_count() == baseline_sdk_contexts);
}
