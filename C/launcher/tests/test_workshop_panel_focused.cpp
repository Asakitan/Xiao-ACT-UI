#include <catch2/catch_test_macros.hpp>

#include "workshop_panel_internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using sao::launcher::workshop_panel::Operations;
using sao::launcher::workshop_panel::Owner;
using sao::launcher::workshop_panel::PluginDetail;
using sao::launcher::workshop_panel::PluginPage;
using sao::launcher::workshop_panel::PluginSummary;

class BoundCompositor final {
  public:
    BoundCompositor() {
        SaoCompositorConfig config{};
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &handle_) == SAO_STATUS_OK);
    }

    ~BoundCompositor() {
        if (handle_ != nullptr)
            (void)sao_ui_compositor_try_destroy(handle_);
    }

    BoundCompositor(const BoundCompositor&) = delete;
    BoundCompositor& operator=(const BoundCompositor&) = delete;

    sao_ui_compositor_handle_t get() const noexcept {
        return handle_;
    }

  private:
    sao_ui_compositor_handle_t handle_{};
};

class TempDirectory final {
  public:
    TempDirectory() {
        static std::atomic<std::uint64_t> next{1};
        root = std::filesystem::temp_directory_path() /
               ("sao_workshop_panel_" + std::to_string(next.fetch_add(1)));
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root, error);
        REQUIRE_FALSE(error);
        root = std::filesystem::absolute(root);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path root;
};

struct FakeBackend {
    class ActiveCall final {
      public:
        ActiveCall(FakeBackend& backend, std::string call) : backend_(backend) {
            const int active_count = backend_.active.fetch_add(1) + 1;
            int observed = backend_.maximum_active.load();
            while (active_count > observed &&
                   !backend_.maximum_active.compare_exchange_weak(observed, active_count)) {
            }
            std::lock_guard lock(backend_.mutex);
            backend_.calls.push_back(std::move(call));
        }

        ~ActiveCall() {
            backend_.active.fetch_sub(1);
        }

      private:
        FakeBackend& backend_;
    };

    Operations operations() {
        Operations result;
        result.list = [this](std::stop_token stop, std::uint32_t page, std::uint32_t size,
                             PluginPage& output) -> sao_status_t {
            ActiveCall call(*this, "list:" + std::to_string(page));
            pause(stop);
            if (stop.stop_requested())
                return SAO_STATUS_ERR_CANCELLED;
            ++list_calls;
            output.page = page;
            output.size = size;
            output.total = 25;
            output.items = {summary("com.example.alpha", "Alpha Tools", "1.2.3"),
                            summary("com.example.beta", "Beta Overlay", "2.0.0")};
            return list_status.load();
        };
        result.detail = [this](std::stop_token stop, std::string_view id,
                               PluginDetail& output) -> sao_status_t {
            ActiveCall call(*this, "detail:" + std::string(id));
            pause(stop);
            if (stop.stop_requested())
                return SAO_STATUS_ERR_CANCELLED;
            output.summary = summary(std::string(id), "Alpha Tools", "1.2.3");
            output.description = "A focused fake workshop plugin used without network access.";
            output.sha256_hex = std::string(64, 'a');
            output.signature_algorithm = "ed25519";
            output.size_bytes = 8192;
            output.min_client_version_major = 5;
            return detail_status.load();
        };
        result.download = [this](std::stop_token stop, std::string_view id,
                                 const std::filesystem::path& target,
                                 std::filesystem::path& output) -> sao_status_t {
            ActiveCall call(*this, "download:" + std::string(id));
            pause(stop);
            {
                std::lock_guard lock(mutex);
                download_directory = target;
            }
            if (stop.stop_requested())
                return SAO_STATUS_ERR_CANCELLED;
            output = target / (std::string(id) + "-1.2.3.sao-plugin");
            return download_status.load();
        };
        result.verify = [this](std::stop_token stop, const std::filesystem::path& package,
                               std::string_view id) -> sao_status_t {
            ActiveCall call(*this, "verify:" + std::string(id));
            pause(stop);
            {
                std::lock_guard lock(mutex);
                verified_package = package;
            }
            return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : verify_status.load();
        };
        result.install = [this](std::stop_token stop, const std::filesystem::path& package,
                                const std::filesystem::path& plugins) -> sao_status_t {
            ActiveCall call(*this, "install");
            pause(stop);
            {
                std::lock_guard lock(mutex);
                installed_package = package;
                plugins_directory = plugins;
            }
            return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : install_status.load();
        };
        result.uninstall = [this](std::stop_token stop, std::string_view id,
                                  const std::filesystem::path& plugins,
                                  bool keep_backup) -> sao_status_t {
            ActiveCall call(*this, "uninstall:" + std::string(id));
            pause(stop);
            {
                std::lock_guard lock(mutex);
                plugins_directory = plugins;
                uninstall_keeps_backup = keep_backup;
            }
            return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : uninstall_status.load();
        };
        return result;
    }

    std::vector<std::string> call_snapshot() const {
        std::lock_guard lock(mutex);
        return calls;
    }

    void clear_calls() {
        std::lock_guard lock(mutex);
        calls.clear();
    }

    static PluginSummary summary(std::string id, std::string name, std::string version) {
        PluginSummary value;
        value.id = std::move(id);
        value.name = std::move(name);
        value.version = std::move(version);
        value.tag = "tools";
        value.author = "fixture";
        value.rating = 475;
        value.downloads = 42;
        return value;
    }

    void pause(std::stop_token stop) const {
        const auto until = std::chrono::steady_clock::now() + delay;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
    }

    mutable std::mutex mutex;
    std::vector<std::string> calls;
    std::filesystem::path download_directory;
    std::filesystem::path verified_package;
    std::filesystem::path installed_package;
    std::filesystem::path plugins_directory;
    std::chrono::milliseconds delay{8};
    std::atomic<int> active{};
    std::atomic<int> maximum_active{};
    std::atomic<std::uint32_t> list_calls{};
    std::atomic<sao_status_t> list_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> detail_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> download_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> verify_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> install_status{SAO_STATUS_OK};
    std::atomic<sao_status_t> uninstall_status{SAO_STATUS_OK};
    bool uninstall_keeps_backup{};
};

bool service_until(Owner& owner, const std::function<bool()>& predicate, int attempts = 1000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (owner.service_ui() != SAO_STATUS_OK)
            return false;
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool wait_until(const std::function<bool()>& predicate, int attempts = 1000) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

std::size_t panel_count() {
    std::size_t count = 0;
    REQUIRE(sao_ui_panel_registry_count(&count) == SAO_STATUS_OK);
    return count;
}

} // namespace

TEST_CASE("Workshop panel freezes the white-gold theme and native spec",
          "[launcher][workshop][focused][theme]") {
    const SaoPanelDescriptor descriptor = sao::launcher::workshop_panel::panel_descriptor();
    CHECK(std::string(descriptor.panel_id_utf8) == "sao.launcher.plugin_workshop");
    CHECK(std::string(descriptor.title_utf8) == "Plugin Workshop");
    CHECK(descriptor.movable);
    CHECK(descriptor.resizable);
    CHECK(descriptor.show_close_button);
    CHECK(descriptor.auto_scroll);
    CHECK_FALSE(descriptor.visible);

    const auto theme = nlohmann::json::parse(sao::launcher::workshop_panel::theme_override_json());
    REQUIRE(theme.contains("colors"));
    CHECK(theme["colors"]["APP_BG"] == "#FFFCF5");
    CHECK(theme["colors"]["APP_CARD"] == "#FFFFFF");
    CHECK(theme["colors"]["APP_BORDER"] == "#E2D5B0");
    CHECK(theme["colors"]["APP_GOLD"] == "#D4A520");
    CHECK(theme["colors"]["APP_ACCENT"] == "#2FA9B8");

    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    Owner owner(compositor.get(), base.root, backend.operations());
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));

    const auto snapshot = owner.snapshot();
    CHECK(snapshot.visible);
    CHECK(snapshot.items.size() == 2);
    CHECK(snapshot.last_spec.find("Plugin Workshop") != std::string::npos);
    CHECK(snapshot.last_spec.find("Catalog") != std::string::npos);
    CHECK(snapshot.last_spec.find("Task Center") != std::string::npos);
    CHECK(snapshot.last_spec.find("workshop.refresh") != std::string::npos);
    CHECK(snapshot.last_spec.find("workshop.page.previous") != std::string::npos);
    CHECK(snapshot.last_spec.find("workshop.page.next") != std::string::npos);
    CHECK(snapshot.last_spec.find("workshop.plugin.detail") != std::string::npos);
    CHECK(snapshot.last_spec.find("workshop.plugin.install") != std::string::npos);
    CHECK(snapshot.last_spec.find("workshop.plugin.uninstall") != std::string::npos);
    CHECK(snapshot.last_spec.find("completed") != std::string::npos);

    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
}

TEST_CASE("Workshop worker serializes queued disk and network operations",
          "[launcher][workshop][focused][queue]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    backend.delay = 15ms;
    Owner owner(compositor.get(), base.root, backend.operations());

    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("workshop.plugin.detail",
                                              R"({"id":"com.example.alpha"})") == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("workshop.plugin.install",
                                              R"({"id":"com.example.alpha"})") == SAO_STATUS_OK);
    REQUIRE(owner.dispatch_action_for_testing("workshop.plugin.uninstall",
                                              R"({"id":"com.example.alpha"})") == SAO_STATUS_OK);
    CHECK(owner.dispatch_action_for_testing("workshop.plugin.install", R"({"id":"../escape"})") ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 4; }));
    CHECK(backend.maximum_active.load() == 1);
    CHECK(backend.call_snapshot() == std::vector<std::string>{"list:1", "detail:com.example.alpha",
                                                              "download:com.example.alpha",
                                                              "verify:com.example.alpha", "install",
                                                              "uninstall:com.example.alpha"});
    CHECK(backend.download_directory == base.root / "cache" / "workshop");
    CHECK(backend.plugins_directory == base.root / "plugins");
    CHECK(backend.verified_package == backend.installed_package);
    CHECK(backend.uninstall_keeps_backup);
    CHECK(owner.snapshot().last_status == SAO_STATUS_OK);

    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
}

TEST_CASE("Workshop install chain stops at the first failed stage",
          "[launcher][workshop][focused][install_chain]") {
    const auto run_case = [](sao_status_t download_status, sao_status_t verify_status,
                             sao_status_t install_status, std::vector<std::string> expected_calls) {
        BoundCompositor compositor;
        TempDirectory base;
        FakeBackend backend;
        backend.download_status.store(download_status);
        backend.verify_status.store(verify_status);
        backend.install_status.store(install_status);
        Owner owner(compositor.get(), base.root, backend.operations());
        REQUIRE(owner.open() == SAO_STATUS_OK);
        REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));
        backend.clear_calls();

        REQUIRE(owner.dispatch_action_for_testing(
                    "workshop.plugin.install", R"({"id":"com.example.alpha"})") == SAO_STATUS_OK);
        REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 2; }));
        CHECK(backend.call_snapshot() == expected_calls);
        CHECK(owner.snapshot().last_status != SAO_STATUS_OK);
        CHECK_FALSE(owner.snapshot().error_text.empty());
        REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
    };

    SECTION("download failure skips verify and install") {
        run_case(SAO_STATUS_ERR_OS_CALL_FAILED, SAO_STATUS_OK, SAO_STATUS_OK,
                 {"download:com.example.alpha"});
    }
    SECTION("verify failure skips install") {
        run_case(SAO_STATUS_OK, SAO_STATUS_ERR_HANDLE_INVALID, SAO_STATUS_OK,
                 {"download:com.example.alpha", "verify:com.example.alpha"});
    }
    SECTION("install failure remains terminal") {
        run_case(SAO_STATUS_OK, SAO_STATUS_OK, SAO_STATUS_ERR_OS_CALL_FAILED,
                 {"download:com.example.alpha", "verify:com.example.alpha", "install"});
    }
}

TEST_CASE("Workshop owner reuses one panel across repeated opens",
          "[launcher][workshop][focused][single_instance]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    const std::size_t before = panel_count();
    std::vector<bool> visibility;

    Owner owner(compositor.get(), base.root, backend.operations());
    owner.set_visibility_changed_callback([&](bool visible) { visibility.push_back(visible); });
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));
    const sao_ui_panel_handle_t first = owner.snapshot().panel;
    REQUIRE(first != nullptr);
    CHECK(panel_count() == before + 1);

    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(owner.service_ui() == SAO_STATUS_OK);
    CHECK(owner.snapshot().panel == first);
    CHECK(panel_count() == before + 1);
    CHECK(backend.list_calls.load() == 1);

    REQUIRE(owner.hide() == SAO_STATUS_OK);
    CHECK_FALSE(owner.snapshot().visible);
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 2; }));
    CHECK(owner.snapshot().panel == first);
    CHECK(panel_count() == before + 1);
    CHECK(backend.list_calls.load() == 2);
    CHECK(visibility == std::vector<bool>{true, false, true});

    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
    CHECK(panel_count() == before);
}

TEST_CASE("Workshop foreign-thread teardown preserves the online worker and panel",
          "[launcher][workshop][focused][owner_thread]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    backend.delay = 250ms;
    Owner owner(compositor.get(), base.root, backend.operations());

    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(wait_until([&] { return backend.active.load() == 1; }));
    const sao_ui_panel_handle_t panel = owner.snapshot().panel;
    REQUIRE(panel != nullptr);

    std::atomic<sao_status_t> foreign_status{SAO_STATUS_OK};
    std::thread foreign([&] { foreign_status.store(owner.try_take_offline()); });
    foreign.join();

    CHECK(foreign_status.load() == SAO_STATUS_ERR_ACCESS_DENIED);
    const auto after_foreign = owner.snapshot();
    CHECK(after_foreign.online);
    CHECK(after_foreign.panel == panel);
    CHECK(after_foreign.visible);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));
    CHECK(backend.list_calls.load() == 1);
    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
}

TEST_CASE("Workshop close events defer owner-thread hide and publish exact visibility",
          "[launcher][workshop][focused][close_event][visibility]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    Owner owner(compositor.get(), base.root, backend.operations());
    std::vector<bool> first_callback;
    std::vector<bool> replacement_callback;

    owner.set_visibility_changed_callback([&](bool visible) { first_callback.push_back(visible); });
    CHECK(first_callback.empty());
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));
    CHECK(first_callback == std::vector<bool>{true});

    owner.set_visibility_changed_callback(
        [&](bool visible) { replacement_callback.push_back(visible); });
    CHECK(replacement_callback == std::vector<bool>{true});

    std::atomic<sao_status_t> event_status{SAO_STATUS_ERR_UNKNOWN};
    std::thread event_thread([&] {
        event_status.store(owner.dispatch_panel_event_for_testing(SAO_UI_PANEL_EVENT_CLOSE));
    });
    event_thread.join();
    CHECK(event_status.load() == SAO_STATUS_OK);
    CHECK(owner.snapshot().visible);

    REQUIRE(owner.service_ui() == SAO_STATUS_OK);
    CHECK_FALSE(owner.snapshot().visible);
    CHECK(first_callback == std::vector<bool>{true});
    CHECK(replacement_callback == std::vector<bool>{true, false});

    REQUIRE(owner.dispatch_panel_event_for_testing(SAO_UI_PANEL_EVENT_CLOSE) == SAO_STATUS_OK);
    REQUIRE(owner.service_ui() == SAO_STATUS_OK);
    CHECK(replacement_callback == std::vector<bool>{true, false});

    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 2; }));
    CHECK(replacement_callback == std::vector<bool>{true, false, true});
    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
    CHECK(replacement_callback == std::vector<bool>{true, false, true, false});
}

TEST_CASE("Workshop offline retirement is retryable after unregister failure",
          "[launcher][workshop][focused][teardown][retry]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    const std::size_t before = panel_count();
    Owner owner(compositor.get(), base.root, backend.operations());

    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));
    const sao_ui_panel_handle_t panel = owner.snapshot().panel;
    REQUIRE(panel != nullptr);
    owner.fail_next_unregister_for_testing(SAO_STATUS_ERR_OS_CALL_FAILED);

    CHECK(owner.try_take_offline() == SAO_STATUS_ERR_OS_CALL_FAILED);
    const auto failed = owner.snapshot();
    CHECK_FALSE(failed.online);
    CHECK_FALSE(failed.visible);
    CHECK_FALSE(failed.busy);
    CHECK(failed.panel == panel);
    CHECK(panel_count() == before + 1);
    CHECK(owner.dispatch_action_for_testing("workshop.refresh") == SAO_STATUS_ERR_NOT_INITIALIZED);

    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
    CHECK(owner.snapshot().panel == nullptr);
    CHECK(panel_count() == before);
    CHECK(owner.try_take_offline() == SAO_STATUS_OK);
}

TEST_CASE("Workshop teardown rejects visibility callback reentry",
          "[launcher][workshop][focused][teardown][reentry]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    const std::size_t before = panel_count();
    Owner owner(compositor.get(), base.root, backend.operations());
    std::vector<sao_status_t> reentrant_statuses;

    owner.set_visibility_changed_callback([&](bool visible) {
        if (!visible)
            reentrant_statuses.push_back(owner.try_take_offline());
    });
    REQUIRE(owner.open() == SAO_STATUS_OK);
    REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));

    REQUIRE(owner.try_take_offline() == SAO_STATUS_OK);
    CHECK(reentrant_statuses == std::vector<sao_status_t>{SAO_UI_PANEL_STATUS_ERR_BUSY});
    CHECK(owner.snapshot().panel == nullptr);
    CHECK(panel_count() == before);
}

TEST_CASE("Workshop destructor retries a transient unregister failure without losing the handle",
          "[launcher][workshop][focused][destructor][retry]") {
    BoundCompositor compositor;
    TempDirectory base;
    FakeBackend backend;
    const std::size_t before = panel_count();

    {
        Owner owner(compositor.get(), base.root, backend.operations());
        REQUIRE(owner.open() == SAO_STATUS_OK);
        REQUIRE(service_until(owner, [&] { return owner.snapshot().completed_operations >= 1; }));
        owner.fail_next_unregister_for_testing(SAO_STATUS_ERR_OS_CALL_FAILED);
    }

    CHECK(panel_count() == before);
}
