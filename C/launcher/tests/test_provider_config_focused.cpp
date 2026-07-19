#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#endif

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON) || defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
extern "C" size_t sao_launcher_test_platform_timer_count() noexcept;
extern "C" size_t sao_launcher_test_platform_timer_worker_count() noexcept;
extern "C" uint64_t sao_launcher_test_platform_timer_unregister_attempt_count() noexcept;
extern "C" void sao_launcher_test_fire_timer_during_register(bool enabled) noexcept;
extern "C" void sao_launcher_test_fail_next_timer_unregister(bool enabled) noexcept;
#endif

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

sao::plugins::loader::plugin_handle_t add_external_plugin(const char* plugin_id,
                                                           const fs::path& source_path) {
    sao::plugins::loader::plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "fixture.emma";
    manifest.language = sao::plugins::loader::engine_kind::emma;
    manifest.source_path = source_path.string();
    manifest.abi_version = 2;
    sao::plugins::loader::plugin_handle_t handle = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_registry_add_plugin(
                sao::plugins::loader::sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    return handle;
}

void platform_timer_callback(void*) {}

int32_t platform_render_callback(const char*, const char*, char**, void*) {
    return SAO_OK;
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON) || defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
template <typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

struct one_shot_probe {
    sao::plugins::loader::plugin_context_t* context = nullptr;
    std::array<char, 32> token{};
    std::atomic_bool token_ready{false};
    std::atomic_uint32_t callbacks{0};
    std::atomic_uint32_t callbacks_before_token{0};
    bool complete_loader_ledger = true;
    bool block_callback = false;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
};

void one_shot_callback(void* user_data) {
    auto& probe = *static_cast<one_shot_probe*>(user_data);
    if (!probe.token_ready.load(std::memory_order_acquire)) {
        probe.callbacks_before_token.fetch_add(1, std::memory_order_relaxed);
    } else if (probe.complete_loader_ledger) {
        (void)sao::plugins::loader::sao_plugins_ctx_complete_timer(probe.context,
                                                                   probe.token.data());
    }
    probe.callbacks.fetch_add(1, std::memory_order_relaxed);
    if (!probe.block_callback)
        return;
    std::unique_lock lock(probe.mutex);
    probe.entered = true;
    probe.condition.notify_all();
    probe.condition.wait(lock, [&probe] { return probe.release; });
}

void publish_timer_token(one_shot_probe& probe, char* token) {
    REQUIRE(token != nullptr);
    REQUIRE(token[0] != '\0');
    REQUIRE(strlen(token) < probe.token.size());
    strcpy_s(probe.token.data(), probe.token.size(), token);
    sao::plugins::loader::sao_plugins_ctx_free_string(token);
    probe.token_ready.store(true, std::memory_order_release);
}

bool wait_for_blocked_callback(one_shot_probe& probe) {
    std::unique_lock lock(probe.mutex);
    return probe.condition.wait_for(lock, std::chrono::seconds(5),
                                    [&probe] { return probe.entered; });
}

void release_blocked_callback(one_shot_probe& probe) {
    {
        std::lock_guard lock(probe.mutex);
        probe.release = true;
    }
    probe.condition.notify_all();
}
#endif

} // namespace

TEST_CASE("launcher owns real platform provider sessions and excludes unwired capabilities",
          "[launcher][provider][plugins][platform][focused]") {
    temporary_tree tree("platform_provider");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);

    const auto handle = add_external_plugin("launcher_platform_provider_probe", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    wchar_t* selected_path = reinterpret_cast<wchar_t*>(1);
    CHECK(sao::plugins::loader::sao_plugins_ctx_open_file(context, "[]", "Pick", L"", 0,
                                                           &selected_path) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(selected_path == nullptr);
    CHECK(sao::plugins::loader::sao_plugins_ctx_open_window(context, "probe", 320, 240) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao::plugins::loader::sao_plugins_ctx_register_hotkey(
              context, "probe", "F12", "Probe", platform_timer_callback, nullptr) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    uint32_t render_token = 0;
    CHECK(sao::plugins::loader::sao_plugins_ctx_register_render_hook(
              context, "probe", 0.0F, platform_render_callback, nullptr, &render_token) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(render_token == 0);
    CHECK(sao::plugins::loader::sao_plugins_ctx_set_overlay(context, "probe", "{}") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao::plugins::loader::sao_plugins_ctx_request_redraw(context, "probe", "test") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(sao::plugins::loader::sao_plugins_ctx_create_compositor_layer(
              context, "probe", 4, 4, 0, 0, 0, true, false, 0) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON) || defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    char* timer_token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_interval(
                context, platform_timer_callback, 60.0, nullptr, &timer_token) == SAO_OK);
    REQUIRE(timer_token != nullptr);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_clear_timer(context, timer_token) == SAO_OK);
    sao::plugins::loader::sao_plugins_ctx_free_string(timer_token);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_notify(context, "Provider", "Ready", 0.1,
                                                          "info") == SAO_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_dismiss_notify(context) == SAO_OK);
#else
    char* timer_token = reinterpret_cast<char*>(1);
    CHECK(sao::plugins::loader::sao_plugins_ctx_set_interval(
              context, platform_timer_callback, 60.0, nullptr, &timer_token) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(timer_token == nullptr);
    CHECK(sao::plugins::loader::sao_plugins_ctx_notify(context, "Provider", "Ready", 0.1,
                                                        "info") ==
          sao::plugins::loader::SAO_PLUGINS_ERR_UNSUPPORTED);
#endif

    CHECK(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PYTHON) || defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
TEST_CASE("launcher one-shot timeouts release SDK timers without map or worker growth",
          "[launcher][provider][plugins][platform][timer][one-shot][focused]") {
    temporary_tree tree("platform_timeout_stress");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_stress", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    REQUIRE(sao_launcher_test_platform_timer_count() == 0);
    REQUIRE(sao_launcher_test_platform_timer_worker_count() == 0);

    constexpr size_t kBatchSize = 48;
    for (int batch = 0; batch < 2; ++batch) {
        std::vector<one_shot_probe> probes(kBatchSize);
        for (auto& probe : probes) {
            probe.context = context;
            char* token = nullptr;
            REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                        context, one_shot_callback, 0.1, &probe, &token) == SAO_OK);
            publish_timer_token(probe, token);
        }
        REQUIRE(wait_until([&probes] {
            return std::all_of(probes.begin(), probes.end(), [](const one_shot_probe& probe) {
                return probe.callbacks.load(std::memory_order_relaxed) == 1;
            });
        }));
        REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));
        CHECK(std::all_of(probes.begin(), probes.end(), [](const one_shot_probe& probe) {
            return probe.callbacks_before_token.load(std::memory_order_relaxed) == 0;
        }));
        CHECK(sao_launcher_test_platform_timer_worker_count() == 1);
    }

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_worker_count() == 0; }));
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("launcher defers a synchronous registration callback until timer publication",
          "[launcher][provider][plugins][platform][timer][registration][focused]") {
    temporary_tree tree("platform_timeout_sync_register");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_sync_register", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    one_shot_probe probe;
    probe.context = context;
    sao_launcher_test_fire_timer_during_register(true);
    char* token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                context, one_shot_callback, 0.1, &probe, &token) == SAO_OK);
    CHECK(probe.callbacks.load(std::memory_order_relaxed) == 0);
    publish_timer_token(probe, token);
    REQUIRE(wait_until([&probe] {
        return probe.callbacks.load(std::memory_order_relaxed) == 1;
    }));
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));
    CHECK(probe.callbacks_before_token.load(std::memory_order_relaxed) == 0);

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("launcher timer clear and shutdown race deferred cleanup idempotently",
          "[launcher][provider][plugins][platform][timer][concurrency][focused]") {
    temporary_tree tree("platform_timeout_concurrent_cleanup");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_concurrent_cleanup", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    one_shot_probe probe;
    probe.context = context;
    probe.complete_loader_ledger = false;
    probe.block_callback = true;
    char* token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                context, one_shot_callback, 0.02, &probe, &token) == SAO_OK);
    publish_timer_token(probe, token);
    REQUIRE(wait_for_blocked_callback(probe));

    const auto attempts = sao_launcher_test_platform_timer_unregister_attempt_count();
    std::atomic_int32_t clear_status{SAO_ERR_NOT_INITIALIZED};
    std::thread clear_thread([&] {
        clear_status.store(sao::plugins::loader::sao_plugins_ctx_clear_timer(
                               context, probe.token.data()),
                           std::memory_order_release);
    });
    REQUIRE(wait_until([attempts] {
        return sao_launcher_test_platform_timer_unregister_attempt_count() > attempts;
    }));
    CHECK(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    release_blocked_callback(probe);
    clear_thread.join();
    CHECK(clear_status.load(std::memory_order_acquire) == SAO_OK);
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("launcher preserves failed one-shot unregister for teardown retry",
          "[launcher][provider][plugins][platform][timer][retry][focused]") {
    temporary_tree tree("platform_timeout_unregister_retry");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    const auto handle = add_external_plugin("launcher_timeout_unregister_retry", tree.root());
    auto* context = sao::plugins::loader::sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    one_shot_probe probe;
    probe.context = context;
    char* token = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_ctx_set_timeout(
                context, one_shot_callback, 0.1, &probe, &token) == SAO_OK);
    publish_timer_token(probe, token);
    const auto attempts = sao_launcher_test_platform_timer_unregister_attempt_count();
    sao_launcher_test_fail_next_timer_unregister(true);
    REQUIRE(wait_until([&probe] {
        return probe.callbacks.load(std::memory_order_relaxed) == 1;
    }));
    REQUIRE(wait_until([attempts] {
        return sao_launcher_test_platform_timer_unregister_attempt_count() > attempts;
    }));
    CHECK(sao_launcher_test_platform_timer_count() == 1);

    sao::plugins::loader::sao_plugins_ctx_destroy(context);
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_count() == 0; }));
    REQUIRE(wait_until([] { return sao_launcher_test_platform_timer_worker_count() == 0; }));
    CHECK(sao_launcher_test_platform_timer_unregister_attempt_count() >= attempts + 2);
    REQUIRE(sao::plugins::loader::sao_plugins_registry_remove(
                sao::plugins::loader::sao_plugins_registry_instance(), handle) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}
#endif

TEST_CASE("launcher owns dependency path sessions across shutdown retry",
          "[launcher][provider][plugins][deps][focused]") {
    temporary_tree tree("deps_provider");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    const auto dependency_path = tree.root() / "libs";
    REQUIRE(fs::create_directories(dependency_path));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);

    sao::plugins::loader::deps_bootstrap_record record;
    record.added_paths.push_back(fs::absolute(dependency_path).wstring());
    sao::plugins::loader::deps_session_t session = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_deps_attach(
                "launcher_deps_provider_probe", tree.root().c_str(), &record, &session) == SAO_OK);
    REQUIRE(session != nullptr);

    CHECK(sao_plugins_shutdown(registry) != SAO_STATUS_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_deps_session_close(session) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);

    registry = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &registry) == SAO_STATUS_OK);
    REQUIRE(registry != nullptr);
    REQUIRE(sao_plugins_shutdown(registry) == SAO_STATUS_OK);
}

TEST_CASE("failed second discovery preserves the first registry provider owners",
          "[launcher][provider][plugins][registration][rollback][focused]") {
    temporary_tree tree("provider_registration_rollback");
    REQUIRE(fs::create_directories(tree.root() / "plugins"));
    const auto dependency_path = tree.root() / "libs";
    REQUIRE(fs::create_directories(dependency_path));
    REQUIRE(sao::launcher::loadLauncherProviderConfiguration(tree.root().c_str(), nullptr) ==
            SAO_STATUS_OK);

    sao_plugins_registry* first = nullptr;
    REQUIRE(sao_plugins_discover(nullptr, &first) == SAO_STATUS_OK);
    REQUIRE(first != nullptr);
    sao_plugins_registry* second = nullptr;
    CHECK(sao_plugins_discover(nullptr, &second) ==
          sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS);
    CHECK(second == nullptr);

    sao::plugins::loader::deps_bootstrap_record record;
    record.added_paths.push_back(fs::absolute(dependency_path).wstring());
    sao::plugins::loader::deps_session_t session = nullptr;
    REQUIRE(sao::plugins::loader::sao_plugins_deps_attach(
                "launcher_provider_registration_probe", tree.root().c_str(), &record, &session) ==
            SAO_OK);
    REQUIRE(session != nullptr);
    CHECK(sao_plugins_shutdown(first) != SAO_STATUS_OK);
    REQUIRE(sao::plugins::loader::sao_plugins_deps_session_close(session) == SAO_OK);
    REQUIRE(sao_plugins_shutdown(first) == SAO_STATUS_OK);
}

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
