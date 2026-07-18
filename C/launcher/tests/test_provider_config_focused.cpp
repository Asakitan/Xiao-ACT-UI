#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
#include "sao/plugins/loader/loader_status.h"
#endif

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

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
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("script autostart without its configured adapter fails closed",
          "[launcher][provider][plugins][adapter][focused]") {
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
    REQUIRE(sao_plugins_activate_autostart(registry) == SAO_STATUS_PLUGIN_LOAD_FAIL);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
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
    REQUIRE(sao_plugins_shutdown(registry) == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
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
