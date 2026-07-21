#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/csharp_host/cs_loader_adapter.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace sao::plugins::csharp_host;
using namespace sao::plugins::loader;
namespace fs = std::filesystem;

#ifndef SAO_HELLO_CSHARP_DIR
#define SAO_HELLO_CSHARP_DIR "plugins/examples/hello_csharp"
#endif
#if !defined(SAO_MANAGED_ENTITY_A_DIR) || !defined(SAO_MANAGED_ENTITY_B_DIR)
#define SAO_MANAGED_ENTITY_FIXTURE_PATHS_MISSING 1
#define SAO_MANAGED_ENTITY_A_DIR ""
#define SAO_MANAGED_ENTITY_B_DIR ""
#endif

namespace {

std::string path_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

plugin_manifest hello_manifest(std::string id) {
    plugin_manifest manifest;
    manifest.plugin_id = std::move(id);
    manifest.name = "Generic C# fixture";
    manifest.version = "1.0.0";
    manifest.entry = "prebuilt/HelloPlugin.dll";
    manifest.managed_type = "SaoAuto.Plugins.HelloCsharp.HelloPlugin, HelloPlugin";
    manifest.runtimeconfig = "prebuilt/HelloPlugin.runtimeconfig.json";
    manifest.language = engine_kind::csharp;
    manifest.source_path = path_utf8(fs::path(SAO_HELLO_CSHARP_DIR));
    return manifest;
}

plugin_manifest entity_manifest(std::string id, const char* output_dir, const char* assembly_name) {
    plugin_manifest manifest;
    manifest.plugin_id = std::move(id);
    manifest.name = "Managed Entity v2 fixture";
    manifest.version = "1.0.0";
    manifest.entry = std::string(assembly_name) + ".dll";
    manifest.managed_type =
        std::string("SaoAuto.Plugins.HelloCsharp.HelloPlugin, ") + assembly_name;
    manifest.runtimeconfig = std::string(assembly_name) + ".runtimeconfig.json";
    manifest.language = engine_kind::csharp;
    manifest.source_path = path_utf8(fs::path(output_dir));
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

bool generic_fixture_available(std::string& reason) {
    bool hostfxr = false;
    REQUIRE(sao_plugins_cshost_is_available(&hostfxr) == SAO_OK);
    if (!hostfxr) {
        reason = "hostfxr runtime capability missing";
        return false;
    }
    const fs::path root(SAO_HELLO_CSHARP_DIR);
    if (!fs::is_regular_file(root / L"prebuilt" / L"HelloPlugin.dll")) {
        reason = "precompiled managed fixture missing";
        return false;
    }
    if (!fs::is_regular_file(root / L"prebuilt" / L"HelloPlugin.runtimeconfig.json")) {
        reason = "managed fixture runtimeconfig missing";
        return false;
    }
    return true;
}

bool managed_entity_fixture_available(std::string& reason) {
#if defined(SAO_MANAGED_ENTITY_FIXTURE_PATHS_MISSING)
    reason = ".NET SDK capability missing at CMake configure time";
    return false;
#else
    bool hostfxr = false;
    REQUIRE(sao_plugins_cshost_is_available(&hostfxr) == SAO_OK);
    if (!hostfxr) {
        reason = "hostfxr runtime capability missing";
        return false;
    }
    for (const auto& [directory, assembly] :
         {std::pair{fs::path(SAO_MANAGED_ENTITY_A_DIR), L"HelloPluginEntityA"},
          std::pair{fs::path(SAO_MANAGED_ENTITY_B_DIR), L"HelloPluginEntityB"}}) {
        CAPTURE(path_utf8(directory));
        REQUIRE(fs::is_regular_file(directory / (std::wstring(assembly) + L".dll")));
        REQUIRE(fs::is_regular_file(directory / (std::wstring(assembly) + L".runtimeconfig.json")));
    }
    return true;
#endif
}

template <typename T> const T* stride_element(const void* base, uint32_t stride, uint32_t index) {
    const auto* bytes = static_cast<const std::byte*>(base);
    return reinterpret_cast<const T*>(bytes + static_cast<size_t>(stride) * index);
}

struct managed_row_snapshot {
    std::string category_id;
    std::string category_label;
    std::string category_icon;
    double category_priority = 0.0;
    std::string row_label;
    std::string row_icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = false;
    bool keep_menu_open = false;
    bool close_menu_before = false;

    bool operator==(const managed_row_snapshot& other) const {
        return category_id == other.category_id && category_label == other.category_label &&
               category_icon == other.category_icon &&
               category_priority == other.category_priority && row_label == other.row_label &&
               row_icon == other.row_icon && action_id == other.action_id &&
               payload_json == other.payload_json && can_activate == other.can_activate &&
               keep_menu_open == other.keep_menu_open &&
               close_menu_before == other.close_menu_before;
    }
};

struct managed_provider_snapshot {
    std::string provider_id;
    std::string owner_plugin_id;
    uint64_t generation = 0;
    uint64_t revision = 0;
    entity_snapshot_content_token_t content_token = 0;
    uint32_t row_stride = 0;
    std::vector<managed_row_snapshot> rows;
};

struct managed_root_snapshot {
    std::string owner_plugin_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::vector<std::pair<std::string, std::string>> actions;
};

struct managed_catalog_snapshot {
    uint64_t revision = 0;
    entity_snapshot_content_token_t content_token = 0;
    std::vector<managed_provider_snapshot> providers;
    std::vector<managed_root_snapshot> roots;
};

std::string copied_text(const char* value) {
    return value == nullptr ? std::string{} : std::string(value);
}

int32_t SAO_PLUGINS_CALL copy_managed_catalog(const entity_provider_catalog_view_v2* catalog,
                                              void* user_data) {
    if (catalog == nullptr || user_data == nullptr ||
        catalog->struct_size < kEntityProviderCatalogViewV2RequiredPrefixSize ||
        catalog->abi_version != kEntitySnapshotAbiVersion2 ||
        catalog->provider_stride_bytes < kEntityProviderViewV2RequiredPrefixSize ||
        catalog->root_contribution_stride_bytes < kEntityRootContributionViewV2RequiredPrefixSize) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    managed_catalog_snapshot candidate;
    candidate.revision = catalog->revision;
    candidate.content_token = catalog->content_token;
    candidate.providers.reserve(catalog->provider_count);
    for (uint32_t provider_index = 0; provider_index < catalog->provider_count; ++provider_index) {
        const auto* provider = stride_element<entity_provider_view_v2>(
            catalog->providers, catalog->provider_stride_bytes, provider_index);
        if (provider->struct_size < kEntityProviderViewV2RequiredPrefixSize ||
            provider->snapshot_abi_version != kEntitySnapshotAbiVersion2 ||
            provider->row_stride_bytes < kEntityMenuRowV2RequiredPrefixSize ||
            (provider->row_count != 0 && provider->rows == nullptr)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        managed_provider_snapshot copied;
        copied.provider_id = copied_text(provider->provider_id_utf8);
        copied.owner_plugin_id = copied_text(provider->owner_plugin_id_utf8);
        copied.generation = provider->generation;
        copied.revision = provider->revision;
        copied.content_token = provider->content_token;
        copied.row_stride = provider->row_stride_bytes;
        copied.rows.reserve(provider->row_count);
        for (uint32_t row_index = 0; row_index < provider->row_count; ++row_index) {
            const auto* row = stride_element<entity_menu_row_v2>(
                provider->rows, provider->row_stride_bytes, row_index);
            if (row->struct_size < kEntityMenuRowV2RequiredPrefixSize)
                return SAO_ERR_INVALID_ARGUMENT;
            copied.rows.push_back({
                copied_text(row->category_id_utf8),
                copied_text(row->category_label_utf8),
                copied_text(row->category_icon_utf8),
                row->category_priority,
                copied_text(row->row_label_utf8),
                copied_text(row->row_icon_utf8),
                copied_text(row->action_id_utf8),
                copied_text(row->payload_json_utf8),
                row->can_activate != 0,
                row->keep_menu_open != 0,
                row->close_menu_before != 0,
            });
        }
        candidate.providers.push_back(std::move(copied));
    }
    candidate.roots.reserve(catalog->root_contribution_count);
    for (uint32_t root_index = 0; root_index < catalog->root_contribution_count; ++root_index) {
        const auto* root = stride_element<entity_root_contribution_view_v2>(
            catalog->root_contributions, catalog->root_contribution_stride_bytes, root_index);
        if (root->struct_size < kEntityRootContributionViewV2RequiredPrefixSize ||
            root->action_stride_bytes < kEntityRootActionRefViewV2RequiredPrefixSize ||
            (root->action_count != 0 && root->actions == nullptr)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        managed_root_snapshot copied;
        copied.owner_plugin_id = copied_text(root->owner_plugin_id_utf8);
        copied.contribution_id = copied_text(root->contribution_id_utf8);
        copied.root_id = copied_text(root->root_id_utf8);
        copied.name = copied_text(root->name_utf8);
        copied.icon = copied_text(root->icon_utf8);
        copied.priority = root->priority;
        copied.actions.reserve(root->action_count);
        for (uint32_t action_index = 0; action_index < root->action_count; ++action_index) {
            const auto* action = stride_element<entity_root_action_ref_view_v2>(
                root->actions, root->action_stride_bytes, action_index);
            if (action->struct_size < kEntityRootActionRefViewV2RequiredPrefixSize)
                return SAO_ERR_INVALID_ARGUMENT;
            copied.actions.emplace_back(copied_text(action->provider_id_utf8),
                                        copied_text(action->action_id_utf8));
        }
        candidate.roots.push_back(std::move(copied));
    }
    *static_cast<managed_catalog_snapshot*>(user_data) = std::move(candidate);
    return SAO_OK;
}

int32_t snapshot_managed_catalog(managed_catalog_snapshot& output) {
    return sao_plugins_entity_provider_snapshot_v2(copy_managed_catalog, &output);
}

const managed_provider_snapshot* find_provider(const managed_catalog_snapshot& catalog,
                                               const std::string& provider_id) {
    const auto found = std::find_if(catalog.providers.begin(), catalog.providers.end(),
                                    [&provider_id](const managed_provider_snapshot& provider) {
                                        return provider.provider_id == provider_id;
                                    });
    return found == catalog.providers.end() ? nullptr : &*found;
}

const managed_root_snapshot* find_root(const managed_catalog_snapshot& catalog,
                                       const std::string& contribution_id) {
    const auto found = std::find_if(catalog.roots.begin(), catalog.roots.end(),
                                    [&contribution_id](const managed_root_snapshot& root) {
                                        return root.contribution_id == contribution_id;
                                    });
    return found == catalog.roots.end() ? nullptr : &*found;
}

struct lifecycle_unload_probe {
    plugin_handle_t plugin = nullptr;
    std::atomic<uint32_t> callback_count{0};
    std::atomic<int32_t> status{SAO_OK};
};

void reentrant_unload_callback(const char*, const char*, void* user_data) {
    auto& probe = *static_cast<lifecycle_unload_probe*>(user_data);
    probe.callback_count.fetch_add(1, std::memory_order_relaxed);
    probe.status.store(sao_plugins_lifecycle_unload(probe.plugin), std::memory_order_release);
}

std::string adapter_error(cs_loader_adapter_owner_t owner, plugin_handle_t plugin) {
    char* raw = nullptr;
    REQUIRE(sao_plugins_cshost_loader_adapter_get_last_error(owner, plugin, &raw) == SAO_OK);
    const std::string result = raw == nullptr ? std::string{} : raw;
    sao_plugins_cshost_free_string(raw);
    return result;
}

} // namespace

TEST_CASE("csharp generic manifest fields are public and source entries fail closed",
          "[plugins][csharp][generic][contract]") {
    const std::string text = R"({
        "id":"generic_manifest","entry":"plugin.dll","language":"csharp",
        "managed_type":"Example.Plugin, Example",
        "runtimeconfig":"runtime/plugin.runtimeconfig.json"
    })";
    plugin_manifest manifest;
    REQUIRE(sao_plugins_manifest_parse(text.data(), text.size(), &manifest) == SAO_OK);
    REQUIRE(manifest.managed_type == "Example.Plugin, Example");
    REQUIRE(manifest.runtimeconfig == "runtime/plugin.runtimeconfig.json");
    REQUIRE(validate_manifest(manifest) == SAO_OK);

    manifest.runtimeconfig = "../outside.runtimeconfig.json";
    REQUIRE(validate_manifest(manifest) == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("csharp generic loader runs real component load and separates hooks",
          "[plugins][csharp][generic][lifecycle]") {
    std::string capability_reason;
    if (!generic_fixture_available(capability_reason)) {
        WARN("CAPABILITY_SKIP: " << capability_reason);
        SUCCEED("generic lifecycle capability unavailable");
        return;
    }

    cs_loader_adapter_owner_t owner = nullptr;
    const int32_t register_status = sao_plugins_cshost_register_loader_adapter(nullptr, &owner);
    INFO("register_status=" << register_status);
    REQUIRE(register_status == SAO_OK);
    REQUIRE(owner != nullptr);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);

    auto missing_type = hello_manifest("csharp_generic_missing_type");
    missing_type.managed_type.clear();
    auto missing_type_plugin = add_plugin(missing_type);
    REQUIRE(sao_plugins_lifecycle_load(missing_type_plugin) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(adapter_error(owner, missing_type_plugin).find("managed_type") != std::string::npos);
    REQUIRE(adapter_error(owner, missing_type_plugin).empty());
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(missing_type_plugin);

    auto missing_assembly = hello_manifest("csharp_generic_missing_assembly");
    missing_assembly.entry = "prebuilt/MissingComponent.dll";
    auto missing_assembly_plugin = add_plugin(missing_assembly);
    REQUIRE(sao_plugins_lifecycle_load(missing_assembly_plugin) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(adapter_error(owner, missing_assembly_plugin).find("managed file") !=
            std::string::npos);
    remove_plugin(missing_assembly_plugin);

    auto missing_runtime = hello_manifest("csharp_generic_missing_runtime");
    missing_runtime.runtimeconfig = "prebuilt/MissingComponent.runtimeconfig.json";
    auto missing_runtime_plugin = add_plugin(missing_runtime);
    REQUIRE(sao_plugins_lifecycle_load(missing_runtime_plugin) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(adapter_error(owner, missing_runtime_plugin).find("runtimeconfig unavailable") !=
            std::string::npos);
    remove_plugin(missing_runtime_plugin);

    auto source_entry = hello_manifest("csharp_generic_source_rejected");
    source_entry.entry = "HelloPlugin.cs";
    auto source_entry_plugin = add_plugin(source_entry);
    REQUIRE(sao_plugins_lifecycle_load(source_entry_plugin) == SAO_PLUGINS_ERR_UNSUPPORTED);
    REQUIRE(adapter_error(owner, source_entry_plugin).find("precompiled .dll entries only") !=
            std::string::npos);
    remove_plugin(source_entry_plugin);

    auto manifest = hello_manifest("csharp_generic_real");
    manifest.runtimeconfig.clear();
    auto plugin = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(plugin) == lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 1);

    plugin_context_t* adapter_context = nullptr;
    plugin_context_t* loader_context = nullptr;
    REQUIRE(sao_plugins_cshost_loader_adapter_get_context(owner, plugin, &adapter_context) ==
            SAO_OK);
    REQUIRE(sao_plugins_lifecycle_get_context(plugin, &loader_context) == SAO_OK);
    REQUIRE(adapter_context != nullptr);
    REQUIRE(adapter_context == loader_context);
    REQUIRE(std::string(sao_plugins_ctx_plugin_id(adapter_context)) == "csharp_generic_real");

    REQUIRE(sao_plugins_cshost_unregister_loader_adapter(owner) == SAO_PLUGINS_ERR_BUSY);

    const int32_t enable_status = sao_plugins_lifecycle_enable(plugin);
    INFO("enable_status=" << enable_status << " adapter_error=" << adapter_error(owner, plugin));
    REQUIRE(enable_status == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(plugin) == lifecycle_state::loaded_active);

    REQUIRE(sao_plugins_lifecycle_unload(plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(plugin) == lifecycle_state::unloaded);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(plugin);

    auto reload = hello_manifest("csharp_generic_reload_rejected");
    auto reload_plugin = add_plugin(reload);
    REQUIRE(sao_plugins_lifecycle_load(reload_plugin) == SAO_PLUGINS_ERR_ALREADY_EXISTS);
    REQUIRE(adapter_error(owner, reload_plugin).find("process restart") != std::string::npos);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(reload_plugin);

    REQUIRE(sao_plugins_cshost_unregister_loader_adapter(owner) == SAO_OK);
}

TEST_CASE("csharp managed Entity v2 provider preserves lifecycle token stride and generations",
          "[plugins][csharp][generic][entity][v2][focused][.managed_entity]") {
    std::string capability_reason;
    if (!managed_entity_fixture_available(capability_reason)) {
        WARN("CAPABILITY_SKIP: " << capability_reason);
        SUCCEED("managed Entity fixture capability unavailable");
        return;
    }

    constexpr const char* kPluginId = "csharp_managed_entity_fixture";
    const std::string provider_id = std::string(kPluginId) + "/managed-menu";
    const std::string contribution_id = "managed-root";
    constexpr const char* kVisibleAction = "managed-action";
    constexpr const char* kReentryTopic = "csharp_entity_reentrant_unload";

    cs_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_cshost_register_loader_adapter(nullptr, &owner) == SAO_OK);
    REQUIRE(owner != nullptr);

    auto first_manifest =
        entity_manifest(kPluginId, SAO_MANAGED_ENTITY_A_DIR, "HelloPluginEntityA");
    auto first_plugin = add_plugin(first_manifest);
    REQUIRE(sao_plugins_lifecycle_load(first_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(first_plugin) == lifecycle_state::loaded_disabled);
    plugin_context_t* first_context = nullptr;
    REQUIRE(sao_plugins_cshost_loader_adapter_get_context(owner, first_plugin, &first_context) ==
            SAO_OK);
    REQUIRE(first_context != nullptr);
    REQUIRE(sao_plugins_lifecycle_enable(first_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(first_plugin) == lifecycle_state::loaded_active);

    managed_catalog_snapshot initial_catalog;
    REQUIRE(snapshot_managed_catalog(initial_catalog) == SAO_OK);
    const auto* initial_provider = find_provider(initial_catalog, provider_id);
    const auto* initial_root = find_root(initial_catalog, contribution_id);
    REQUIRE(initial_provider != nullptr);
    REQUIRE(initial_root != nullptr);
    REQUIRE(initial_provider->rows.size() == 1);
    REQUIRE(initial_root->actions.size() == 1);
    CHECK(initial_provider->owner_plugin_id == kPluginId);
    CHECK(initial_provider->generation != 0);
    CHECK(initial_provider->revision == 1);
    CHECK(initial_provider->content_token != kInvalidEntitySnapshotContentToken);
    CHECK(initial_provider->row_stride == sizeof(entity_menu_row_v2));
    CHECK(initial_provider->rows[0].category_id == "managed-tools");
    CHECK(initial_provider->rows[0].category_label == "Managed Tools");
    CHECK(initial_provider->rows[0].row_label == "Managed Fixture");
    CHECK(initial_provider->rows[0].action_id == kVisibleAction);
    CHECK(initial_provider->rows[0].payload_json == R"({"action_count":0})");
    CHECK(initial_provider->rows[0].can_activate);
    CHECK_FALSE(initial_provider->rows[0].keep_menu_open);
    CHECK_FALSE(initial_provider->rows[0].close_menu_before);
    CHECK(initial_root->owner_plugin_id == kPluginId);
    CHECK(initial_root->root_id == "plugin:csharp-managed");
    CHECK(initial_root->name == "Managed Fixture");
    CHECK(initial_root->actions[0] == std::pair{provider_id, std::string(kVisibleAction)});

    const uint64_t first_generation = initial_provider->generation;
    const uint64_t initial_revision = initial_provider->revision;
    const auto initial_content_token = initial_provider->content_token;

    lifecycle_unload_probe unload_probe;
    unload_probe.plugin = first_plugin;
    uint32_t unload_token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(first_context, kReentryTopic, reentrant_unload_callback,
                                      &unload_probe, &unload_token) == SAO_OK);
    REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation,
                                               kVisibleAction,
                                               R"({"source":"managed"})") == SAO_OK);
    CHECK(unload_probe.callback_count.load(std::memory_order_acquire) == 1);
    CHECK(unload_probe.status.load(std::memory_order_acquire) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(first_plugin) == lifecycle_state::loaded_active);

    managed_catalog_snapshot after_reentry;
    REQUIRE(snapshot_managed_catalog(after_reentry) == SAO_OK);
    const auto* after_reentry_provider = find_provider(after_reentry, provider_id);
    REQUIRE(after_reentry_provider != nullptr);
    REQUIRE(after_reentry_provider->rows.size() == 1);
    CHECK(after_reentry_provider->generation == first_generation);
    CHECK(after_reentry_provider->revision == initial_revision + 1);
    CHECK(after_reentry_provider->content_token != initial_content_token);
    CHECK(after_reentry_provider->rows[0].row_label == "Managed Fixture 1");
    CHECK(after_reentry_provider->rows[0].payload_json == R"({"action_count":1})");

    REQUIRE(sao_plugins_ctx_unsubscribe(first_context, unload_token) == SAO_OK);
    REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation,
                                               kVisibleAction, "{}") == SAO_OK);
    CHECK(unload_probe.callback_count.load(std::memory_order_acquire) == 1);

    managed_catalog_snapshot stable_catalog;
    REQUIRE(snapshot_managed_catalog(stable_catalog) == SAO_OK);
    const auto* stable_provider = find_provider(stable_catalog, provider_id);
    REQUIRE(stable_provider != nullptr);
    REQUIRE(stable_provider->rows.size() == 1);
    CHECK(stable_provider->revision == initial_revision + 2);
    CHECK(stable_provider->rows[0].row_label == "Managed Fixture 2");
    CHECK(stable_provider->rows[0].payload_json == R"({"action_count":2})");

    struct invalid_snapshot_case {
        const char* action;
        int32_t expected_status;
    };
    const invalid_snapshot_case invalid_cases[] = {
        {"fixture:set-invalid-utf8", SAO_ERR_INVALID_ARGUMENT},
        {"fixture:set-invalid-json", SAO_ERR_INVALID_ARGUMENT},
        {"fixture:set-invalid-flags", SAO_ERR_INVALID_ARGUMENT},
        {"fixture:set-invalid-reserved", SAO_ERR_INVALID_ARGUMENT},
        {"fixture:set-over-budget", SAO_ERR_INVALID_ARGUMENT},
        {"fixture:set-zero-token", SAO_ERR_INVALID_ARGUMENT},
        {"fixture:set-count-mismatch", SAO_PLUGINS_ERR_BUSY},
        {"fixture:set-revision-mismatch", SAO_PLUGINS_ERR_BUSY},
        {"fixture:set-token-mismatch", SAO_PLUGINS_ERR_BUSY},
        {"fixture:set-stride-mismatch", SAO_PLUGINS_ERR_BUSY},
    };
    for (const auto& invalid : invalid_cases) {
        CAPTURE(invalid.action);
        REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation,
                                                   invalid.action, "{}") == SAO_OK);
        managed_catalog_snapshot rejected;
        CHECK(snapshot_managed_catalog(rejected) == invalid.expected_status);
        CHECK(rejected.providers.empty());
        REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation,
                                                   "fixture:set-stable", "{}") == SAO_OK);
        managed_catalog_snapshot restored;
        REQUIRE(snapshot_managed_catalog(restored) == SAO_OK);
        const auto* restored_provider = find_provider(restored, provider_id);
        REQUIRE(restored_provider != nullptr);
        CHECK(restored_provider->generation == first_generation);
        CHECK(restored_provider->revision == stable_provider->revision);
        CHECK(restored_provider->content_token == stable_provider->content_token);
        CHECK(restored_provider->rows == stable_provider->rows);
    }

    REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation,
                                               "fixture:bump-producer-token", "{}") == SAO_OK);
    managed_catalog_snapshot producer_token_changed;
    REQUIRE(snapshot_managed_catalog(producer_token_changed) == SAO_OK);
    const auto* producer_token_provider = find_provider(producer_token_changed, provider_id);
    REQUIRE(producer_token_provider != nullptr);
    CHECK(producer_token_provider->revision == stable_provider->revision);
    CHECK(producer_token_provider->content_token == stable_provider->content_token);
    CHECK(producer_token_changed.content_token == stable_catalog.content_token);

    REQUIRE(sao_plugins_lifecycle_disable(first_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(first_plugin) == lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 1);
    managed_catalog_snapshot after_explicit_unregister;
    REQUIRE(snapshot_managed_catalog(after_explicit_unregister) == SAO_OK);
    CHECK(find_provider(after_explicit_unregister, provider_id) == nullptr);
    CHECK(find_root(after_explicit_unregister, contribution_id) == nullptr);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation, kVisibleAction,
                                             "{}") == SAO_ERR_HANDLE_INVALID);

    REQUIRE(sao_plugins_lifecycle_unload(first_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(first_plugin) == lifecycle_state::unloaded);
    CHECK(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    managed_catalog_snapshot after_first_unload;
    REQUIRE(snapshot_managed_catalog(after_first_unload) == SAO_OK);
    CHECK(find_provider(after_first_unload, provider_id) == nullptr);
    remove_plugin(first_plugin);

    auto second_manifest =
        entity_manifest(kPluginId, SAO_MANAGED_ENTITY_B_DIR, "HelloPluginEntityB");
    auto second_plugin = add_plugin(second_manifest);
    REQUIRE(sao_plugins_lifecycle_load(second_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(second_plugin) == SAO_OK);
    managed_catalog_snapshot second_catalog;
    REQUIRE(snapshot_managed_catalog(second_catalog) == SAO_OK);
    const auto* second_provider = find_provider(second_catalog, provider_id);
    const auto* second_root = find_root(second_catalog, contribution_id);
    REQUIRE(second_provider != nullptr);
    REQUIRE(second_root != nullptr);
    REQUIRE(second_provider->rows.size() == 1);
    REQUIRE(second_root->actions.size() == 1);
    CHECK(second_provider->generation != first_generation);
    CHECK(second_provider->provider_id == provider_id);
    CHECK(second_provider->rows[0].action_id == kVisibleAction);
    CHECK(second_root->contribution_id == contribution_id);
    CHECK(second_root->actions[0] == std::pair{provider_id, std::string(kVisibleAction)});
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), first_generation, kVisibleAction,
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), second_provider->generation,
                                               kVisibleAction, "{}") == SAO_OK);

    const uint64_t second_generation = second_provider->generation;
    REQUIRE(sao_plugins_lifecycle_unload(second_plugin) == SAO_OK);
    CHECK(sao_plugins_lifecycle_state(second_plugin) == lifecycle_state::unloaded);
    CHECK(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), second_generation, kVisibleAction,
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    remove_plugin(second_plugin);
    REQUIRE(sao_plugins_cshost_unregister_loader_adapter(owner) == SAO_OK);
}
