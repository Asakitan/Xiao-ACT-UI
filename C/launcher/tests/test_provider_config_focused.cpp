#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#endif

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON)
constexpr int32_t kPythonNoHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_UNCONFIGURED;
constexpr int32_t kPythonMissingHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_UNAVAILABLE;
#else
constexpr int32_t kPythonNoHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE;
constexpr int32_t kPythonMissingHomeStatus = SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE;
#endif

namespace fs = std::filesystem;

class temporary_tree {
  public:
    explicit temporary_tree(const char* label) {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                (std::string("sao_launcher_provider_") + label + "_" + std::to_string(suffix));
        REQUIRE(fs::create_directories(root_));
    }

    ~temporary_tree() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    const fs::path& root() const {
        return root_;
    }

  private:
    fs::path root_;
};

class current_path_guard {
  public:
    explicit current_path_guard(const fs::path& replacement) : original_(fs::current_path()) {
        fs::current_path(replacement);
    }

    ~current_path_guard() {
        std::error_code error;
        fs::current_path(original_, error);
    }

  private:
    fs::path original_;
};

void write_text(const fs::path& path, const std::string& content) {
    REQUIRE((fs::create_directories(path.parent_path()) || fs::is_directory(path.parent_path())));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output.good());
}

std::vector<fs::path> configured_roots() {
    std::vector<fs::path> roots;
    for (const auto& root : sao::launcher::launcherProviderConfigurationSnapshot().plugins.roots) {
        roots.push_back(fs::weakly_canonical(root));
    }
    return roots;
}

std::string path_utf8(const fs::path& path) {
    const auto wide = path.wstring();
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(size > 0);
    std::string output(static_cast<size_t>(size), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), output.data(), size, nullptr,
                                nullptr) == size);
    return output;
}

} // namespace

TEST_CASE("provider defaults use only existing packaged roots",
          "[launcher][provider][plugins][focused]") {
    temporary_tree tree("packaged");
    const auto base = tree.root() / "package";
    const auto packaged_plugins = base / "plugins";
    const auto packaged_python_plugins = base / "python" / "plugins";
    REQUIRE(fs::create_directories(packaged_plugins));
    REQUIRE(fs::create_directories(packaged_python_plugins));

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), nullptr) ==
            SAO_STATUS_OK);
    const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    REQUIRE(configuration.enabled);
    REQUIRE(configured_roots() == std::vector<fs::path>{fs::canonical(packaged_plugins),
                                                        fs::canonical(packaged_python_plugins)});
    REQUIRE(configuration.python_home.empty());
}

TEST_CASE("provider defaults derive source roots without current path",
          "[launcher][provider][plugins][focused]") {
    temporary_tree tree("source");
    const auto source = tree.root() / "workspace" / "sao_auto";
    const auto base = source / "C" / "build" / "focused" / "bin";
    const auto top_plugins = source.parent_path() / "plugins";
    const auto source_python_plugins = source / "python" / "plugins";
    const auto unrelated = tree.root() / "unrelated";
    REQUIRE(fs::create_directories(base));
    REQUIRE(fs::create_directories(top_plugins));
    REQUIRE(fs::create_directories(source_python_plugins));
    REQUIRE(fs::create_directories(unrelated / "plugins"));
    write_text(source / "C" / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.28)\n");

    current_path_guard current_path(unrelated);
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), nullptr) ==
            SAO_STATUS_OK);
    const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    REQUIRE(configuration.enabled);
    REQUIRE(configured_roots() == std::vector<fs::path>{fs::canonical(top_plugins),
                                                        fs::canonical(source_python_plugins)});
    REQUIRE(configuration.python_home.empty());
}

TEST_CASE("provider defaults stay disabled when no candidate root exists",
          "[launcher][provider][plugins][focused]") {
    temporary_tree tree("missing");
    const auto base = tree.root() / "empty";
    REQUIRE(fs::create_directories(base));

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), nullptr) ==
            SAO_STATUS_OK);
    const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
    REQUIRE_FALSE(configuration.enabled);
    REQUIRE(configuration.roots.empty());
    REQUIRE(configuration.python_home.empty());
}

TEST_CASE("provider explicit native-only manifest does not require a script entry",
          "[launcher][provider][plugins][manifest][focused]") {
    temporary_tree tree("native_manifest");
    const auto base = tree.root() / "package";
    const auto plugin = base / "native_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "native_fixture.dll", "fixture");
    write_text(
        manifest_path,
        R"({"id":"provider_native_only","enabled":false,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","abi_version":2})");

    nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.struct_size == sizeof(status));
    CHECK(status.python_runtime_status == kPythonNoHomeStatus);
    CHECK(status.discovered_count == 1);

    constexpr std::size_t prefix_size =
        offsetof(sao_plugins_status_snapshot_t, operational_status) +
        sizeof(status.operational_status);
    sao_plugins_status_snapshot_t prefix{};
    std::memset(reinterpret_cast<std::byte*>(&prefix) + prefix_size, 0x5a,
                sizeof(prefix) - prefix_size);
    prefix.struct_size = static_cast<std::uint32_t>(prefix_size);
    REQUIRE(sao_plugins_status_snapshot(registry, &prefix) == SAO_STATUS_OK);
    CHECK(prefix.struct_size == prefix_size);
    CHECK(prefix.python_runtime_status == kPythonNoHomeStatus);
    CHECK(prefix.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    const auto* suffix = reinterpret_cast<const unsigned char*>(&prefix) + prefix_size;
    CHECK(std::all_of(suffix, reinterpret_cast<const unsigned char*>(&prefix) + sizeof(prefix),
                      [](unsigned char value) { return value == 0x5a; }));
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);

    registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("unavailable optional Python runtime does not block native discovery",
          "[launcher][provider][plugins][runtime][focused]") {
    temporary_tree tree("unavailable_python");
    const auto base = tree.root() / "package";
    const auto plugin = base / "native_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "native_fixture.dll", "fixture");
    write_text(
        manifest_path,
        R"({"id":"provider_native_without_python","enabled":false,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","abi_version":2})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(manifest_path)})},
          {"python_home", path_utf8(base / "missing_python_runtime")}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.python_runtime_status == kPythonMissingHomeStatus);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("enabled Python without runtime is deferred and visible as degraded",
          "[launcher][provider][plugins][adapter][degraded][focused]") {
    temporary_tree tree("missing_adapter");
    const auto base = tree.root() / "package";
    const auto plugin = base / "python_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.py", "def on_load(ctx):\n    return True\n");
    write_text(
        manifest_path,
        R"({"id":"provider_python_without_home","language":"python","entry":"plugin.py","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    REQUIRE(sao::launcher::launcherProviderConfigurationSnapshot().plugins.python_home.empty());
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.python_runtime_status == kPythonNoHomeStatus);
    CHECK(status.python_launch_strategy == SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    CHECK(status.deferred_count == 1);
    CHECK(status.loaded_count == 0);
    CHECK(status.enabled_count == 0);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("Python degradation propagates through disabled dependency nodes",
          "[launcher][provider][plugins][dependencies][degraded][focused]") {
    temporary_tree tree("python_disabled_dependency_closure");
    const auto base = tree.root() / "package";
    const auto python = base / "python_base";
    const auto bridge = base / "disabled_bridge";
    const auto leaf = base / "enabled_leaf";
    write_text(python / "plugin.py", "def on_load(ctx):\n    return True\n");
    write_text(python / "plugin.json",
               R"({"id":"python_base","language":"python","entry":"plugin.py","enabled":false})");
    write_text(bridge / "bridge.dll", "fixture");
    write_text(
        bridge / "plugin.json",
        R"({"id":"disabled_bridge","enabled":false,"native_entry":"bridge.dll","native_abi":"sao_plugin_v2","abi_version":2,"requires":["python_base"]})");
    write_text(leaf / "leaf.dll", "fixture");
    write_text(
        leaf / "plugin.json",
        R"({"id":"enabled_leaf","enabled":true,"native_entry":"leaf.dll","native_abi":"sao_plugin_v2","abi_version":2,"requires":["disabled_bridge"]})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(python / "plugin.json"),
                                               path_utf8(bridge / "plugin.json"),
                                               path_utf8(leaf / "plugin.json")})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());

    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);
    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.discovered_count == 3);
    CHECK(status.deferred_count == 1);
    CHECK(status.loaded_count == 0);
    CHECK(status.enabled_count == 0);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
struct lifecycle_reentry_probe {
    sao_plugins_registry* registry = nullptr;
    bool entered = false;
    sao_status_t status_status = SAO_STATUS_OK;
    sao_status_t reload_status = SAO_STATUS_OK;
    sao_status_t shutdown_status = SAO_STATUS_OK;
};

void lifecycle_reentry_callback(sao::plugins::loader::plugin_handle_t,
                                sao::plugins::loader::lifecycle_event, const char*,
                                void* user_data) {
    auto& probe = *static_cast<lifecycle_reentry_probe*>(user_data);
    if (probe.entered)
        return;
    probe.entered = true;
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    probe.status_status = sao_plugins_status_snapshot(probe.registry, &status);
    probe.reload_status = sao_plugins_reload_all(probe.registry);
    probe.shutdown_status = sao_plugins_shutdown(probe.registry);
}

TEST_CASE("provider lifecycle subscribers get BUSY on operation reentry",
          "[launcher][provider][plugins][lifecycle][reentry][focused]") {
    temporary_tree tree("emma_lifecycle_reentry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    write_text(plugin / "plugin.emma", "fn on_load(ctx)\n    return true\nend\n"
                                       "fn on_enable()\n    return true\nend\n"
                                       "fn on_disable()\n    return true\nend\n"
                                       "fn on_unload()\n    return true\nend\n");
    write_text(
        plugin / "plugin.json",
        R"({"id":"provider_emma_reentry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(plugin / "plugin.json")})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    lifecycle_reentry_probe probe{registry};
    std::uint32_t token = 0;
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_subscribe(&lifecycle_reentry_callback,
                                                                  &probe, &token) == SAO_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_unsubscribe(token) == SAO_OK);
    REQUIRE(probe.entered);
    CHECK(probe.status_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.reload_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.shutdown_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider repeat discover and shutdown releases Emma adapter ownership",
          "[launcher][provider][plugins][emma][focused]") {
    temporary_tree tree("emma_repeat");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               R"EMMA(fn on_load(ctx)
    return true
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    return true
end
)EMMA");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_repeat","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    for (int iteration = 0; iteration < 2; ++iteration) {
        sao_plugins_registry* registry = nullptr;
        REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
        REQUIRE(registry != nullptr);
        REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
        REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    }
}

TEST_CASE("provider preserves BUSY cleanup for retry",
          "[launcher][provider][plugins][cleanup_retry][focused]") {
    temporary_tree tree("emma_busy_retry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               R"EMMA(let unload_attempts = 0
fn on_load(ctx)
    return true
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    unload_attempts = unload_attempts + 1
    return unload_attempts > 1
end
)EMMA");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_busy_retry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    const auto handle = sao::plugins::loader::sao_plugins_registry_find(
        sao::plugins::loader::sao_plugins_registry_instance(), "provider_emma_busy_retry");
    REQUIRE(handle != nullptr);
    REQUIRE(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
            sao::plugins::loader::lifecycle_state::loaded_active);
    REQUIRE(sao_plugins_reload_all(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
          sao::plugins::loader::lifecycle_state::loaded_active);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    CHECK(status.last_operation_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(status.last_rollback_status == SAO_STATUS_OK);
    CHECK(status.rollback_attempted == 1);
    CHECK(status.rollback_succeeded == 1);
    CHECK(status.loaded_count == 1);
    CHECK(status.enabled_count == 1);
    REQUIRE(sao_plugins_reload_all(registry) == SAO_STATUS_OK);
    CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
          sao::plugins::loader::lifecycle_state::loaded_active);
    REQUIRE(sao_plugins_shutdown(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider reload restores a multi-node dependency graph exactly",
          "[launcher][provider][plugins][reload][dependencies][rollback][focused]") {
    temporary_tree tree("emma_dependency_rollback");
    const auto base = tree.root() / "package";
    const auto make_plugin = [&](const char* id, const char* requires_json,
                                 const char* unload_body) {
        const auto plugin = base / id;
        write_text(plugin / "plugin.emma", std::string{"fn on_load(ctx)\n    return true\nend\n"
                                                       "fn on_enable()\n    return true\nend\n"
                                                       "fn on_disable()\n    return true\nend\n"} +
                                               unload_body + "\n");
        write_text(plugin / "plugin.json", std::string{"{\"id\":\""} + id +
                                               "\",\"language\":\"emma\",\"entry\":\"plugin.emma\","
                                               "\"enabled\":true,\"requires\":" +
                                               requires_json + "}");
        return plugin / "plugin.json";
    };
    const auto base_manifest = make_plugin(
        "dep_base", "[]",
        "let unload_attempts = 0\nfn on_unload()\n    unload_attempts = unload_attempts + 1\n"
        "    return unload_attempts > 1\nend");
    const auto mid_manifest =
        make_plugin("dep_mid", "[\"dep_base\"]", "fn on_unload()\n    return true\nend");
    const auto leaf_manifest =
        make_plugin("dep_leaf", "[\"dep_mid\"]", "fn on_unload()\n    return true\nend");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true},
          {"manifests", nlohmann::json::array({path_utf8(base_manifest), path_utf8(mid_manifest),
                                               path_utf8(leaf_manifest)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    const auto global = sao::plugins::loader::sao_plugins_registry_instance();
    const auto base_handle = sao::plugins::loader::sao_plugins_registry_find(global, "dep_base");
    const auto mid_handle = sao::plugins::loader::sao_plugins_registry_find(global, "dep_mid");
    const auto leaf_handle = sao::plugins::loader::sao_plugins_registry_find(global, "dep_leaf");
    REQUIRE(base_handle != nullptr);
    REQUIRE(mid_handle != nullptr);
    REQUIRE(leaf_handle != nullptr);

    REQUIRE(sao_plugins_reload_all(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    for (const auto handle : {base_handle, mid_handle, leaf_handle}) {
        CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
              sao::plugins::loader::lifecycle_state::loaded_active);
    }
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_READY);
    CHECK(status.last_operation_status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    CHECK(status.last_rollback_status == SAO_STATUS_OK);
    CHECK(status.rollback_attempted == 1);
    CHECK(status.rollback_succeeded == 1);
    CHECK(status.loaded_count == 3);
    CHECK(status.enabled_count == 3);

    REQUIRE(sao_plugins_reload_all(registry) == SAO_STATUS_OK);
    for (const auto handle : {base_handle, mid_handle, leaf_handle}) {
        CHECK(sao::plugins::loader::sao_plugins_lifecycle_state(handle) ==
              sao::plugins::loader::lifecycle_state::loaded_active);
    }
    REQUIRE(sao_plugins_shutdown(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider reload rollback failure publishes degraded internal",
          "[launcher][provider][plugins][reload][rollback][degraded][focused]") {
    temporary_tree tree("emma_reload_degraded");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma", "fn on_load(ctx)\n    return true\nend\n"
                                       "fn on_enable()\n    return true\nend\n"
                                       "fn on_disable()\n    return true\nend\n"
                                       "fn on_unload()\n    return true\nend\n");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_degraded","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    write_text(plugin / "plugin.emma",
               "fn on_load(ctx)\n    missing_function()\n    return true\nend\n");

    CHECK(sao_plugins_reload_all(registry) == SAO_STATUS_INTERNAL);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    REQUIRE(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_OK);
    CHECK(status.operational_status == SAO_PLUGINS_OPERATIONAL_DEGRADED);
    CHECK(status.last_operation_status != SAO_STATUS_OK);
    CHECK(status.last_rollback_status != SAO_STATUS_OK);
    CHECK(status.rollback_attempted == 1);
    CHECK(status.rollback_succeeded == 0);
    CHECK(status.loaded_count == 0);
    CHECK(status.enabled_count == 0);
    CHECK(sao_plugins_reload_all(registry) == SAO_STATUS_INTERNAL);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("provider stable shell serializes reload status and shutdown",
          "[launcher][provider][plugins][concurrency][shutdown][focused]") {
    temporary_tree tree("stable_shell_concurrency");
    const auto base = tree.root() / "package";
    const auto plugin = base / "native_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "native_fixture.dll", "fixture");
    write_text(
        manifest_path,
        R"({"id":"provider_concurrent_native","enabled":false,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","abi_version":2})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);

    std::atomic_bool start{false};
    std::atomic_uint32_t unexpected{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 100; ++iteration) {
                sao_plugins_status_snapshot_t status{};
                status.struct_size = sizeof(status);
                const auto result = sao_plugins_status_snapshot(registry, &status);
                if (result != SAO_STATUS_OK && result != SAO_STATUS_INTERNAL) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int iteration = 0; iteration < 50; ++iteration) {
            const auto result = sao_plugins_reload_all(registry);
            if (result != SAO_STATUS_OK && result != SAO_STATUS_INTERNAL) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    start.store(true, std::memory_order_release);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(unexpected.load(std::memory_order_relaxed) == 0);
    CHECK(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
    sao_plugins_status_snapshot_t status{};
    status.struct_size = sizeof(status);
    CHECK(sao_plugins_status_snapshot(registry, &status) == SAO_STATUS_INTERNAL);
}

TEST_CASE("provider retries failed resident runtime cleanup",
          "[launcher][provider][plugins][cleanup_retry][failed][focused]") {
    temporary_tree tree("emma_failed_retry");
    const auto base = tree.root() / "package";
    const auto plugin = base / "emma_plugin";
    const auto manifest_path = plugin / "plugin.json";
    write_text(plugin / "plugin.emma",
               R"EMMA(let unload_attempts = 0
fn on_load(ctx)
    return true
end
fn on_enable()
    return true
end
fn on_disable()
    return true
end
fn on_unload()
    unload_attempts = unload_attempts + 1
    if unload_attempts == 1
        missing_function()
    end
    return true
end
)EMMA");
    write_text(
        manifest_path,
        R"({"id":"provider_emma_failed_retry","language":"emma","entry":"plugin.emma","enabled":true})");
    const nlohmann::json root = {
        {"plugins",
         {{"enabled", true}, {"manifests", nlohmann::json::array({path_utf8(manifest_path)})}}},
    };
    const auto config_path = base / "provider.json";
    write_text(config_path, root.dump());
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(base.c_str(), config_path.c_str()) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}
#endif
