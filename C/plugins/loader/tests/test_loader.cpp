#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/loader/plugin_isolation.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_scanner.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sao::plugins::loader;
namespace fs = std::filesystem;

#ifndef SAO_TEST_NATIVE_PLUGIN_PATH
#define SAO_TEST_NATIVE_PLUGIN_PATH L""
#endif

namespace {

struct entity_provider_row_snapshot {
    std::string category_id;
    std::string category_label;
    std::string category_icon;
    double category_priority = 0.0;
    std::string row_label;
    std::string row_icon;
    std::string action_id;
    std::string payload_json;
    bool can_activate = true;
    bool keep_menu_open = false;
    bool close_menu_before = false;
};

struct entity_provider_snapshot_record {
    std::string provider_id;
    std::string owner_plugin_id;
    uint64_t generation = 0;
    uint64_t revision = 0;
    std::vector<entity_provider_row_snapshot> rows;
};

struct entity_provider_catalog_snapshot {
    uint64_t revision = 0;
    std::vector<entity_provider_snapshot_record> providers;
};

struct adapter_probe {
    plugin_context_t* load_context = nullptr;
    plugin_context_t* unload_context = nullptr;
    int load_calls = 0;
    int on_load_calls = 0;
    int on_unload_calls = 0;
    int unload_calls = 0;
    int32_t on_load_status = SAO_OK;
    int32_t on_unload_status = SAO_OK;
    bool allow_unload = true;
    bool context_visible_during_load = false;
    bool extension_visible_during_on_unload = false;
    bool extension_removed_before_adapter_unload = false;
};

int32_t SAO_PLUGINS_CALL copy_entity_provider_catalog(
    const entity_provider_catalog_view* catalog, void* user_data) {
    if (catalog == nullptr || user_data == nullptr ||
        catalog->struct_size < sizeof(entity_provider_catalog_view)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& out = *static_cast<entity_provider_catalog_snapshot*>(user_data);
    entity_provider_catalog_snapshot candidate;
    candidate.revision = catalog->revision;
    candidate.providers.reserve(catalog->provider_count);
    for (uint32_t provider_index = 0;
         provider_index < catalog->provider_count; ++provider_index) {
        const auto& provider = catalog->providers[provider_index];
        entity_provider_snapshot_record copied;
        copied.provider_id = provider.provider_id_utf8;
        copied.owner_plugin_id = provider.owner_plugin_id_utf8;
        copied.generation = provider.generation;
        copied.revision = provider.revision;
        copied.rows.reserve(provider.row_count);
        for (uint32_t row_index = 0; row_index < provider.row_count;
             ++row_index) {
            const auto& row = provider.rows[row_index];
            copied.rows.push_back({
                row.category_id_utf8,
                row.category_label_utf8,
                row.category_icon_utf8,
                row.category_priority,
                row.row_label_utf8,
                row.row_icon_utf8,
                row.action_id_utf8,
                row.payload_json_utf8,
                row.can_activate != 0,
                row.keep_menu_open != 0,
                row.close_menu_before != 0,
            });
        }
        candidate.providers.push_back(std::move(copied));
    }
    out = std::move(candidate);
    return SAO_OK;
}

int32_t snapshot_entity_providers(entity_provider_catalog_snapshot& out) {
    return sao_plugins_entity_provider_snapshot(copy_entity_provider_catalog,
                                                &out);
}

int32_t SAO_PLUGINS_CALL probe_adapter_load(
    plugin_handle_t plugin, const plugin_manifest*, void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.load_calls;
    plugin_context_t* context = nullptr;
    probe.context_visible_during_load =
        sao_plugins_lifecycle_get_context(plugin, &context) == SAO_OK &&
        context != nullptr;
    probe.load_context = context;
    return context == nullptr
               ? SAO_ERR_NOT_INITIALIZED
               : sao_plugins_ctx_register_ui_panel(
                     context, "rollback_panel", R"({"title":"Rollback"})",
                     nullptr, nullptr, nullptr);
}

int32_t SAO_PLUGINS_CALL probe_adapter_on_load(plugin_handle_t plugin,
                                               void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.on_load_calls;
    plugin_context_t* context = nullptr;
    if (sao_plugins_lifecycle_get_context(plugin, &context) != SAO_OK ||
        context != probe.load_context) {
        return SAO_ERR_NOT_INITIALIZED;
    }
    return probe.on_load_status;
}

int32_t SAO_PLUGINS_CALL probe_adapter_enable(plugin_handle_t, void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL probe_adapter_disable(plugin_handle_t, void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL probe_adapter_on_unload(plugin_handle_t plugin,
                                                 bool* allow,
                                                 void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.on_unload_calls;
    plugin_context_t* context = nullptr;
    const auto extensions = snapshot_extensions(
        sao_plugins_registry_instance(), extension_kind::ui_panel);
    probe.extension_visible_during_on_unload =
        std::any_of(extensions.begin(), extensions.end(), [](const auto& item) {
            return item.id == "rollback_panel";
        });
    if (sao_plugins_lifecycle_get_context(plugin, &context) != SAO_OK ||
        context != probe.load_context) {
        return SAO_ERR_NOT_INITIALIZED;
    }
    probe.unload_context = context;
    *allow = probe.allow_unload;
    return probe.on_unload_status;
}

int32_t SAO_PLUGINS_CALL probe_adapter_unload(plugin_handle_t plugin,
                                              void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.unload_calls;
    plugin_context_t* context = nullptr;
    const auto extensions = snapshot_extensions(
        sao_plugins_registry_instance(), extension_kind::ui_panel);
    probe.extension_removed_before_adapter_unload =
        std::none_of(extensions.begin(), extensions.end(), [](const auto& item) {
            return item.id == "rollback_panel";
        });
    return sao_plugins_lifecycle_get_context(plugin, &context) == SAO_OK &&
                   context == probe.load_context
               ? SAO_OK
               : SAO_ERR_NOT_INITIALIZED;
}

host_adapter_vtable probe_adapter_vtable(adapter_probe* probe) {
    host_adapter_vtable adapter{};
    adapter.load_plugin = probe_adapter_load;
    adapter.call_on_load = probe_adapter_on_load;
    adapter.call_on_enable = probe_adapter_enable;
    adapter.call_on_disable = probe_adapter_disable;
    adapter.call_on_unload = probe_adapter_on_unload;
    adapter.unload_plugin = probe_adapter_unload;
    adapter.host_user_data = probe;
    return adapter;
}

struct TempDirectory {
    fs::path path;

    explicit TempDirectory(const wchar_t* label) {
        wchar_t base[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, base) > 0);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::path(base) / (std::wstring(L"sao_loader_") + label + L"_" + std::to_wstring(stamp));
        REQUIRE(fs::create_directories(path));
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string path_utf8(const fs::path& path) {
    const auto wide = path.native();
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr);
    return result;
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(std::string id, const fs::path& source,
                              std::vector<std::string> dependencies = {}) {
    plugin_manifest manifest;
    manifest.plugin_id = std::move(id);
    manifest.name = manifest.plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.emma";
    manifest.language = engine_kind::emma;
    manifest.source_path = path_utf8(source);
    manifest.abi_version = 2;
    manifest.requires_list = std::move(dependencies);
    return manifest;
}

plugin_handle_t add_plugin(plugin_manifest manifest) {
    plugin_handle_t handle = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(
        sao_plugins_registry_instance(), &manifest, &handle) == SAO_OK);
    REQUIRE(handle != nullptr);
    return handle;
}

void remove_plugin(plugin_handle_t handle) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) == SAO_OK);
}

void put16(std::ofstream& output, uint16_t value) {
    const unsigned char bytes[] = {
        static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8)};
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void put32(std::ofstream& output, uint32_t value) {
    const unsigned char bytes[] = {
        static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

struct ZipItem {
    std::string name;
    std::string data;
    uint16_t method = 0;
    uint32_t local_offset = 0;
};

void write_zip(const fs::path& path, std::vector<ZipItem> items) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    for (auto& item : items) {
        item.local_offset = static_cast<uint32_t>(output.tellp());
        put32(output, 0x04034b50U); put16(output, 20); put16(output, 0);
        put16(output, item.method); put16(output, 0); put16(output, 0); put32(output, 0);
        put32(output, static_cast<uint32_t>(item.data.size()));
        put32(output, static_cast<uint32_t>(item.data.size()));
        put16(output, static_cast<uint16_t>(item.name.size())); put16(output, 0);
        output.write(item.name.data(), static_cast<std::streamsize>(item.name.size()));
        output.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
    }
    const auto central_offset = static_cast<uint32_t>(output.tellp());
    for (const auto& item : items) {
        put32(output, 0x02014b50U); put16(output, 20); put16(output, 20); put16(output, 0);
        put16(output, item.method); put16(output, 0); put16(output, 0); put32(output, 0);
        put32(output, static_cast<uint32_t>(item.data.size()));
        put32(output, static_cast<uint32_t>(item.data.size()));
        put16(output, static_cast<uint16_t>(item.name.size()));
        put16(output, 0); put16(output, 0); put16(output, 0); put16(output, 0); put32(output, 0);
        put32(output, item.local_offset);
        output.write(item.name.data(), static_cast<std::streamsize>(item.name.size()));
    }
    const auto central_end = static_cast<uint32_t>(output.tellp());
    put32(output, 0x06054b50U); put16(output, 0); put16(output, 0);
    put16(output, static_cast<uint16_t>(items.size()));
    put16(output, static_cast<uint16_t>(items.size()));
    put32(output, central_end - central_offset); put32(output, central_offset); put16(output, 0);
    REQUIRE(output.good());
}

std::atomic_int g_adapter_loads{0};
std::atomic_int g_adapter_enables{0};
std::atomic_int g_adapter_disables{0};
std::atomic_int g_adapter_unloads{0};
std::atomic_bool g_adapter_fail_on_load{false};

int32_t SAO_PLUGINS_CALL adapter_load(plugin_handle_t, const plugin_manifest*, void*) {
    ++g_adapter_loads;
    return SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_simple_load(plugin_handle_t, void*) {
    return g_adapter_fail_on_load ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_enable(plugin_handle_t, void*) { ++g_adapter_enables; return SAO_OK; }
int32_t SAO_PLUGINS_CALL adapter_disable(plugin_handle_t, void*) { ++g_adapter_disables; return SAO_OK; }
int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t, bool* allow, void*) {
    *allow = true;
    return SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t, void*) { ++g_adapter_unloads; return SAO_OK; }

int32_t render_panel(const char*, char**, void*) { return SAO_OK; }

void install_emma_adapter() {
    host_adapter_vtable adapter{};
    adapter.load_plugin = adapter_load;
    adapter.call_on_load = adapter_simple_load;
    adapter.call_on_enable = adapter_enable;
    adapter.call_on_disable = adapter_disable;
    adapter.call_on_unload = adapter_on_unload;
    adapter.unload_plugin = adapter_unload;
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::emma, &adapter) == SAO_OK);
}

} // namespace

TEST_CASE("manifest parses and validates normalized fields", "[plugins][loader][manifest]") {
    const std::string text = R"({
        "id":"loader_manifest_case","name":"Loader","version":"2.1.0",
        "entry":"main.lua","language":"lua","enabled":true,"abi_version":2,
        "requires":["base_plugin>=1.0"],"permissions":["fs"],
        "capabilities":[{"id":"ui","title":"UI","actions":["open"]}],
        "hotkeys":{"toggle":"F8"},"settings_schema":{"rate":{"type":"int","default":4}}
    })";
    plugin_manifest manifest;
    REQUIRE(sao_plugins_manifest_parse(text.data(), text.size(), &manifest) == SAO_OK);
    REQUIRE(validate_manifest(manifest) == SAO_OK);
    REQUIRE(manifest.language == engine_kind::lua);
    REQUIRE(manifest.requires_list == std::vector<std::string>{"base_plugin>=1.0"});
    REQUIRE(manifest.capabilities.at(0).actions == std::vector<std::string>{"open"});
    REQUIRE(manifest.hotkeys.at(0).default_key == "F8");
    REQUIRE(manifest.settings_schema.at(0).default_json == "4");

    REQUIRE(sao_plugins_manifest_parse("{", 1, &manifest) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE_FALSE(manifest.parse_error.empty());
    manifest = make_manifest("../escape", fs::path{});
    REQUIRE(validate_manifest(manifest) == SAO_ERR_INVALID_ARGUMENT);
    manifest = make_manifest("safe_id", fs::path{});
    manifest.entry = "../entry.dll";
    REQUIRE(validate_manifest(manifest) == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("native-only manifest uses its native entry without a script entry",
          "[plugins][loader][manifest][native]") {
    const std::string text = R"({
        "id":"native_only_manifest","enabled":true,
        "native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2",
        "abi_version":2
    })";
    plugin_manifest manifest;
    REQUIRE(sao_plugins_manifest_parse(text.data(), text.size(), &manifest) == SAO_OK);
    REQUIRE(validate_manifest(manifest) == SAO_OK);
    REQUIRE(manifest.entry == "native_fixture.dll");
    REQUIRE(manifest.native_entry == "native_fixture.dll");
    REQUIRE(manifest.language == engine_kind::csharp);

    TempDirectory temp(L"native_only_manifest");
    write_text(temp.path / L"plugin.json", text);
    write_text(temp.path / L"native_fixture.dll", "fixture");
    scanned_plugin scanned;
    REQUIRE(sao_plugins_scanner_refresh_one(temp.path.c_str(), &scanned) == SAO_OK);
    REQUIRE(scanned.manifest.entry == "native_fixture.dll");
    REQUIRE(scanned.manifest.native_entry == "native_fixture.dll");
}

TEST_CASE("scanner discovers valid directories and computes a changing signature", "[plugins][loader][scanner]") {
    TempDirectory temp(L"scanner");
    const auto plugin_dir = temp.path / L"plugins" / L"one";
    write_text(plugin_dir / L"plugin.json", R"({"id":"scanner_case","entry":"plugin.emma","language":"emma"})");
    write_text(plugin_dir / L"plugin.emma", "entry");
    scan_config config;
    config.builtin_roots.push_back((temp.path / L"plugins").native());
    config.enable_workspace_walkup = false;
    scanned_plugin* plugins = nullptr;
    size_t count = 0;
    REQUIRE(sao_plugins_scanner_discover(&config, &plugins, &count) == SAO_OK);
    REQUIRE(count == 1);
    REQUIRE(plugins[0].manifest.plugin_id == "scanner_case");
    REQUIRE_FALSE(plugins[0].is_user_installed);
    const auto signature_before = sao_plugins_scanner_signature(&config);
    sao_plugins_scanner_free(plugins, count);
    write_text(plugin_dir / L"plugin.json", R"({"id":"scanner_case","entry":"plugin.emma","language":"emma","version":"2"})");
    REQUIRE(sao_plugins_scanner_signature(&config) != signature_before);

    fs::create_directory(temp.path / L".git");
    wchar_t* workspace = nullptr;
    REQUIRE(sao_plugins_scanner_find_workspace_root(plugin_dir.c_str(), &workspace) == SAO_OK);
    REQUIRE(fs::equivalent(workspace, temp.path));
    sao_plugins_scanner_free_wstring(workspace);
}

TEST_CASE("registry owns records and context implements settings and local events", "[plugins][loader][context]") {
    TempDirectory temp(L"context");
    write_text(temp.path / L"plugin.emma", "entry");
    auto handle = add_plugin(make_manifest("context_case", temp.path));
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);
    REQUIRE(std::string(sao_plugins_ctx_plugin_id(context)) == "context_case");
    REQUIRE(fs::equivalent(sao_plugins_ctx_path(context), temp.path));

    REQUIRE(sao_plugins_ctx_set_defaults(context, R"({"rate":3,"name":"base"})") == SAO_OK);
    REQUIRE(sao_plugins_ctx_set_setting(context, "rate", "7") == SAO_OK);
    char* setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "rate", &setting) == SAO_OK);
    REQUIRE(std::string(setting) == "7");
    sao_plugins_ctx_free_string(setting);

    int callbacks = 0;
    uint32_t token = 0;
    REQUIRE(sao_plugins_ctx_subscribe_once(context, "tick",
        +[](const char*, const char*, void* user) { ++*static_cast<int*>(user); },
        &callbacks, &token) == SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "tick", R"({"value":1})") == SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "tick", R"({"value":2})") == SAO_OK);
    REQUIRE(callbacks == 1);
    char* snapshot = nullptr;
    REQUIRE(sao_plugins_ctx_snapshot_value(context, "tick", &snapshot) == SAO_OK);
    REQUIRE(std::string(snapshot).find("2") != std::string::npos);
    sao_plugins_ctx_free_string(snapshot);

    REQUIRE(sao_plugins_ctx_register_ui_panel(context, "panel", R"({"title":"Panel"})", nullptr, nullptr, nullptr) == SAO_OK);
    REQUIRE(snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel).back().plugin_id == "context_case");
    REQUIRE(sao_plugins_ctx_register_ui_panel(context, "live_panel", R"({"title":"Live"})", render_panel, nullptr, nullptr) == SAO_PLUGINS_ERR_UNSUPPORTED);
    REQUIRE(sao_plugins_ctx_set_timeout(context, nullptr, 1.0, nullptr, nullptr) == SAO_PLUGINS_ERR_UNSUPPORTED);
    sao_plugins_ctx_destroy(context);
    remove_plugin(handle);
}

TEST_CASE("dependency bootstrap is local-only and fails closed for missing installs", "[plugins][loader][deps]") {
    TempDirectory temp(L"deps");
    fs::create_directories(temp.path / L"libs" / L"PIL");
    write_text(temp.path / L"requirements.txt", "Pillow>=10\nmissing-dist==1\n");
    deps_bootstrap_record record;
    REQUIRE(sao_plugins_deps_ensure(temp.path.c_str(), false, &record) == SAO_PLUGINS_ERR_DEPENDENCY_MISSING);
    REQUIRE(record.deps_summary.at("pillow") == "libs");
    REQUIRE(record.deps_summary.at("missing-dist") == "missing");
    REQUIRE(sao_plugins_deps_ensure(temp.path.c_str(), true, &record) == SAO_PLUGINS_ERR_UNSUPPORTED);
}

TEST_CASE("topological ordering rejects missing dependencies and cycles", "[plugins][loader][deps][lifecycle]") {
    auto c = add_plugin(make_manifest("topo_c", fs::path{}));
    auto b = add_plugin(make_manifest("topo_b", fs::path{}, {"topo_c"}));
    auto a = add_plugin(make_manifest("topo_a", fs::path{}, {"topo_b>=1"}));
    plugin_handle_t input[] = {a, c, b};
    plugin_handle_t output[3]{};
    REQUIRE(sao_plugins_lifecycle_topo_sort(input, 3, output) == SAO_OK);
    REQUIRE(output[0] == c);
    REQUIRE(output[1] == b);
    REQUIRE(output[2] == a);
    remove_plugin(a); remove_plugin(b); remove_plugin(c);

    auto missing = add_plugin(make_manifest("topo_missing", fs::path{}, {"absent"}));
    plugin_handle_t missing_output{};
    REQUIRE(sao_plugins_lifecycle_topo_sort(&missing, 1, &missing_output) == SAO_PLUGINS_ERR_DEPENDENCY_MISSING);
    remove_plugin(missing);

    auto cycle_a = add_plugin(make_manifest("cycle_a", fs::path{}, {"cycle_b"}));
    auto cycle_b = add_plugin(make_manifest("cycle_b", fs::path{}, {"cycle_a"}));
    plugin_handle_t cycle_input[] = {cycle_a, cycle_b};
    plugin_handle_t cycle_output[2]{};
    REQUIRE(sao_plugins_lifecycle_topo_sort(cycle_input, 2, cycle_output) == SAO_PLUGINS_ERR_DEPENDENCY_CYCLE);
    remove_plugin(cycle_a); remove_plugin(cycle_b);
}

TEST_CASE("host adapter lifecycle executes symmetric transitions", "[plugins][loader][lifecycle]") {
    TempDirectory temp(L"adapter");
    write_text(temp.path / L"plugin.emma", "entry");
    install_emma_adapter();
    g_adapter_loads = 0; g_adapter_enables = 0; g_adapter_disables = 0; g_adapter_unloads = 0;
    g_adapter_fail_on_load = false;
    auto handle = add_plugin(make_manifest("adapter_case", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_active);
    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(g_adapter_loads == 1);
    REQUIRE(g_adapter_enables == 1);
    REQUIRE(g_adapter_disables >= 1);
    REQUIRE(g_adapter_unloads == 1);
    remove_plugin(handle);

    g_adapter_fail_on_load = true;
    handle = add_plugin(make_manifest("adapter_rollback_case", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
    REQUIRE(g_adapter_unloads == 2);
    remove_plugin(handle);
    g_adapter_fail_on_load = false;
}

TEST_CASE("script adapter sees canonical context during load and unregisters cleanly",
          "[plugins][loader][lifecycle][context]") {
    TempDirectory temp(L"adapter_context");
    write_text(temp.path / L"plugin.lua", "entry");
    auto manifest = make_manifest("adapter_context_case", temp.path);
    manifest.entry = "plugin.lua";
    manifest.language = engine_kind::lua;
    auto handle = add_plugin(manifest);

    plugin_context_t* context = reinterpret_cast<plugin_context_t*>(1);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);

    adapter_probe probe;
    const auto adapter = probe_adapter_vtable(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(
            engine_kind::lua, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(probe.context_visible_during_load);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context == probe.load_context);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::lua) ==
            SAO_PLUGINS_ERR_BUSY);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(probe.unload_context == probe.load_context);
    REQUIRE(probe.extension_visible_during_on_unload);
    REQUIRE(probe.extension_removed_before_adapter_unload);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::lua) ==
            SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("script load failure rolls back hooks resources and context in order",
          "[plugins][loader][lifecycle][rollback]") {
    TempDirectory temp(L"adapter_rollback");
    write_text(temp.path / L"plugin.as", "entry");
    auto manifest = make_manifest("adapter_rollback_focused", temp.path);
    manifest.entry = "plugin.as";
    manifest.language = engine_kind::angelscript;
    auto handle = add_plugin(manifest);

    adapter_probe probe;
    probe.on_load_status = SAO_ERR_OS_CALL_FAILED;
    const auto adapter = probe_adapter_vtable(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(
            engine_kind::angelscript, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
    REQUIRE(probe.load_calls == 1);
    REQUIRE(probe.on_load_calls == 1);
    REQUIRE(probe.on_unload_calls == 1);
    REQUIRE(probe.unload_calls == 1);
    REQUIRE(probe.load_context == probe.unload_context);
    REQUIRE(probe.extension_visible_during_on_unload);
    REQUIRE(probe.extension_removed_before_adapter_unload);

    plugin_context_t* context = reinterpret_cast<plugin_context_t*>(1);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    REQUIRE(snapshot_extensions(sao_plugins_registry_instance(),
                                extension_kind::ui_panel)
                .empty());
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(
            engine_kind::angelscript) == SAO_OK);
    remove_plugin(handle);
}

    TEST_CASE("script load rollback veto remains resident until unload retry",
          "[plugins][loader][lifecycle][rollback][resident-failed]") {
        TempDirectory temp(L"adapter_rollback_veto");
        write_text(temp.path / L"plugin.lua", "entry");
        auto manifest = make_manifest("adapter_rollback_veto", temp.path);
        manifest.entry = "plugin.lua";
        manifest.language = engine_kind::lua;
        auto handle = add_plugin(manifest);

        adapter_probe probe;
        probe.on_load_status = SAO_ERR_OS_CALL_FAILED;
        probe.allow_unload = false;
        const auto adapter = probe_adapter_vtable(&probe);
        REQUIRE(sao_plugins_lifecycle_register_host_adapter(
            engine_kind::lua, &adapter) == SAO_OK);

        REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_PLUGINS_ERR_BUSY);
        REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
        REQUIRE(probe.on_unload_calls == 1);
        REQUIRE(probe.unload_calls == 0);

        plugin_context_t* context = nullptr;
        REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
        REQUIRE(context == probe.load_context);
        REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(),
                        handle) == SAO_PLUGINS_ERR_BUSY);
        REQUIRE(sao_plugins_registry_find(sao_plugins_registry_instance(),
                          manifest.plugin_id.c_str()) == handle);
        REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(
            engine_kind::lua) == SAO_PLUGINS_ERR_BUSY);

        probe.allow_unload = true;
        REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
        REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::unloaded);
        REQUIRE(probe.on_unload_calls == 2);
        REQUIRE(probe.unload_calls == 1);
        REQUIRE(probe.extension_visible_during_on_unload);
        REQUIRE(probe.extension_removed_before_adapter_unload);
        REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) ==
            SAO_ERR_NOT_INITIALIZED);
        REQUIRE(context == nullptr);
        REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(
            engine_kind::lua) == SAO_OK);
        remove_plugin(handle);
    }

    TEST_CASE("native load rollback error remains resident until unload retry",
          "[plugins][loader][lifecycle][rollback][resident-failed][native]") {
        TempDirectory temp(L"native_rollback_error");
        const fs::path fixture = SAO_TEST_NATIVE_PLUGIN_PATH;
        REQUIRE(fs::is_regular_file(fixture));
        const auto copied = temp.path / L"native_fixture.dll";
        REQUIRE(CopyFileW(fixture.c_str(), copied.c_str(), FALSE) == TRUE);
        write_text(temp.path / L"plugin.emma", "entry");

        const auto control_module = LoadLibraryExW(
        copied.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        REQUIRE(control_module != nullptr);
        using set_lifecycle_statuses_fn = int32_t(SAO_PLUGINS_CALL*)(int32_t,
                                     int32_t);
        const auto set_lifecycle_statuses =
        reinterpret_cast<set_lifecycle_statuses_fn>(GetProcAddress(
            control_module, "sao_test_plugin_set_lifecycle_statuses"));
        REQUIRE(set_lifecycle_statuses != nullptr);
        REQUIRE(set_lifecycle_statuses(SAO_ERR_OS_CALL_FAILED,
                       SAO_ERR_OS_CALL_FAILED) == SAO_OK);

        auto manifest = make_manifest("native_rollback_error", temp.path);
        manifest.native_entry = "native_fixture.dll";
        manifest.native_abi = "sao_plugin_v2";
        manifest.capabilities.push_back({"native_test"});
        auto handle = add_plugin(manifest);

        REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_ERR_OS_CALL_FAILED);
        REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
        plugin_context_t* context = nullptr;
        REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
        REQUIRE(context != nullptr);
        REQUIRE(GetModuleHandleW(copied.c_str()) != nullptr);
        REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(),
                        handle) == SAO_PLUGINS_ERR_BUSY);
        REQUIRE(sao_plugins_registry_find(sao_plugins_registry_instance(),
                          manifest.plugin_id.c_str()) == handle);

        REQUIRE(set_lifecycle_statuses(SAO_ERR_OS_CALL_FAILED, SAO_OK) == SAO_OK);
        REQUIRE(FreeLibrary(control_module) == TRUE);
        REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
        REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::unloaded);
        REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
        REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) ==
            SAO_ERR_NOT_INITIALIZED);
        REQUIRE(context == nullptr);
        remove_plugin(handle);
    }

TEST_CASE("host adapter register and unregister reject duplicate operations",
          "[plugins][loader][lifecycle][adapter-registry]") {
    adapter_probe first_probe;
    adapter_probe second_probe;
    const auto first = probe_adapter_vtable(&first_probe);
    const auto second = probe_adapter_vtable(&second_probe);

    REQUIRE(sao_plugins_lifecycle_register_host_adapter(
            engine_kind::csharp, &first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(
            engine_kind::csharp, &second) ==
            SAO_PLUGINS_ERR_ALREADY_EXISTS);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(
            engine_kind::csharp) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(
            engine_kind::csharp) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(
            engine_kind::csharp, &second) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(
            engine_kind::csharp) == SAO_OK);
}

TEST_CASE("native DLL lifecycle validates ABI and capabilities then frees the module", "[plugins][loader][native]") {
    TempDirectory temp(L"native");
    const fs::path fixture = SAO_TEST_NATIVE_PLUGIN_PATH;
    REQUIRE(fs::is_regular_file(fixture));
    const auto copied = temp.path / L"native_fixture.dll";
    REQUIRE(CopyFileW(fixture.c_str(), copied.c_str(), FALSE) == TRUE);
    write_text(temp.path / L"plugin.emma", "entry");

    auto manifest = make_manifest("native_case", temp.path);
    manifest.native_entry = "native_fixture.dll";
    manifest.native_abi = "sao_plugin_v2";
    manifest.enabled = true;
    manifest.capabilities.push_back({"native_test"});
    auto handle = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_active);
    REQUIRE(GetModuleHandleW(copied.c_str()) != nullptr);

    entity_provider_catalog_snapshot providers;
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    const auto provider = std::find_if(
        providers.providers.begin(), providers.providers.end(),
        [](const auto& item) {
            return item.provider_id == "native_case/fixture";
        });
    REQUIRE(provider != providers.providers.end());
    REQUIRE(provider->generation > 0);
    const uint64_t provider_generation = provider->generation;
    REQUIRE(provider->revision == 7);
    REQUIRE(provider->rows.size() == 1);
    CHECK(provider->rows[0].category_id == "fixture-category");
    CHECK(provider->rows[0].action_id == "fixture.action");
    CHECK(provider->rows[0].payload_json == R"({"source":"fixture"})");
    REQUIRE(sao_plugins_entity_provider_invoke(
                "native_case/fixture", provider_generation,
                "fixture.action", R"({"source":"fixture"})") == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    providers = {};
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    CHECK(std::none_of(providers.providers.begin(), providers.providers.end(),
                       [](const auto& item) {
                           return item.provider_id == "native_case/fixture";
                       }));
    CHECK(sao_plugins_entity_provider_invoke(
              "native_case/fixture", provider_generation,
              "fixture.action", "{}") == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    providers = {};
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    const auto reenabled = std::find_if(
        providers.providers.begin(), providers.providers.end(),
        [](const auto& item) {
            return item.provider_id == "native_case/fixture";
        });
    REQUIRE(reenabled != providers.providers.end());
    CHECK(reenabled->generation == provider_generation);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
    CHECK(sao_plugins_entity_provider_invoke(
              "native_case/fixture", provider_generation,
              "fixture.action", "{}") == SAO_ERR_HANDLE_INVALID);
    providers = {};
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    CHECK(std::none_of(providers.providers.begin(), providers.providers.end(),
                       [](const auto& item) {
                           return item.provider_id == "native_case/fixture";
                       }));
    remove_plugin(handle);

    manifest.plugin_id = "native_version_bad";
    manifest.version = "2.0.0";
    handle = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_PLUGINS_ERR_VERSION_MISMATCH);
    REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
    remove_plugin(handle);

    manifest.plugin_id = "native_abi_bad";
    manifest.version = "1.0.0";
    manifest.abi_version = 1;
    manifest.native_abi = "sao_plugin_v1";
    manifest.enabled = false;
    handle = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_PLUGINS_ERR_ABI_MISMATCH);
    REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
    remove_plugin(handle);

    manifest.plugin_id = "native_cap_bad";
    manifest.abi_version = 2;
    manifest.native_abi = "sao_plugin_v2";
    manifest.capabilities = {{"missing_capability"}};
    handle = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_PLUGINS_ERR_CAPABILITY_MISMATCH);
    REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
    remove_plugin(handle);
}

TEST_CASE("archive install and uninstall enforce user ownership", "[plugins][loader][install]") {
    TempDirectory temp(L"install");
    const auto user_plugins = temp.path / L"user_plugins";
    fs::create_directory(user_plugins);
    const auto archive = temp.path / L"plugin.zip";
    write_zip(archive, {
        {"plugin.json", R"({"id":"install_case","name":"Installed","version":"1.2.3","entry":"plugin.emma","language":"emma"})"},
        {"plugin.emma", "entry"},
    });
    install_result result;
    REQUIRE(sao_plugins_install_archive(archive.c_str(), user_plugins.c_str(), false, &result) == SAO_OK);
    REQUIRE(result.ok);
    REQUIRE(result.plugin_id == "install_case");
    REQUIRE(fs::is_regular_file(result.installed_path / fs::path(L"plugin.json")));

    scan_config config;
    config.user_roots.push_back(user_plugins.native());
    config.enable_workspace_walkup = false;
    scanned_plugin* scanned = nullptr;
    size_t scanned_count = 0;
    REQUIRE(sao_plugins_scanner_discover(&config, &scanned, &scanned_count) == SAO_OK);
    REQUIRE(scanned_count == 1);
    REQUIRE(scanned[0].manifest.user_installed);
    auto handle = add_plugin(scanned[0].manifest);
    REQUIRE(handle != nullptr);
    sao_plugins_scanner_free(scanned, scanned_count);
    REQUIRE(sao_plugins_uninstall_plugin("install_case", false) == SAO_OK);
    REQUIRE_FALSE(fs::exists(result.installed_path));
    REQUIRE(sao_plugins_registry_find(sao_plugins_registry_instance(), "install_case") == nullptr);

    auto builtin_manifest = make_manifest("builtin_case", temp.path);
    builtin_manifest.user_installed = false;
    auto builtin = add_plugin(builtin_manifest);
    REQUIRE(sao_plugins_uninstall_plugin("builtin_case", false) == SAO_PLUGINS_ERR_NOT_OWNER);
    remove_plugin(builtin);

    const auto compressed = temp.path / L"compressed.zip";
    write_zip(compressed, {{"plugin.json", "{}", 8}});
    REQUIRE(sao_plugins_install_archive(compressed.c_str(), user_plugins.c_str(), false, &result) == SAO_PLUGINS_ERR_UNSUPPORTED);

    const auto traversal = temp.path / L"traversal.zip";
    write_zip(traversal, {{"../escaped.txt", "escape"}, {"plugin.json", "{}"}});
    REQUIRE(sao_plugins_install_archive(traversal.c_str(), user_plugins.c_str(), false, &result) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE_FALSE(fs::exists(temp.path / L"escaped.txt"));
}

TEST_CASE("isolation reports unsupported while retaining failure diagnostics", "[plugins][loader][isolation]") {
    auto handle = add_plugin(make_manifest("isolation_case", fs::path{}));
    isolation_config config;
    config.mode = isolation_mode::subprocess_sandbox;
    REQUIRE(sao_plugins_isolation_arm(handle, &config) == SAO_PLUGINS_ERR_UNSUPPORTED);
    sao_plugins_isolation_record_failure(handle, "fixture failure");
    uint32_t failures = 0;
    const char* message = nullptr;
    REQUIRE(sao_plugins_isolation_failure_stats(handle, &failures, &message) == SAO_OK);
    REQUIRE(failures == 1);
    REQUIRE(std::string(message) == "fixture failure");
    remove_plugin(handle);
}
