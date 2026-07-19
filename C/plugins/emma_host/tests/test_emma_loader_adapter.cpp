#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/emma_host/emma_call.h"
#include "sao/plugins/emma_host/emma_interpreter.h"
#include "sao/plugins/emma_host/emma_loader_adapter.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using namespace sao::plugins::emma_host;
using namespace sao::plugins::loader;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class temp_tree final {
  public:
    explicit temp_tree(const wchar_t* suffix) {
        static std::atomic_uint64_t sequence{0};
        root = fs::temp_directory_path() /
               (L"sao_emma_adapter_" + std::to_wstring(GetCurrentProcessId()) + L"_" + suffix +
                L"_" + std::to_wstring(sequence.fetch_add(1)));
        std::error_code error;
        fs::remove_all(root, error);
        REQUIRE(fs::create_directories(root));
    }

    ~temp_tree() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
};

std::string path_utf8(const fs::path& path) {
    const std::wstring wide = path.native();
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string result(static_cast<size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), result.data(), required, nullptr,
                                nullptr) == required);
    return result;
}

void write_text(const fs::path& path, const std::string& text) {
    REQUIRE((fs::create_directories(path.parent_path()) || fs::is_directory(path.parent_path())));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(const temp_tree& tree, const char* plugin_id,
                              const char* entry = "nested/plugin.emma") {
    plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = entry;
    manifest.language = engine_kind::emma;
    manifest.enabled = false;
    manifest.source_path = path_utf8(tree.root);
    return manifest;
}

plugin_handle_t add_plugin(const plugin_manifest& manifest) {
    plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &plugin) ==
            SAO_OK);
    REQUIRE(plugin != nullptr);
    return plugin;
}

void remove_plugin(plugin_handle_t plugin) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), plugin) == SAO_OK);
}

struct menu_row_snapshot {
    std::string label;
    std::string icon;
    std::string action_id;
    std::string payload;
    bool can_activate = false;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const menu_row_snapshot&) const = default;
};

struct menu_provider_snapshot {
    std::string provider_id;
    std::string owner_id;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::vector<menu_row_snapshot> rows;

    bool operator==(const menu_provider_snapshot&) const = default;
};

struct menu_root_snapshot {
    std::string owner_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::vector<std::pair<std::string, std::string>> actions;

    bool operator==(const menu_root_snapshot&) const = default;
};

struct menu_catalog_snapshot {
    std::vector<menu_provider_snapshot> providers;
    std::vector<menu_root_snapshot> roots;

    bool operator==(const menu_catalog_snapshot&) const = default;
};

int32_t SAO_PLUGINS_CALL copy_menu_catalog(const entity_provider_catalog_view* view,
                                           void* user_data) {
    if (view == nullptr || user_data == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    menu_catalog_snapshot candidate;
    for (std::uint32_t provider_index = 0; provider_index < view->provider_count;
         ++provider_index) {
        const auto& source = view->providers[provider_index];
        menu_provider_snapshot provider;
        provider.provider_id = source.provider_id_utf8;
        provider.owner_id = source.owner_plugin_id_utf8;
        provider.generation = source.generation;
        provider.revision = source.revision;
        for (std::uint32_t row_index = 0; row_index < source.row_count; ++row_index) {
            const auto& row = source.rows[row_index];
            provider.rows.push_back({row.row_label_utf8, row.row_icon_utf8, row.action_id_utf8,
                                     row.payload_json_utf8, row.can_activate != 0,
                                     row.keep_menu_open != 0, row.close_menu_before != 0});
        }
        candidate.providers.push_back(std::move(provider));
    }
    for (std::uint32_t root_index = 0; root_index < view->root_contribution_count; ++root_index) {
        const auto& source = view->root_contributions[root_index];
        menu_root_snapshot root;
        root.owner_id = source.owner_plugin_id_utf8;
        root.contribution_id = source.contribution_id_utf8;
        root.root_id = source.root_id_utf8;
        root.name = source.name_utf8;
        root.icon = source.icon_utf8;
        root.priority = source.priority;
        for (std::uint32_t action_index = 0; action_index < source.action_count; ++action_index) {
            root.actions.emplace_back(source.actions[action_index].provider_id_utf8,
                                      source.actions[action_index].action_id_utf8);
        }
        candidate.roots.push_back(std::move(root));
    }
    *static_cast<menu_catalog_snapshot*>(user_data) = std::move(candidate);
    return SAO_OK;
}

int32_t snapshot_menu_catalog(menu_catalog_snapshot& output) {
    return sao_plugins_entity_provider_snapshot(copy_menu_catalog, &output);
}

std::string stable_suffix(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

void set_context_json(plugin_context_t* context, const char* key, std::string_view value) {
    REQUIRE(sao_plugins_ctx_set_setting(context, key, std::string(value).c_str()) == SAO_OK);
}

std::string context_json(plugin_context_t* context, const char* key) {
    char* value = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, key, &value) == SAO_OK);
    REQUIRE(value != nullptr);
    std::string result(value);
    sao_plugins_ctx_free_string(value);
    return result;
}

bool has_panel(const char* plugin_id, const char* panel_id) {
    const auto panels =
        snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel);
    for (const auto& panel : panels) {
        if (panel.plugin_id == plugin_id && panel.id == panel_id)
            return true;
    }
    return false;
}

class direct_hook_gate final {
  public:
    emma_value wait(std::vector<emma_value>) {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return int64_t{7};
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

int32_t SAO_PLUGINS_CALL install_direct_hook_gate(interpreter* interp, void* user_data) {
    if (interp == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* gate = static_cast<direct_hook_gate*>(user_data);
    auto callable = std::make_shared<sao::plugins::emma_host::callable>();
    callable->name = "wait_for_unload";
    callable->host_impl = [gate](std::vector<emma_value> arguments) {
        return gate->wait(std::move(arguments));
    };
    interp->register_global("wait_for_unload", std::move(callable));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL reenter_direct_plugin(interpreter*, void* user_data) {
    auto plugin = *static_cast<emma_plugin_handle_t*>(user_data);
    return sao_plugins_emma_call_hook(plugin, "quick_hook", "[]", nullptr);
}

} // namespace

TEST_CASE("Emma direct unload closes active hooks without UAF",
          "[plugins][emma][direct][concurrency]") {
    temp_tree tree(L"direct_concurrency");
    write_text(tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn blocking_hook()
    return wait_for_unload()
end
fn quick_hook()
    return 1
end
)EMMA");
    const auto manifest = make_manifest(tree, "emma.direct.concurrent");
    plugin_handle_t loader_plugin = add_plugin(manifest);
    plugin_context_t* context = sao_plugins_ctx_create(loader_plugin);
    REQUIRE(context != nullptr);

    emma_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(tree.root.c_str(), manifest.entry.c_str(),
                                         manifest.plugin_id.c_str(), context, &plugin) == SAO_OK);
    REQUIRE(plugin != nullptr);

    direct_hook_gate gate;
    REQUIRE(sao_plugins_emma_with_interpreter(plugin, install_direct_hook_gate, &gate) == SAO_OK);
    CHECK(sao_plugins_emma_with_interpreter(plugin, reenter_direct_plugin, &plugin) ==
          SAO_PLUGINS_ERR_BUSY);

    std::atomic_int hook_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread hook_thread([&] {
        hook_status.store(sao_plugins_emma_call_hook(plugin, "blocking_hook", "[]", nullptr));
    });
    REQUIRE(gate.wait_until_entered(std::chrono::seconds(2)));

    CHECK(sao_plugins_emma_unload_script(plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_emma_call_hook(plugin, "quick_hook", "[]", nullptr) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_emma_call_on_enable(plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK_FALSE(sao_plugins_emma_has_hook(plugin, "quick_hook"));
    CHECK(sao_plugins_emma_with_interpreter(plugin, install_direct_hook_gate, &gate) ==
          SAO_PLUGINS_ERR_BUSY);

    gate.release();
    hook_thread.join();
    REQUIRE(hook_status.load() == SAO_OK);

    std::barrier start{3};
    std::atomic_int first_unload{SAO_ERR_OS_CALL_FAILED};
    std::atomic_int second_unload{SAO_ERR_OS_CALL_FAILED};
    std::jthread first([&] {
        start.arrive_and_wait();
        first_unload.store(sao_plugins_emma_unload_script(plugin));
    });
    std::jthread second([&] {
        start.arrive_and_wait();
        second_unload.store(sao_plugins_emma_unload_script(plugin));
    });
    start.arrive_and_wait();
    first.join();
    second.join();

    const int32_t first_status = first_unload.load();
    const int32_t second_status = second_unload.load();
    CHECK(((first_status == SAO_OK &&
            (second_status == SAO_ERR_HANDLE_INVALID || second_status == SAO_PLUGINS_ERR_BUSY)) ||
           (second_status == SAO_OK &&
            (first_status == SAO_ERR_HANDLE_INVALID || first_status == SAO_PLUGINS_ERR_BUSY))));
    CHECK(sao_plugins_emma_unload_script(plugin) == SAO_ERR_HANDLE_INVALID);
    CHECK(sao_plugins_emma_call_on_disable(plugin) == SAO_ERR_HANDLE_INVALID);

    sao_plugins_ctx_destroy(context);
    remove_plugin(loader_plugin);
}

TEST_CASE("Emma menu category adapter preserves canonical identity and lifecycle",
          "[plugins][emma][adapter][menu]") {
    temp_tree tree(L"menu_identity");
    write_text(tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn shared_command()
    if ctx.get_setting("fail_action", false)
        missing_action()
    end
    ctx.set_setting("action_invoked", true)
end
fn build_menu()
    let mode = ctx.get_setting("mode", 0)
    if mode == 0
        return [
            {action_id: "explicit", label: "显式", icon: "界", command: shared_command,
             payload_json: "{\"raw\":true}", keep_menu_open: true},
            {label: "共享 A", icon: "A", command: shared_command,
             payload: {slot: "a"}, close_menu_before: true},
            {label: "共享 A", icon: "A", command: shared_command,
             payload: {slot: "a"}, close_menu_before: true},
            {label: "共享 B", icon: "B", command: shared_command,
             payload: {slot: "b"}, can_activate: false}
        ]
    end
    if mode == 1
        return [
            {label: "插入项", icon: "I", command: shared_command},
            {action_id: "explicit", label: "显式", icon: "界", command: shared_command,
             payload_json: "{\"raw\":true}", keep_menu_open: true},
            {label: "共享 A", icon: "A", command: shared_command,
             payload: {slot: "a"}, close_menu_before: true},
            {label: "共享 A", icon: "A", command: shared_command,
             payload: {slot: "a"}, close_menu_before: true},
            {label: "共享 B", icon: "B", command: shared_command,
             payload: {slot: "b"}, can_activate: false}
        ]
    end
    if mode == 2
        return [
            {label: "共享 B", icon: "B", command: shared_command,
             payload: {slot: "b"}, can_activate: false},
            {action_id: "explicit", label: "显式", icon: "界", command: shared_command,
             payload_json: "{\"raw\":true}", keep_menu_open: true},
            {label: "共享 A", icon: "A", command: shared_command,
             payload: {slot: "a"}, close_menu_before: true},
            {label: "共享 A", icon: "A", command: shared_command,
             payload: {slot: "a"}, close_menu_before: true}
        ]
    end
    if mode == 3
        missing_builder()
    end
    return []
end
fn on_load(ctx)
    ctx.register_menu_category("工具 α", "⚙", build_menu, 10.5)
end
)EMMA");

    emma_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_emma_register_loader_adapter(&owner) == SAO_OK);
    const auto manifest = make_manifest(tree, "emma.menu.identity");
    plugin_handle_t plugin = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(plugin) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(plugin, &context) == SAO_OK);

    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    REQUIRE(sao_plugins_lifecycle_enable(plugin) == SAO_OK);
    std::atomic_int snapshot_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread worker([&] { snapshot_status = snapshot_menu_catalog(catalog); });
    worker.join();
    REQUIRE(snapshot_status.load() == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.roots.size() == 1);
    const std::string local_id = "menu-" + stable_suffix("工具 α");
    const auto initial = catalog;
    const auto& provider = initial.providers.front();
    CHECK(provider.provider_id == manifest.plugin_id + "/" + local_id);
    CHECK(provider.owner_id == manifest.plugin_id);
    CHECK(provider.generation > 0);
    CHECK(provider.revision == 1);
    REQUIRE(provider.rows.size() == 4);
    CHECK(provider.rows[0].payload == "{\"raw\":true}");
    CHECK(provider.rows[0].keep_menu_open);
    CHECK(provider.rows[1].close_menu_before);
    CHECK_FALSE(provider.rows[3].can_activate);
    CHECK(initial.roots[0].contribution_id == local_id);
    CHECK(initial.roots[0].root_id == "plugin:" + stable_suffix(manifest.plugin_id + "\n工具 α"));
    CHECK(initial.roots[0].name == "工具 α");
    CHECK(initial.roots[0].icon == "⚙");
    CHECK(initial.roots[0].priority == 10.5);
    REQUIRE(initial.roots[0].actions.size() == 4);

    const std::string explicit_action = provider.rows[0].action_id;
    const std::string shared_a0 = provider.rows[1].action_id;
    const std::string shared_a1 = provider.rows[2].action_id;
    const std::string shared_b = provider.rows[3].action_id;
    CHECK(explicit_action == "menu-action-" + stable_suffix(local_id + "\nexplicit"));
    CHECK(shared_a0 != shared_a1);
    CHECK(shared_a0 != shared_b);
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             provider.rows[0].payload.c_str()) == SAO_OK);
    CHECK(context_json(context, "action_invoked") == "true");
    set_context_json(context, "fail_action", "true");
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             "{}") == SAO_ERR_OS_CALL_FAILED);
    set_context_json(context, "fail_action", "false");
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             "menu-action-0000000000000000",
                                             "{}") == SAO_ERR_HANDLE_INVALID);

    set_context_json(context, "mode", "1");
    menu_catalog_snapshot inserted;
    REQUIRE(snapshot_menu_catalog(inserted) == SAO_OK);
    REQUIRE(inserted.providers[0].rows.size() == 5);
    CHECK(inserted.providers[0].revision == 2);
    CHECK(inserted.providers[0].rows[1].action_id == explicit_action);
    CHECK(inserted.providers[0].rows[2].action_id == shared_a0);
    CHECK(inserted.providers[0].rows[3].action_id == shared_a1);
    CHECK(inserted.providers[0].rows[4].action_id == shared_b);
    set_context_json(context, "mode", "2");
    menu_catalog_snapshot reordered;
    REQUIRE(snapshot_menu_catalog(reordered) == SAO_OK);
    CHECK(reordered.providers[0].revision == 3);
    CHECK(reordered.providers[0].rows[0].action_id == shared_b);
    CHECK(reordered.providers[0].rows[1].action_id == explicit_action);
    set_context_json(context, "mode", "3");
    const auto committed = reordered;
    CHECK(snapshot_menu_catalog(reordered) == SAO_ERR_OS_CALL_FAILED);
    CHECK(reordered == committed);
    set_context_json(context, "mode", "2");
    REQUIRE(snapshot_menu_catalog(reordered) == SAO_OK);
    CHECK(reordered.providers[0].revision == 3);
    set_context_json(context, "mode", "4");
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers[0].rows.empty());
    CHECK(catalog.providers[0].revision == 4);
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(), "{}") == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_disable(plugin) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             "{}") == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(plugin) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers[0].generation == provider.generation);
    REQUIRE(sao_plugins_lifecycle_unload(plugin) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    remove_plugin(plugin);
    REQUIRE(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_OK);
}

TEST_CASE("Emma menu registrations roll back failed lifecycle generations",
          "[plugins][emma][adapter][menu][rollback]") {
    emma_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_emma_register_loader_adapter(&owner) == SAO_OK);

    temp_tree failed_tree(L"menu_failed_load");
    write_text(failed_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn build_menu()
    return [{label: "failed row", command: fn() return true end}]
end
fn on_load(ctx)
    ctx.register_menu_category("Failed menu", "F", build_menu, 1.0)
    missing_load()
end
)EMMA");
    const auto failed_manifest = make_manifest(failed_tree, "emma.menu.failed-load");
    plugin_handle_t failed_plugin = add_plugin(failed_manifest);
    CHECK(sao_plugins_lifecycle_load(failed_plugin) == SAO_ERR_OS_CALL_FAILED);
    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(failed_plugin);

    temp_tree enable_tree(L"menu_enable_scope");
    write_text(enable_tree.root / L"nested" / L"plugin.emma", R"EMMA(
let saved = nil
fn command()
    saved.set_setting("enable_action", true)
end
fn build_menu()
    return [{label: "enabled row", command: command}]
end
fn on_load(ctx)
    saved = ctx
end
fn on_enable()
    saved.register_menu_category("Enabled menu", "E", build_menu, 2.0)
    if saved.get_setting("fail_enable", true)
        missing_enable()
    end
end
)EMMA");
    const auto enable_manifest = make_manifest(enable_tree, "emma.menu.enable-scope");
    plugin_handle_t enable_plugin = add_plugin(enable_manifest);
    REQUIRE(sao_plugins_lifecycle_load(enable_plugin) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(enable_plugin, &context) == SAO_OK);
    CHECK(sao_plugins_lifecycle_enable(enable_plugin) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(enable_plugin) == lifecycle_state::loaded_disabled);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());

    set_context_json(context, "fail_enable", "false");
    REQUIRE(sao_plugins_lifecycle_enable(enable_plugin) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    const auto first_generation = catalog.providers[0];
    const std::string expected_provider =
        enable_manifest.plugin_id + "/menu-" + stable_suffix("Enabled menu");
    CHECK(first_generation.provider_id == expected_provider);
    CHECK(sao_plugins_entity_provider_invoke(expected_provider.c_str(), first_generation.generation,
                                             first_generation.rows[0].action_id.c_str(),
                                             "{}") == SAO_OK);
    CHECK(context_json(context, "enable_action") == "true");

    REQUIRE(sao_plugins_lifecycle_disable(enable_plugin) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(sao_plugins_entity_provider_invoke(expected_provider.c_str(), first_generation.generation,
                                             first_generation.rows[0].action_id.c_str(),
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_enable(enable_plugin) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.providers[0].provider_id == expected_provider);
    CHECK(catalog.providers[0].generation != first_generation.generation);
    const auto current_generation = catalog.providers[0];
    REQUIRE(sao_plugins_lifecycle_unload(enable_plugin) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke(
              expected_provider.c_str(), current_generation.generation,
              current_generation.rows[0].action_id.c_str(), "{}") == SAO_ERR_HANDLE_INVALID);
    remove_plugin(enable_plugin);
    REQUIRE(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_OK);
}

TEST_CASE("Emma menu snapshots reject invalid candidates atomically",
          "[plugins][emma][adapter][menu][budget]") {
    temp_tree tree(L"menu_budget");
    write_text(tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn command()
    return true
end
fn build_menu()
    let mode = ctx.get_setting("mode", 0)
    if mode == 0
        return [{action_id: "baseline", label: "baseline",
                 payload_json: "{\"raw\":true}"}]
    end
    if mode == 1
        return [{label: ctx.get_setting("oversized", "")}]
    end
    if mode == 2
        return ctx.get_setting("too_many", [])
    end
    if mode == 3
        return ctx.get_setting("oversized_snapshot", [])
    end
    if mode == 4
        return [{label: "flag", keep_menu_open: "false"}]
    end
    if mode == 5
        return [{label: "payload", payload_json: "{invalid"}]
    end
    if mode == 6
        let rows = ctx.get_setting("remembered", [])
        for row in rows
            row.command = command
        end
        return rows
    end
    if mode == 7
        return [{action_id: "overflow", label: "overflow", command: command}]
    end
    if mode == 8
        return [
            {action_id: "collision", label: "first"},
            {action_id: "collision", label: "second"}
        ]
    end
    return [{label: "not actionable", can_activate: true}]
end
fn on_load(ctx)
    ctx.register_menu_category("Budget", "", build_menu)
end
)EMMA");

    emma_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_emma_register_loader_adapter(&owner) == SAO_OK);
    const auto manifest = make_manifest(tree, "emma.menu.budget");
    plugin_handle_t plugin = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(plugin) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(plugin, &context) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(plugin) == SAO_OK);

    set_context_json(context, "oversized", json(std::string(16385, 'x')).dump());
    json too_many = json::array();
    for (std::size_t index = 0; index < 4097; ++index)
        too_many.push_back({{"label", "row"}});
    set_context_json(context, "too_many", too_many.dump());
    json oversized_snapshot = json::array();
    const std::string large_payload = json(std::string(2046, 'x')).dump();
    for (std::size_t index = 0; index < 4096; ++index) {
        oversized_snapshot.push_back({{"action_id", "large-" + std::to_string(index)},
                                      {"label", "row"},
                                      {"payload_json", large_payload}});
    }
    set_context_json(context, "oversized_snapshot", oversized_snapshot.dump());
    json remembered = json::array();
    for (std::size_t index = 0; index < 4096; ++index) {
        remembered.push_back(
            {{"action_id", "remembered-" + std::to_string(index)}, {"label", "row"}});
    }
    set_context_json(context, "remembered", remembered.dump());

    menu_catalog_snapshot baseline;
    REQUIRE(snapshot_menu_catalog(baseline) == SAO_OK);
    REQUIRE(baseline.providers.size() == 1);
    REQUIRE(baseline.providers[0].rows.size() == 1);
    CHECK(baseline.providers[0].revision == 1);
    CHECK(baseline.providers[0].rows[0].payload == "{\"raw\":true}");
    for (int mode = 1; mode <= 5; ++mode) {
        set_context_json(context, "mode", std::to_string(mode));
        menu_catalog_snapshot candidate = baseline;
        CHECK(snapshot_menu_catalog(candidate) == SAO_ERR_OS_CALL_FAILED);
        CHECK(candidate == baseline);
    }
    set_context_json(context, "mode", "8");
    menu_catalog_snapshot collision = baseline;
    CHECK(snapshot_menu_catalog(collision) == SAO_ERR_OS_CALL_FAILED);
    CHECK(collision == baseline);

    set_context_json(context, "mode", "6");
    menu_catalog_snapshot at_limit;
    REQUIRE(snapshot_menu_catalog(at_limit) == SAO_OK);
    REQUIRE(at_limit.providers.size() == 1);
    REQUIRE(at_limit.providers[0].rows.size() == 4096);
    CHECK(at_limit.providers[0].revision == 2);
    set_context_json(context, "mode", "7");
    menu_catalog_snapshot overflow = at_limit;
    CHECK(snapshot_menu_catalog(overflow) == SAO_ERR_OS_CALL_FAILED);
    CHECK(overflow == at_limit);

    set_context_json(context, "mode", "9");
    menu_catalog_snapshot not_actionable;
    REQUIRE(snapshot_menu_catalog(not_actionable) == SAO_OK);
    REQUIRE(not_actionable.providers[0].rows.size() == 1);
    CHECK_FALSE(not_actionable.providers[0].rows[0].can_activate);
    CHECK(not_actionable.providers[0].revision == 3);

    REQUIRE(sao_plugins_lifecycle_unload(plugin) == SAO_OK);
    remove_plugin(plugin);
    REQUIRE(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_OK);
}

TEST_CASE("Emma generic loader adapter closes lifecycle, context and JSON",
          "[plugins][emma][adapter]") {
    emma_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_emma_register_loader_adapter(&owner) == SAO_OK);
    REQUIRE(owner != nullptr);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);

    emma_loader_adapter_owner_t duplicate = reinterpret_cast<emma_loader_adapter_owner_t>(1);
    CHECK(sao_plugins_emma_register_loader_adapter(&duplicate) == SAO_PLUGINS_ERR_ALREADY_EXISTS);
    CHECK(duplicate == nullptr);
    CHECK(sao_plugins_emma_unregister_loader_adapter(nullptr) == SAO_ERR_INVALID_ARGUMENT);

    temp_tree lifecycle_tree(L"lifecycle");
    write_text(lifecycle_tree.root / L"nested" / L"actual-entry.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("emma adapter loaded")
    ctx.register_ui_panel("main", {title: "Emma Panel"}, nil, nil)
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
    auto lifecycle_manifest =
        make_manifest(lifecycle_tree, "emma.adapter.lifecycle", "nested/actual-entry.emma");
    plugin_handle_t lifecycle_plugin = add_plugin(lifecycle_manifest);

    REQUIRE(sao_plugins_lifecycle_load(lifecycle_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(lifecycle_plugin) == lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 1);
    CHECK(has_panel("emma.adapter.lifecycle", "main"));
    CHECK(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(lifecycle_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_disable(lifecycle_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(lifecycle_plugin) == SAO_OK);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    CHECK_FALSE(has_panel("emma.adapter.lifecycle", "main"));
    remove_plugin(lifecycle_plugin);

    temp_tree veto_tree(L"veto");
    write_text(veto_tree.root / L"nested" / L"plugin.emma", R"EMMA(
let unload_attempts = 0
fn on_load(ctx)
    ctx.log("veto fixture loaded")
end
fn on_unload()
    unload_attempts = unload_attempts + 1
    return unload_attempts > 1
end
)EMMA");
    const auto veto_manifest = make_manifest(veto_tree, "emma.adapter.veto");
    plugin_handle_t veto_plugin = add_plugin(veto_manifest);
    REQUIRE(sao_plugins_lifecycle_load(veto_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_unload(veto_plugin) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(veto_plugin) == lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 1);
    REQUIRE(sao_plugins_lifecycle_unload(veto_plugin) == SAO_OK);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(veto_plugin);

    temp_tree failed_tree(L"failed");
    write_text(failed_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn on_load(ctx)
    ctx.register_ui_panel("rollback", {title: "Rollback"}, nil, nil)
    missing_function()
end
)EMMA");
    const auto failed_manifest = make_manifest(failed_tree, "emma.adapter.failed");
    plugin_handle_t failed_plugin = add_plugin(failed_manifest);
    CHECK(sao_plugins_lifecycle_load(failed_plugin) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(failed_plugin) == lifecycle_state::failed);
    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    CHECK_FALSE(has_panel("emma.adapter.failed", "rollback"));
    plugin_context_t* failed_context = reinterpret_cast<plugin_context_t*>(1);
    CHECK(sao_plugins_lifecycle_get_context(failed_plugin, &failed_context) ==
          SAO_ERR_NOT_INITIALIZED);
    CHECK(failed_context == nullptr);
    char* adapter_error = nullptr;
    REQUIRE(sao_plugins_emma_loader_adapter_get_last_error(owner, failed_plugin, &adapter_error) ==
            SAO_OK);
    REQUIRE(adapter_error != nullptr);
    INFO(adapter_error);
    CHECK(std::string(adapter_error).find("[runtime]") != std::string::npos);
    sao_plugins_emma_free_string(adapter_error);
    remove_plugin(failed_plugin);

    temp_tree json_tree(L"json");
    write_text(json_tree.root / L"nested" / L"json.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("direct JSON fixture loaded")
end
fn echo(number, object, items)
    return {number: number, object: object, items: items}
end
fn unsupported_result()
    return unsupported_result
end
)EMMA");
    const auto json_manifest = make_manifest(json_tree, "emma.adapter.json", "nested/json.emma");
    plugin_handle_t json_loader_plugin = add_plugin(json_manifest);
    plugin_context_t* direct_context = sao_plugins_ctx_create(json_loader_plugin);
    REQUIRE(direct_context != nullptr);
    emma_plugin_handle_t direct_plugin = nullptr;
    REQUIRE(sao_plugins_emma_load_script(json_tree.root.c_str(), "nested/json.emma",
                                         json_manifest.plugin_id.c_str(), direct_context,
                                         &direct_plugin) == SAO_OK);
    REQUIRE(direct_plugin != nullptr);
    REQUIRE(sao_plugins_emma_call_on_load(direct_plugin) == SAO_OK);

    char* result = nullptr;
    REQUIRE(sao_plugins_emma_call_hook(direct_plugin, "echo",
                                       R"([7,{"name":"Aldina"},[true,null,2.5]])",
                                       &result) == SAO_OK);
    REQUIRE(result != nullptr);
    const json parsed = json::parse(result);
    sao_plugins_emma_free_string(result);
    result = nullptr;
    CHECK(parsed ==
          json{{"number", 7}, {"object", {{"name", "Aldina"}}}, {"items", {true, nullptr, 2.5}}});
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "echo", "{bad", &result) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(result == nullptr);
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "echo", "{}", &result) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "missing", "[]", &result) ==
          SAO_ERR_HANDLE_INVALID);
    CHECK(sao_plugins_emma_call_hook(direct_plugin, "unsupported_result", "[]", &result) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(result == nullptr);
    REQUIRE(sao_plugins_emma_unload_script(direct_plugin) == SAO_OK);
    sao_plugins_ctx_destroy(direct_context);
    remove_plugin(json_loader_plugin);

    temp_tree concurrent_tree(L"concurrent");
    write_text(concurrent_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("slow fixture loaded")
end
fn on_enable()
    ctx.register_ui_panel("slow_started", {title: "Slow Started"}, nil, nil)
    let index = 0
    while index < 2000000
        index = index + 1
    end
end
)EMMA");
    const auto concurrent_manifest = make_manifest(concurrent_tree, "emma.adapter.concurrent");
    plugin_handle_t concurrent_plugin = add_plugin(concurrent_manifest);
    REQUIRE(sao_plugins_lifecycle_load(concurrent_plugin) == SAO_OK);

    temp_tree quick_tree(L"quick");
    write_text(quick_tree.root / L"nested" / L"plugin.emma", R"EMMA(
fn on_load(ctx)
    ctx.log("quick fixture loaded")
end
fn on_enable()
    return true
end
)EMMA");
    const auto quick_manifest = make_manifest(quick_tree, "emma.adapter.quick");
    plugin_handle_t quick_plugin = add_plugin(quick_manifest);
    REQUIRE(sao_plugins_lifecycle_load(quick_plugin) == SAO_OK);

    std::atomic_int concurrent_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread concurrent_call(
        [&] { concurrent_status.store(sao_plugins_lifecycle_enable(concurrent_plugin)); });
    const auto state_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!has_panel("emma.adapter.concurrent", "slow_started") &&
           std::chrono::steady_clock::now() < state_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(has_panel("emma.adapter.concurrent", "slow_started"));
    REQUIRE(sao_plugins_lifecycle_state(concurrent_plugin) == lifecycle_state::enabling);
    CHECK(sao_plugins_lifecycle_unload(concurrent_plugin) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(quick_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(concurrent_plugin) == lifecycle_state::enabling);
    concurrent_call.join();
    CHECK(concurrent_status.load() == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(quick_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(concurrent_plugin) == SAO_OK);
    remove_plugin(quick_plugin);
    remove_plugin(concurrent_plugin);

    CHECK(sao_plugins_emma_loader_adapter_plugin_count(owner) == 0);
    REQUIRE(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_OK);
    CHECK(sao_plugins_emma_unregister_loader_adapter(owner) == SAO_ERR_HANDLE_INVALID);
}
