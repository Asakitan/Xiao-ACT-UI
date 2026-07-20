#include <catch2/catch_test_macros.hpp>

#include "entity_provider_internal.h"
#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_install.h"
#include "sao/plugins/loader/plugin_isolation.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_scanner.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sao::plugins::loader;
namespace fs = std::filesystem;

namespace sao::plugins::loader {
int32_t plugin_context_register_entity_providers(plugin_context_t* ctx,
                                                 const native_entity_provider_descriptor* providers,
                                                 size_t count) noexcept;
}

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

struct entity_root_snapshot_record {
    std::string owner_plugin_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    std::vector<std::pair<std::string, std::string>> actions;
};

struct entity_provider_catalog_snapshot {
    uint64_t revision = 0;
    std::vector<entity_provider_snapshot_record> providers;
    std::vector<entity_root_snapshot_record> roots;
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

int32_t SAO_PLUGINS_CALL copy_entity_provider_catalog(const entity_provider_catalog_view* catalog,
                                                      void* user_data) {
    if (catalog == nullptr || user_data == nullptr ||
        catalog->struct_size < sizeof(entity_provider_catalog_view)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& out = *static_cast<entity_provider_catalog_snapshot*>(user_data);
    entity_provider_catalog_snapshot candidate;
    candidate.revision = catalog->revision;
    candidate.providers.reserve(catalog->provider_count);
    for (uint32_t provider_index = 0; provider_index < catalog->provider_count; ++provider_index) {
        const auto& provider = catalog->providers[provider_index];
        entity_provider_snapshot_record copied;
        copied.provider_id = provider.provider_id_utf8;
        copied.owner_plugin_id = provider.owner_plugin_id_utf8;
        copied.generation = provider.generation;
        copied.revision = provider.revision;
        copied.rows.reserve(provider.row_count);
        for (uint32_t row_index = 0; row_index < provider.row_count; ++row_index) {
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
    candidate.roots.reserve(catalog->root_contribution_count);
    for (uint32_t root_index = 0; root_index < catalog->root_contribution_count; ++root_index) {
        const auto& root = catalog->root_contributions[root_index];
        entity_root_snapshot_record copied;
        copied.owner_plugin_id = root.owner_plugin_id_utf8;
        copied.contribution_id = root.contribution_id_utf8;
        copied.root_id = root.root_id_utf8;
        copied.name = root.name_utf8;
        copied.icon = root.icon_utf8;
        copied.priority = root.priority;
        copied.actions.reserve(root.action_count);
        for (uint32_t action_index = 0; action_index < root.action_count; ++action_index) {
            copied.actions.emplace_back(root.actions[action_index].provider_id_utf8,
                                        root.actions[action_index].action_id_utf8);
        }
        candidate.roots.push_back(std::move(copied));
    }
    out = std::move(candidate);
    return SAO_OK;
}

int32_t snapshot_entity_providers(entity_provider_catalog_snapshot& out) {
    return sao_plugins_entity_provider_snapshot(copy_entity_provider_catalog, &out);
}

int32_t SAO_PLUGINS_CALL probe_adapter_load(plugin_handle_t plugin, const plugin_manifest*,
                                            void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.load_calls;
    plugin_context_t* context = nullptr;
    probe.context_visible_during_load =
        sao_plugins_lifecycle_get_context(plugin, &context) == SAO_OK && context != nullptr;
    probe.load_context = context;
    return context == nullptr ? SAO_ERR_NOT_INITIALIZED
                              : sao_plugins_ctx_register_ui_panel(context, "rollback_panel",
                                                                  R"({"title":"Rollback"})",
                                                                  nullptr, nullptr, nullptr);
}

int32_t SAO_PLUGINS_CALL probe_adapter_on_load(plugin_handle_t plugin, void* user_data) {
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

int32_t SAO_PLUGINS_CALL probe_adapter_on_unload(plugin_handle_t plugin, bool* allow,
                                                 void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.on_unload_calls;
    plugin_context_t* context = nullptr;
    const auto extensions =
        snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel);
    probe.extension_visible_during_on_unload =
        std::any_of(extensions.begin(), extensions.end(),
                    [](const auto& item) { return item.id == "rollback_panel"; });
    if (sao_plugins_lifecycle_get_context(plugin, &context) != SAO_OK ||
        context != probe.load_context) {
        return SAO_ERR_NOT_INITIALIZED;
    }
    probe.unload_context = context;
    *allow = probe.allow_unload;
    return probe.on_unload_status;
}

int32_t SAO_PLUGINS_CALL probe_adapter_unload(plugin_handle_t plugin, void* user_data) {
    auto& probe = *static_cast<adapter_probe*>(user_data);
    ++probe.unload_calls;
    plugin_context_t* context = nullptr;
    const auto extensions =
        snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel);
    probe.extension_removed_before_adapter_unload =
        std::none_of(extensions.begin(), extensions.end(),
                     [](const auto& item) { return item.id == "rollback_panel"; });
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
        path =
            fs::path(base) / (std::wstring(L"sao_loader_") + label + L"_" + std::to_wstring(stamp));
        REQUIRE(fs::create_directories(path));
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string path_utf8(const fs::path& path) {
    const auto wide = path.native();
    const auto length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                        result.data(), length, nullptr, nullptr);
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
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    REQUIRE(handle != nullptr);
    return handle;
}

void remove_plugin(plugin_handle_t handle) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) == SAO_OK);
}

void put16(std::ofstream& output, uint16_t value) {
    const unsigned char bytes[] = {static_cast<unsigned char>(value),
                                   static_cast<unsigned char>(value >> 8)};
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
        put32(output, 0x04034b50U);
        put16(output, 20);
        put16(output, 0);
        put16(output, item.method);
        put16(output, 0);
        put16(output, 0);
        put32(output, 0);
        put32(output, static_cast<uint32_t>(item.data.size()));
        put32(output, static_cast<uint32_t>(item.data.size()));
        put16(output, static_cast<uint16_t>(item.name.size()));
        put16(output, 0);
        output.write(item.name.data(), static_cast<std::streamsize>(item.name.size()));
        output.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
    }
    const auto central_offset = static_cast<uint32_t>(output.tellp());
    for (const auto& item : items) {
        put32(output, 0x02014b50U);
        put16(output, 20);
        put16(output, 20);
        put16(output, 0);
        put16(output, item.method);
        put16(output, 0);
        put16(output, 0);
        put32(output, 0);
        put32(output, static_cast<uint32_t>(item.data.size()));
        put32(output, static_cast<uint32_t>(item.data.size()));
        put16(output, static_cast<uint16_t>(item.name.size()));
        put16(output, 0);
        put16(output, 0);
        put16(output, 0);
        put16(output, 0);
        put32(output, 0);
        put32(output, item.local_offset);
        output.write(item.name.data(), static_cast<std::streamsize>(item.name.size()));
    }
    const auto central_end = static_cast<uint32_t>(output.tellp());
    put32(output, 0x06054b50U);
    put16(output, 0);
    put16(output, 0);
    put16(output, static_cast<uint16_t>(items.size()));
    put16(output, static_cast<uint16_t>(items.size()));
    put32(output, central_end - central_offset);
    put32(output, central_offset);
    put16(output, 0);
    REQUIRE(output.good());
}

std::atomic_int g_adapter_loads{0};
std::atomic_int g_adapter_enables{0};
std::atomic_int g_adapter_disables{0};
std::atomic_int g_adapter_unloads{0};
std::atomic_bool g_adapter_fail_on_load{false};

struct context_provider_probe {
    std::atomic_int actions{0};
    std::atomic_int snapshot_calls{0};
    std::string last_payload;
    uint64_t revision = 4;
    bool zero_rows = false;
    uint32_t row_struct_size = sizeof(entity_menu_row);
    const char* row_payload_json = R"({"value":7})";
    uint8_t row_can_activate = 1;
    uint8_t row_keep_menu_open = 1;
    uint8_t row_close_menu_before = 0;
    std::atomic_bool enable_entered{false};
    std::atomic_bool registration_started{false};
};

enum class snapshot_protocol_mode {
    stable,
    zero_buffer_too_small,
    shrink_fill,
    grow_fill,
    revision_mismatch,
    fill_buffer_too_small,
    invalid_fill_with_overflow_count,
};

struct snapshot_protocol_probe {
    snapshot_protocol_mode mode = snapshot_protocol_mode::stable;
    std::atomic_int calls{0};
};

void set_protocol_row(entity_menu_row& row, const char* action_id) {
    row = {sizeof(entity_menu_row),
           "protocol",
           "Protocol",
           "",
           0.0,
           "Run",
           "",
           action_id,
           "{}",
           1,
           0,
           0,
           {}};
}

int32_t SAO_PLUGINS_CALL snapshot_protocol_callback(entity_menu_row* rows, uint32_t capacity,
                                                    uint32_t* out_count, uint64_t* out_revision,
                                                    void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto& probe = *static_cast<snapshot_protocol_probe*>(user_data);
    ++probe.calls;
    *out_revision =
        rows != nullptr && probe.mode == snapshot_protocol_mode::revision_mismatch ? 10 : 9;
    if (rows == nullptr && capacity == 0) {
        if (probe.mode == snapshot_protocol_mode::zero_buffer_too_small) {
            *out_count = 0;
            return SAO_ERR_BUFFER_TOO_SMALL;
        }
        *out_count = probe.mode == snapshot_protocol_mode::shrink_fill ? 2 : 1;
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (probe.mode == snapshot_protocol_mode::invalid_fill_with_overflow_count) {
        *out_count = capacity + 1;
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (probe.mode == snapshot_protocol_mode::grow_fill) {
        *out_count = capacity + 1;
        return SAO_OK;
    }
    if (probe.mode == snapshot_protocol_mode::fill_buffer_too_small) {
        *out_count = capacity;
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (probe.mode == snapshot_protocol_mode::shrink_fill) {
        *out_count = 1;
        if (capacity > 0 && rows != nullptr)
            set_protocol_row(rows[0], "one");
        return SAO_OK;
    }
    *out_count = 1;
    if (capacity < 1 || rows == nullptr)
        return SAO_ERR_BUFFER_TOO_SMALL;
    set_protocol_row(rows[0], "one");
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL snapshot_protocol_action(const char*, const char*, void*) {
    return SAO_OK;
}

std::string nested_array_json(size_t depth) {
    return std::string(depth, '[') + "0" + std::string(depth, ']');
}

std::string flat_array_json(size_t scalar_count) {
    std::string value;
    value.reserve(scalar_count * 2 + 1);
    value.push_back('[');
    for (size_t index = 0; index < scalar_count; ++index) {
        if (index != 0)
            value.push_back(',');
        value.push_back('0');
    }
    value.push_back(']');
    return value;
}

class entity_provider_counter_guard final {
  public:
    entity_provider_counter_guard() : saved_(entity_provider_get_counters_for_testing()) {}
    ~entity_provider_counter_guard() {
        entity_provider_set_counters_for_testing(saved_);
    }

    entity_provider_counter_guard(const entity_provider_counter_guard&) = delete;
    entity_provider_counter_guard& operator=(const entity_provider_counter_guard&) = delete;

  private:
    entity_provider_test_counters saved_;
};

struct old_context_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
};

struct future_context_entity_provider_descriptor {
    context_entity_provider_descriptor current{};
    uint64_t future_tail = 0;
};

struct future_native_entity_provider_descriptor {
    native_entity_provider_descriptor current{};
    uint64_t future_tail = 0;
};

static_assert(sizeof(old_context_entity_provider_descriptor) == 40);

int32_t SAO_PLUGINS_CALL context_provider_snapshot(entity_menu_row* rows, uint32_t capacity,
                                                   uint32_t* out_count, uint64_t* out_revision,
                                                   void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& probe = *static_cast<context_provider_probe*>(user_data);
    ++probe.snapshot_calls;
    *out_revision = probe.revision;
    if (probe.zero_rows) {
        *out_count = 0;
        return SAO_OK;
    }
    *out_count = 1;
    if (capacity < 1)
        return SAO_ERR_BUFFER_TOO_SMALL;
    if (rows == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    rows[0] = {probe.row_struct_size,
               "tools",
               "工具",
               "🔧",
               12.5,
               "执行",
               "▶",
               "run",
               probe.row_payload_json,
               probe.row_can_activate,
               probe.row_keep_menu_open,
               probe.row_close_menu_before,
               {}};
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL context_provider_action(const char*, const char* payload_json,
                                                 void* user_data) {
    auto& probe = *static_cast<context_provider_probe*>(user_data);
    ++probe.actions;
    probe.last_payload = payload_json == nullptr ? "" : payload_json;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL context_provider_adapter_load(plugin_handle_t plugin,
                                                       const plugin_manifest*, void* user_data) {
    plugin_context_t* context = nullptr;
    int32_t status = sao_plugins_lifecycle_get_context(plugin, &context);
    if (status != SAO_OK)
        return status;
    entity_root_contribution_descriptor root{};
    root.struct_size = sizeof(root);
    root.contribution_id_utf8 = "tools-root";
    root.root_id_utf8 = "plugin:context-provider-tools";
    root.name_utf8 = "动态工具";
    root.icon_utf8 = "🔧";
    root.priority = 12.5;
    context_entity_provider_descriptor provider{};
    provider.struct_size = sizeof(provider);
    provider.provider_id_utf8 = "tools-provider";
    provider.snapshot = context_provider_snapshot;
    provider.action_handler = context_provider_action;
    provider.user_data = user_data;
    provider.root_contribution = &root;
    return sao_plugins_ctx_register_entity_provider(context, &provider);
}

int32_t SAO_PLUGINS_CALL context_provider_enable_then_fail(plugin_handle_t plugin,
                                                           void* user_data) {
    const int32_t status = context_provider_adapter_load(plugin, nullptr, user_data);
    return status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status;
}

int32_t SAO_PLUGINS_CALL context_provider_enable_registration_race(plugin_handle_t,
                                                                   void* user_data) {
    auto& probe = *static_cast<context_provider_probe*>(user_data);
    probe.enable_entered.store(true);
    while (!probe.registration_started.load())
        std::this_thread::yield();
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL context_provider_simple(plugin_handle_t, void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL context_provider_noop_load(plugin_handle_t, const plugin_manifest*,
                                                    void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL context_provider_on_unload(plugin_handle_t, bool* allow, void*) {
    *allow = true;
    return SAO_OK;
}

host_adapter_vtable context_provider_adapter(context_provider_probe* probe) {
    host_adapter_vtable adapter{};
    adapter.load_plugin = context_provider_adapter_load;
    adapter.call_on_load = context_provider_simple;
    adapter.call_on_enable = context_provider_simple;
    adapter.call_on_disable = context_provider_simple;
    adapter.call_on_unload = context_provider_on_unload;
    adapter.unload_plugin = context_provider_simple;
    adapter.host_user_data = probe;
    return adapter;
}

host_adapter_vtable failing_enable_context_provider_adapter(context_provider_probe* probe) {
    host_adapter_vtable adapter{};
    adapter.load_plugin = context_provider_noop_load;
    adapter.call_on_load = context_provider_simple;
    adapter.call_on_enable = context_provider_enable_then_fail;
    adapter.call_on_disable = context_provider_simple;
    adapter.call_on_unload = context_provider_on_unload;
    adapter.unload_plugin = context_provider_simple;
    adapter.host_user_data = probe;
    return adapter;
}

host_adapter_vtable empty_context_provider_adapter(context_provider_probe* probe) {
    host_adapter_vtable adapter{};
    adapter.load_plugin = context_provider_noop_load;
    adapter.call_on_load = context_provider_simple;
    adapter.call_on_enable = context_provider_simple;
    adapter.call_on_disable = context_provider_simple;
    adapter.call_on_unload = context_provider_on_unload;
    adapter.unload_plugin = context_provider_simple;
    adapter.host_user_data = probe;
    return adapter;
}

host_adapter_vtable enable_registration_race_adapter(context_provider_probe* probe) {
    auto adapter = empty_context_provider_adapter(probe);
    adapter.call_on_enable = context_provider_enable_registration_race;
    return adapter;
}

struct revision_change_snapshot_context {
    plugin_handle_t plugin = nullptr;
    entity_provider_catalog_snapshot candidate;
    int32_t disable_status = SAO_ERR_OS_CALL_FAILED;
};

int32_t SAO_PLUGINS_CALL copy_catalog_then_disable(const entity_provider_catalog_view* catalog,
                                                   void* user_data) {
    auto& context = *static_cast<revision_change_snapshot_context*>(user_data);
    const int32_t status = copy_entity_provider_catalog(catalog, &context.candidate);
    if (status != SAO_OK)
        return status;
    context.disable_status = sao_plugins_lifecycle_disable(context.plugin);
    return context.disable_status;
}

int32_t snapshot_entity_providers_while_disabling(plugin_handle_t plugin,
                                                  entity_provider_catalog_snapshot& out,
                                                  int32_t& disable_status) {
    revision_change_snapshot_context context;
    context.plugin = plugin;
    const int32_t status =
        sao_plugins_entity_provider_snapshot(copy_catalog_then_disable, &context);
    disable_status = context.disable_status;
    if (status == SAO_OK)
        out = std::move(context.candidate);
    return status;
}

int32_t SAO_PLUGINS_CALL adapter_load(plugin_handle_t, const plugin_manifest*, void*) {
    ++g_adapter_loads;
    return SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_simple_load(plugin_handle_t, void*) {
    return g_adapter_fail_on_load ? SAO_ERR_OS_CALL_FAILED : SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_enable(plugin_handle_t, void*) {
    ++g_adapter_enables;
    return SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_disable(plugin_handle_t, void*) {
    ++g_adapter_disables;
    return SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t, bool* allow, void*) {
    *allow = true;
    return SAO_OK;
}
int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t, void*) {
    ++g_adapter_unloads;
    return SAO_OK;
}

int32_t render_panel(const char*, char**, void*) {
    return SAO_OK;
}

struct blocking_event_probe {
    std::mutex mutex;
    std::condition_variable entered_cv;
    std::condition_variable release_cv;
    bool entered = false;
    bool release = false;
    std::atomic_int calls{0};
};

void blocking_event_callback(const char*, const char*, void* user_data) {
    auto& probe = *static_cast<blocking_event_probe*>(user_data);
    ++probe.calls;
    std::unique_lock lock(probe.mutex);
    probe.entered = true;
    probe.entered_cv.notify_all();
    probe.release_cv.wait(lock, [&probe] { return probe.release; });
}

struct reentrant_event_probe {
    plugin_context_t* context = nullptr;
    uint32_t token = 0;
    int32_t unsubscribe_status = SAO_ERR_OS_CALL_FAILED;
    std::atomic_int calls{0};
};

void reentrant_event_callback(const char*, const char*, void* user_data) {
    auto& probe = *static_cast<reentrant_event_probe*>(user_data);
    ++probe.calls;
    probe.unsubscribe_status = sao_plugins_ctx_unsubscribe(probe.context, probe.token);
}

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

TEST_CASE("scanner discovers valid directories and computes a changing signature",
          "[plugins][loader][scanner]") {
    TempDirectory temp(L"scanner");
    const auto plugin_dir = temp.path / L"plugins" / L"one";
    write_text(plugin_dir / L"plugin.json",
               R"({"id":"scanner_case","entry":"plugin.emma","language":"emma"})");
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
    write_text(plugin_dir / L"plugin.json",
               R"({"id":"scanner_case","entry":"plugin.emma","language":"emma","version":"2"})");
    REQUIRE(sao_plugins_scanner_signature(&config) != signature_before);

    fs::create_directory(temp.path / L".git");
    wchar_t* workspace = nullptr;
    REQUIRE(sao_plugins_scanner_find_workspace_root(plugin_dir.c_str(), &workspace) == SAO_OK);
    REQUIRE(fs::equivalent(workspace, temp.path));
    sao_plugins_scanner_free_wstring(workspace);
}

TEST_CASE("registry owns records and context implements settings and local events",
          "[plugins][loader][context]") {
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
    REQUIRE(sao_plugins_ctx_subscribe_once(
                context, "tick",
                +[](const char*, const char*, void* user) { ++*static_cast<int*>(user); },
                &callbacks, &token) == SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "tick", R"({"value":1})") == SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "tick", R"({"value":2})") == SAO_OK);
    REQUIRE(callbacks == 1);
    char* snapshot = nullptr;
    REQUIRE(sao_plugins_ctx_snapshot_value(context, "tick", &snapshot) == SAO_OK);
    REQUIRE(std::string(snapshot).find("2") != std::string::npos);
    sao_plugins_ctx_free_string(snapshot);

    REQUIRE(sao_plugins_ctx_register_ui_panel(context, "panel", R"({"title":"Panel"})", nullptr,
                                              nullptr, nullptr) == SAO_OK);
    REQUIRE(snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel)
                .back()
                .plugin_id == "context_case");
    REQUIRE(sao_plugins_ctx_register_ui_panel(context, "live_panel", R"({"title":"Live"})",
                                              render_panel, nullptr,
                                              nullptr) == SAO_PLUGINS_ERR_UNSUPPORTED);
    REQUIRE(sao_plugins_ctx_set_timeout(context, nullptr, 1.0, nullptr, nullptr) ==
            SAO_PLUGINS_ERR_UNSUPPORTED);
    sao_plugins_ctx_destroy(context);
    remove_plugin(handle);
}

TEST_CASE("generic context JSON inputs are bounded and preserve committed state",
          "[plugins][loader][context][json][bounds][transaction][focused]") {
    TempDirectory temp(L"context_json_bounds");
    write_text(temp.path / L"plugin.emma", "entry");
    auto handle = add_plugin(make_manifest("context_json_bounds", temp.path));
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    constexpr size_t kMaximumContextJsonBytes = 1024 * 1024;
    const std::string maximum_depth = nested_array_json(64);
    const std::string excessive_depth = nested_array_json(65);
    const std::string maximum_nodes = flat_array_json(16383);
    const std::string excessive_nodes = flat_array_json(16384);
    const std::string maximum_bytes =
        "\"" + std::string(kMaximumContextJsonBytes - 2, 'x') + "\"";
    const std::string excessive_bytes =
        "\"" + std::string(kMaximumContextJsonBytes - 1, 'x') + "\"";
    std::string invalid_utf8 = "\"";
    invalid_utf8.push_back(static_cast<char>(0xc3));
    invalid_utf8 += "(\"";

    REQUIRE(sao_plugins_ctx_set_setting(context, "depth", maximum_depth.c_str()) == SAO_OK);
    CHECK(sao_plugins_ctx_set_setting(context, "depth", excessive_depth.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    char* setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "depth", &setting) == SAO_OK);
    CHECK(std::string(setting) == maximum_depth);
    sao_plugins_ctx_free_string(setting);

    REQUIRE(sao_plugins_ctx_set_setting(context, "nodes", maximum_nodes.c_str()) == SAO_OK);
    CHECK(sao_plugins_ctx_set_setting(context, "nodes", excessive_nodes.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "nodes", &setting) == SAO_OK);
    REQUIRE(std::strlen(setting) == maximum_nodes.size());
    CHECK(setting[0] == '[');
    CHECK(setting[maximum_nodes.size() - 1] == ']');
    sao_plugins_ctx_free_string(setting);

    REQUIRE(sao_plugins_ctx_set_setting(context, "bytes", maximum_bytes.c_str()) == SAO_OK);
    CHECK(sao_plugins_ctx_set_setting(context, "bytes", excessive_bytes.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "bytes", &setting) == SAO_OK);
    REQUIRE(std::strlen(setting) == maximum_bytes.size());
    CHECK(setting[0] == '"');
    CHECK(setting[maximum_bytes.size() - 1] == '"');
    sao_plugins_ctx_free_string(setting);

    REQUIRE(sao_plugins_ctx_set_setting(context, "scalar", "7") == SAO_OK);
    CHECK(sao_plugins_ctx_set_setting(context, "scalar", invalid_utf8.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_set_setting(context, "scalar", "1e400") ==
          SAO_ERR_INVALID_ARGUMENT);
    setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "scalar", &setting) == SAO_OK);
    CHECK(std::string(setting) == "7");
    sao_plugins_ctx_free_string(setting);

    REQUIRE(sao_plugins_ctx_set_defaults(context, R"({"kept":1})") == SAO_OK);
    const std::string invalid_defaults =
        R"({"introduced":2,"nested":)" + excessive_depth + "}";
    CHECK(sao_plugins_ctx_set_defaults(context, invalid_defaults.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "kept", &setting) == SAO_OK);
    CHECK(std::string(setting) == "1");
    sao_plugins_ctx_free_string(setting);
    setting = reinterpret_cast<char*>(1);
    CHECK(sao_plugins_ctx_get_setting(context, "introduced", &setting) ==
          SAO_ERR_HANDLE_INVALID);
    CHECK(setting == nullptr);

    int callbacks = 0;
    uint32_t token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(
                context, "bounded-event",
                +[](const char*, const char*, void* user_data) {
                    ++*static_cast<int*>(user_data);
                },
                &callbacks, &token) == SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "bounded-event", R"({"state":"old"})") == SAO_OK);
    CHECK(sao_plugins_ctx_emit(context, "bounded-event", excessive_nodes.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(callbacks == 1);
    char* snapshot = nullptr;
    REQUIRE(sao_plugins_ctx_snapshot_value(context, "bounded-event", &snapshot) == SAO_OK);
    REQUIRE(std::strlen(snapshot) == std::string_view(R"({"state":"old"})").size());
    CHECK(std::string(snapshot) == R"({"state":"old"})");
    sao_plugins_ctx_free_string(snapshot);

    CHECK(sao_plugins_ctx_set_overlay(context, "bounded-overlay", excessive_depth.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_set_overlay(context, "bounded-overlay", maximum_depth.c_str()) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    wchar_t* selected_path = reinterpret_cast<wchar_t*>(1);
    CHECK(sao_plugins_ctx_open_file(context, excessive_nodes.c_str(), nullptr, nullptr, 0,
                                    &selected_path) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(selected_path == nullptr);
    selected_path = reinterpret_cast<wchar_t*>(1);
    CHECK(sao_plugins_ctx_open_file(context, maximum_nodes.c_str(), nullptr, nullptr, 0,
                                    &selected_path) == SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(selected_path == nullptr);

    REQUIRE(sao_plugins_ctx_register_ui_panel(context, "bounded-panel", R"({"title":"old"})",
                                              nullptr, nullptr, nullptr) == SAO_OK);
    REQUIRE(sao_plugins_ctx_register_extension(context, "formatter", "bounded-extension",
                                               R"({"title":"old"})", nullptr, nullptr) ==
            SAO_OK);
    REQUIRE(sao_plugins_ctx_register_menu_surface(context, "bounded-surface",
                                                  R"({"title":"old"})", 0.0F) == SAO_OK);
    REQUIRE(sao_plugins_ctx_register_data_source(
                context, "bounded-source", R"({"title":"old"})",
                +[](void*) -> int32_t { return SAO_OK; },
                +[](void*) -> int32_t { return SAO_OK; }, nullptr) == SAO_OK);

    const std::string deep_metadata =
        R"({"title":"new","value":)" + excessive_depth + "}";
    const std::string node_metadata =
        R"({"title":"new","value":)" + excessive_nodes + "}";
    const std::string oversized_metadata =
        R"({"title":")" + std::string(kMaximumContextJsonBytes, 'x') + R"("})";
    CHECK(sao_plugins_ctx_register_ui_panel(context, "bounded-panel", invalid_utf8.c_str(),
                                            nullptr, nullptr, nullptr) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_register_extension(context, "formatter", "bounded-extension",
                                             deep_metadata.c_str(), nullptr, nullptr) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_register_menu_surface(context, "bounded-surface",
                                                node_metadata.c_str(), 0.0F) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_ctx_register_data_source(
              context, "bounded-source", oversized_metadata.c_str(),
              +[](void*) -> int32_t { return SAO_OK; },
              +[](void*) -> int32_t { return SAO_OK; }, nullptr) ==
          SAO_ERR_INVALID_ARGUMENT);

    for (const auto kind : {extension_kind::ui_panel, extension_kind::formatter,
                            extension_kind::menu_category, extension_kind::data_source}) {
        const auto extensions = snapshot_extensions(sao_plugins_registry_instance(), kind);
        const auto preserved = std::find_if(
            extensions.begin(), extensions.end(), [](const extension_record& record) {
                return record.plugin_id == "context_json_bounds" &&
                       (record.id == "bounded-panel" || record.id == "bounded-extension" ||
                        record.id == "bounded-surface" || record.id == "bounded-source");
            });
        REQUIRE(preserved != extensions.end());
        CHECK(preserved->title == "old");
        CHECK(preserved->payload_json == R"({"title":"old"})");
    }

    REQUIRE(sao_plugins_ctx_unsubscribe(context, token) == SAO_OK);
    sao_plugins_ctx_destroy(context);
    remove_plugin(handle);
}

TEST_CASE("event subscription invocation leases drain concurrent unsubscribe",
          "[plugins][loader][context][event][concurrency][focused]") {
    TempDirectory temp(L"event_subscription_lease");
    write_text(temp.path / L"plugin.emma", "entry");
    auto handle = add_plugin(make_manifest("event_subscription_lease", temp.path));
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    blocking_event_probe probe;
    uint32_t token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(context, "lease", blocking_event_callback, &probe, &token) ==
            SAO_OK);
    std::atomic_int emit_status{SAO_ERR_OS_CALL_FAILED};
    std::atomic_int unsubscribe_status{SAO_ERR_OS_CALL_FAILED};
    std::atomic_bool unsubscribe_finished{false};
    std::jthread emitter([&] { emit_status.store(sao_plugins_ctx_emit(context, "lease", "{}")); });
    {
        std::unique_lock lock(probe.mutex);
        probe.entered_cv.wait(lock, [&probe] { return probe.entered; });
    }
    std::jthread unsubscriber([&] {
        unsubscribe_status.store(sao_plugins_ctx_unsubscribe(context, token));
        unsubscribe_finished.store(true);
    });
    for (int attempt = 0; attempt < 100 && !unsubscribe_finished.load(); ++attempt)
        std::this_thread::yield();
    CHECK_FALSE(unsubscribe_finished.load());
    {
        std::lock_guard lock(probe.mutex);
        probe.release = true;
    }
    probe.release_cv.notify_all();
    emitter.join();
    unsubscriber.join();
    CHECK(emit_status.load() == SAO_OK);
    CHECK(unsubscribe_status.load() == SAO_OK);
    CHECK(probe.calls.load() == 1);
    REQUIRE(sao_plugins_ctx_emit(context, "lease", "{}") == SAO_OK);
    CHECK(probe.calls.load() == 1);

    sao_plugins_ctx_destroy(context);
    remove_plugin(handle);
}

TEST_CASE("event subscription rejects callback-thread teardown and serializes one-shot",
          "[plugins][loader][context][event][reentry][one-shot][focused]") {
    TempDirectory temp(L"event_subscription_reentry");
    write_text(temp.path / L"plugin.emma", "entry");
    auto handle = add_plugin(make_manifest("event_subscription_reentry", temp.path));
    auto* context = sao_plugins_ctx_create(handle);
    REQUIRE(context != nullptr);

    reentrant_event_probe reentry{context};
    REQUIRE(sao_plugins_ctx_subscribe(context, "reentry", reentrant_event_callback, &reentry,
                                      &reentry.token) == SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "reentry", "{}") == SAO_OK);
    CHECK(reentry.calls.load() == 1);
    CHECK(reentry.unsubscribe_status == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_ctx_unsubscribe(context, reentry.token) == SAO_OK);

    std::atomic_int one_shot_calls{0};
    uint32_t once_token = 0;
    REQUIRE(sao_plugins_ctx_subscribe_once(
                context, "once",
                +[](const char*, const char*, void* user_data) {
                    ++*static_cast<std::atomic_int*>(user_data);
                },
                &one_shot_calls, &once_token) == SAO_OK);
    std::atomic_int first_status{SAO_ERR_OS_CALL_FAILED};
    std::atomic_int second_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread first([&] { first_status.store(sao_plugins_ctx_emit(context, "once", "{}")); });
    std::jthread second([&] { second_status.store(sao_plugins_ctx_emit(context, "once", "{}")); });
    first.join();
    second.join();
    CHECK(first_status.load() == SAO_OK);
    CHECK(second_status.load() == SAO_OK);
    CHECK(one_shot_calls.load() == 1);
    CHECK(sao_plugins_ctx_unsubscribe(context, once_token) == SAO_ERR_HANDLE_INVALID);

    sao_plugins_ctx_destroy(context);
    remove_plugin(handle);
}

TEST_CASE("dependency bootstrap is local-only and fails closed for missing installs",
          "[plugins][loader][deps]") {
    TempDirectory temp(L"deps");
    fs::create_directories(temp.path / L"libs" / L"PIL");
    write_text(temp.path / L"requirements.txt", "Pillow>=10\nmissing-dist==1\n");
    deps_bootstrap_record record;
    REQUIRE(sao_plugins_deps_ensure(temp.path.c_str(), false, &record) ==
            SAO_PLUGINS_ERR_DEPENDENCY_MISSING);
    REQUIRE(record.deps_summary.at("pillow") == "libs");
    REQUIRE(record.deps_summary.at("missing-dist") == "missing");
    REQUIRE(sao_plugins_deps_ensure(temp.path.c_str(), true, &record) ==
            SAO_PLUGINS_ERR_UNSUPPORTED);
}

TEST_CASE("topological ordering rejects missing dependencies and cycles",
          "[plugins][loader][deps][lifecycle]") {
    auto c = add_plugin(make_manifest("topo_c", fs::path{}));
    auto b = add_plugin(make_manifest("topo_b", fs::path{}, {"topo_c"}));
    auto a = add_plugin(make_manifest("topo_a", fs::path{}, {"topo_b>=1"}));
    plugin_handle_t input[] = {a, c, b};
    plugin_handle_t output[3]{};
    REQUIRE(sao_plugins_lifecycle_topo_sort(input, 3, output) == SAO_OK);
    REQUIRE(output[0] == c);
    REQUIRE(output[1] == b);
    REQUIRE(output[2] == a);
    remove_plugin(a);
    remove_plugin(b);
    remove_plugin(c);

    auto missing = add_plugin(make_manifest("topo_missing", fs::path{}, {"absent"}));
    plugin_handle_t missing_output{};
    REQUIRE(sao_plugins_lifecycle_topo_sort(&missing, 1, &missing_output) ==
            SAO_PLUGINS_ERR_DEPENDENCY_MISSING);
    remove_plugin(missing);

    auto cycle_a = add_plugin(make_manifest("cycle_a", fs::path{}, {"cycle_b"}));
    auto cycle_b = add_plugin(make_manifest("cycle_b", fs::path{}, {"cycle_a"}));
    plugin_handle_t cycle_input[] = {cycle_a, cycle_b};
    plugin_handle_t cycle_output[2]{};
    REQUIRE(sao_plugins_lifecycle_topo_sort(cycle_input, 2, cycle_output) ==
            SAO_PLUGINS_ERR_DEPENDENCY_CYCLE);
    remove_plugin(cycle_a);
    remove_plugin(cycle_b);
}

TEST_CASE("host adapter lifecycle executes symmetric transitions", "[plugins][loader][lifecycle]") {
    TempDirectory temp(L"adapter");
    write_text(temp.path / L"plugin.emma", "entry");
    install_emma_adapter();
    g_adapter_loads = 0;
    g_adapter_enables = 0;
    g_adapter_disables = 0;
    g_adapter_unloads = 0;
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
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);

    adapter_probe probe;
    const auto adapter = probe_adapter_vtable(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::lua, &adapter) == SAO_OK);
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
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::lua) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("context provider publishes owner-scoped roots with loader lifecycle",
          "[plugins][loader][entity-provider][root-contribution][focused]") {
    TempDirectory temp(L"context_provider_root");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_root", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);

    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.roots.size() == 1);
    const auto& provider = catalog.providers[0];
    const auto& root = catalog.roots[0];
    CHECK(provider.provider_id == "context_provider_root/tools-provider");
    CHECK(provider.owner_plugin_id == "context_provider_root");
    CHECK(provider.generation > 0);
    CHECK(provider.revision == 4);
    REQUIRE(provider.rows.size() == 1);
    CHECK(provider.rows[0].category_priority == 12.5);
    CHECK(provider.rows[0].keep_menu_open);
    CHECK(provider.rows[0].payload_json == R"({"value":7})");
    CHECK(root.owner_plugin_id == "context_provider_root");
    CHECK(root.contribution_id == "tools-root");
    CHECK(root.root_id == "plugin:context-provider-tools");
    CHECK(root.name == "动态工具");
    CHECK(root.priority == 12.5);
    REQUIRE(root.actions.size() == 1);
    CHECK(root.actions[0] ==
          std::pair{std::string("context_provider_root/tools-provider"), std::string("run")});

    const uint64_t generation = provider.generation;
    REQUIRE(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), generation, "run",
                                               R"({"from":"test"})") == SAO_OK);
    CHECK(probe.actions == 1);
    CHECK(probe.last_payload == R"({"from":"test"})");

    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_entity_provider_invoke("context_provider_root/tools-provider", generation,
                                             "run", "{}") == SAO_PLUGINS_ERR_BUSY);

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.providers[0].generation == generation);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke("context_provider_root/tools-provider", generation,
                                             "run", "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("context entity provider ABI accepts required prefixes and future tails",
          "[plugins][loader][entity-provider][abi][focused]") {
    TempDirectory temp(L"context_provider_abi_prefix");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_abi_prefix", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = empty_context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    old_context_entity_provider_descriptor old_descriptor{
        sizeof(old_context_entity_provider_descriptor),
        "old-prefix",
        context_provider_snapshot,
        context_provider_action,
        &probe,
    };
    REQUIRE(sao_plugins_ctx_register_entity_provider(
                context, reinterpret_cast<const context_entity_provider_descriptor*>(
                             &old_descriptor)) == SAO_OK);

    old_context_entity_provider_descriptor short_descriptor{
        sizeof(old_context_entity_provider_descriptor) - 1,
        "short-prefix",
        context_provider_snapshot,
        context_provider_action,
        &probe,
    };
    CHECK(sao_plugins_ctx_register_entity_provider(
              context, reinterpret_cast<const context_entity_provider_descriptor*>(
                           &short_descriptor)) == SAO_PLUGINS_ERR_ABI_MISMATCH);

    context_entity_provider_descriptor recovered{};
    recovered.struct_size = sizeof(recovered);
    recovered.provider_id_utf8 = "short-prefix";
    recovered.snapshot = context_provider_snapshot;
    recovered.action_handler = context_provider_action;
    recovered.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider(context, &recovered) == SAO_OK);

    future_context_entity_provider_descriptor future{};
    future.current.struct_size = sizeof(future);
    future.current.provider_id_utf8 = "future-prefix";
    future.current.snapshot = context_provider_snapshot;
    future.current.action_handler = context_provider_action;
    future.current.user_data = &probe;
    future.future_tail = 0xabcdef0123456789ULL;
    REQUIRE(sao_plugins_ctx_register_entity_provider(context, &future.current) == SAO_OK);

    std::array<future_native_entity_provider_descriptor, 2> native_future{};
    native_future[0].current.struct_size = sizeof(future_native_entity_provider_descriptor);
    native_future[0].current.provider_id_utf8 = "native-future-0";
    native_future[0].current.snapshot = context_provider_snapshot;
    native_future[0].current.action_handler = context_provider_action;
    native_future[0].current.user_data = &probe;
    native_future[0].future_tail = 0x1111111111111111ULL;
    native_future[1].current.struct_size = sizeof(future_native_entity_provider_descriptor);
    native_future[1].current.provider_id_utf8 = "native-future-1";
    native_future[1].current.snapshot = context_provider_snapshot;
    native_future[1].current.action_handler = context_provider_action;
    native_future[1].current.user_data = &probe;
    native_future[1].future_tail = 0x2222222222222222ULL;
    REQUIRE(plugin_context_register_entity_providers(
                context,
                reinterpret_cast<const native_entity_provider_descriptor*>(native_future.data()),
                native_future.size()) == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.size() == 5);
    CHECK(catalog.roots.empty());
    CHECK(std::any_of(catalog.providers.begin(), catalog.providers.end(), [](const auto& item) {
        return item.provider_id == "context_provider_abi_prefix/old-prefix";
    }));
    CHECK(std::any_of(catalog.providers.begin(), catalog.providers.end(), [](const auto& item) {
        return item.provider_id == "context_provider_abi_prefix/short-prefix";
    }));
    CHECK(std::any_of(catalog.providers.begin(), catalog.providers.end(), [](const auto& item) {
        return item.provider_id == "context_provider_abi_prefix/future-prefix";
    }));
    CHECK(std::any_of(catalog.providers.begin(), catalog.providers.end(), [](const auto& item) {
        return item.provider_id == "context_provider_abi_prefix/native-future-0";
    }));
    CHECK(std::any_of(catalog.providers.begin(), catalog.providers.end(), [](const auto& item) {
        return item.provider_id == "context_provider_abi_prefix/native-future-1";
    }));

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity provider rows enforce mandatory flags and JSON atomically",
          "[plugins][loader][entity-provider][abi][json][focused]") {
    TempDirectory temp(L"context_provider_row_abi");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_row_abi", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    probe.row_struct_size = 75;
    probe.row_can_activate = 0;
    probe.row_keep_menu_open = 1;
    probe.row_close_menu_before = 1;
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK_FALSE(catalog.providers[0].rows[0].can_activate);
    CHECK(catalog.providers[0].rows[0].keep_menu_open);
    CHECK(catalog.providers[0].rows[0].close_menu_before);
    const uint64_t generation = catalog.providers[0].generation;

    probe.row_struct_size = 74;
    entity_provider_catalog_snapshot sentinel;
    sentinel.revision = 0xfeed;
    CHECK(snapshot_entity_providers(sentinel) == SAO_PLUGINS_ERR_ABI_MISMATCH);
    CHECK(sentinel.revision == 0xfeed);
    CHECK(sentinel.providers.empty());

    probe.row_struct_size = sizeof(entity_menu_row) + 32;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.providers[0].rows[0].close_menu_before);

    probe.row_payload_json = nullptr;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK(catalog.providers[0].rows[0].payload_json == "{}");

    probe.row_payload_json = "{invalid";
    sentinel = {};
    sentinel.revision = 0xbeef;
    CHECK(snapshot_entity_providers(sentinel) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sentinel.revision == 0xbeef);
    CHECK(sentinel.providers.empty());

    const int action_calls = probe.actions.load();
    CHECK(sao_plugins_entity_provider_invoke("context_provider_row_abi/tools-provider", generation,
                                             "run", "{invalid") == SAO_ERR_INVALID_ARGUMENT);
    CHECK(probe.actions.load() == action_calls);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity provider JSON complexity is bounded before publication and invoke",
          "[plugins][loader][entity-provider][json][complexity][d6][focused]") {
    TempDirectory temp(L"context_provider_json_complexity");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_json_complexity", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    const std::string maximum_depth = nested_array_json(64);
    probe.row_payload_json = maximum_depth.c_str();
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK(catalog.providers[0].rows[0].payload_json == maximum_depth);
    const uint64_t generation = catalog.providers[0].generation;

    const std::string raw_payload = R"({ "text": "\u0041", "number": 1.0, "items": [ 1, 2 ] })";
    probe.row_payload_json = raw_payload.c_str();
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers[0].rows[0].payload_json == raw_payload);
    REQUIRE(sao_plugins_entity_provider_invoke("context_provider_json_complexity/tools-provider",
                                               generation, "run", raw_payload.c_str()) == SAO_OK);
    CHECK(probe.last_payload == raw_payload);

    const std::string excessive_depth = nested_array_json(65);
    probe.row_payload_json = excessive_depth.c_str();
    entity_provider_catalog_snapshot sentinel;
    sentinel.revision = 0xd600;
    CHECK(snapshot_entity_providers(sentinel) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sentinel.revision == 0xd600);
    CHECK(sentinel.providers.empty());

    const std::string maximum_nodes = flat_array_json(16383);
    REQUIRE(sao_plugins_entity_provider_invoke("context_provider_json_complexity/tools-provider",
                                               generation, "run", maximum_nodes.c_str()) == SAO_OK);
    CHECK(probe.last_payload == maximum_nodes);
    const int action_calls = probe.actions.load();
    const std::string excessive_nodes = flat_array_json(16384);
    CHECK(sao_plugins_entity_provider_invoke("context_provider_json_complexity/tools-provider",
                                             generation, "run",
                                             excessive_nodes.c_str()) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(probe.actions.load() == action_calls);

    const std::string maximum_row_bytes = "\"" + std::string(16382, 'x') + "\"";
    probe.row_payload_json = maximum_row_bytes.c_str();
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers[0].rows[0].payload_json == maximum_row_bytes);
    const std::string excessive_row_bytes = "\"" + std::string(16383, 'x') + "\"";
    probe.row_payload_json = excessive_row_bytes.c_str();
    sentinel.revision = 0xd602;
    sentinel.providers.clear();
    CHECK(snapshot_entity_providers(sentinel) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(sentinel.revision == 0xd602);
    CHECK(sentinel.providers.empty());

    const std::string maximum_invoke_bytes = "\"" + std::string(1048574, 'x') + "\"";
    REQUIRE(sao_plugins_entity_provider_invoke("context_provider_json_complexity/tools-provider",
                                               generation, "run",
                                               maximum_invoke_bytes.c_str()) == SAO_OK);
    CHECK(probe.last_payload == maximum_invoke_bytes);
    const int bounded_action_calls = probe.actions.load();
    const std::string excessive_invoke_bytes = "\"" + std::string(1048575, 'x') + "\"";
    CHECK(sao_plugins_entity_provider_invoke("context_provider_json_complexity/tools-provider",
                                             generation, "run", excessive_invoke_bytes.c_str()) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(probe.actions.load() == bounded_action_calls);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity provider snapshot rejects contradictory probe and fill results",
          "[plugins][loader][entity-provider][snapshot][protocol][d6][focused]") {
    TempDirectory temp(L"context_provider_snapshot_protocol");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_snapshot_protocol", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    snapshot_protocol_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "protocol-provider";
    descriptor.snapshot = snapshot_protocol_callback;
    descriptor.action_handler = snapshot_protocol_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);

    entity_provider_catalog_snapshot sentinel;
    sentinel.revision = 0xd601;
    probe.mode = snapshot_protocol_mode::zero_buffer_too_small;
    probe.calls = 0;
    CHECK(snapshot_entity_providers(sentinel) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(probe.calls == 1);
    CHECK(sentinel.revision == 0xd601);

    probe.mode = snapshot_protocol_mode::shrink_fill;
    probe.calls = 0;
    CHECK(snapshot_entity_providers(sentinel) == SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.calls == 18);
    CHECK(sentinel.revision == 0xd601);

    probe.mode = snapshot_protocol_mode::invalid_fill_with_overflow_count;
    probe.calls = 0;
    CHECK(snapshot_entity_providers(sentinel) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(probe.calls == 2);
    CHECK(sentinel.revision == 0xd601);

    for (const auto mode :
         {snapshot_protocol_mode::grow_fill, snapshot_protocol_mode::revision_mismatch,
          snapshot_protocol_mode::fill_buffer_too_small}) {
        probe.mode = mode;
        probe.calls = 0;
        CHECK(snapshot_entity_providers(sentinel) == SAO_PLUGINS_ERR_BUSY);
        CHECK(probe.calls == 18);
        CHECK(sentinel.revision == 0xd601);
    }

    probe.mode = snapshot_protocol_mode::stable;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity provider generation allocation exhausts without wrapping",
          "[plugins][loader][entity-provider][generation][overflow][d6][focused]") {
    entity_provider_counter_guard counter_guard;
    TempDirectory temp(L"context_provider_generation_overflow");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_generation_overflow", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = empty_context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);

    auto counters = entity_provider_get_counters_for_testing();
    counters.next_generation = (std::numeric_limits<uint64_t>::max)() - 1;
    entity_provider_set_counters_for_testing(counters);
    context_entity_provider_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.snapshot = context_provider_snapshot;
    descriptor.action_handler = context_provider_action;
    descriptor.user_data = &probe;
    descriptor.provider_id_utf8 = "last-generation";
    REQUIRE(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_OK);
    descriptor.provider_id_utf8 = "exhausted-generation";
    CHECK(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_ERR_OS_CALL_FAILED);
    descriptor.provider_id_utf8 = "still-exhausted-generation";
    CHECK(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_ERR_OS_CALL_FAILED);

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.size() == 1);
    const auto last_generation =
        std::find_if(catalog.providers.begin(), catalog.providers.end(), [](const auto& provider) {
            return provider.provider_id == "context_provider_generation_overflow/last-generation";
        });
    REQUIRE(last_generation != catalog.providers.end());
    CHECK(last_generation->generation == (std::numeric_limits<uint64_t>::max)() - 1);
    CHECK(entity_provider_get_counters_for_testing().next_generation ==
          (std::numeric_limits<uint64_t>::max)());

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity catalog revision exhaustion preserves publication and invoke",
          "[plugins][loader][entity-provider][revision][overflow][d6][focused]") {
    entity_provider_counter_guard counter_guard;
    TempDirectory temp(L"context_provider_catalog_revision_overflow");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_catalog_revision_overflow", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);

    auto counters = entity_provider_get_counters_for_testing();
    counters.catalog_revision = (std::numeric_limits<uint64_t>::max)() - 1;
    entity_provider_set_counters_for_testing(counters);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.revision == (std::numeric_limits<uint64_t>::max)());
    const uint64_t generation = catalog.providers[0].generation;

    context_entity_provider_descriptor extra{};
    extra.struct_size = sizeof(extra);
    extra.provider_id_utf8 = "revision-overflow";
    extra.snapshot = context_provider_snapshot;
    extra.action_handler = context_provider_action;
    extra.user_data = &probe;
    CHECK(sao_plugins_ctx_register_entity_provider(context, &extra) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.size() == 1);
    CHECK(catalog.revision == (std::numeric_limits<uint64_t>::max)());

    const char* provider_ids[] = {"context_provider_catalog_revision_overflow/tools-provider"};
    CHECK(plugin_context_unregister_entity_providers(context, provider_ids, 1) ==
          SAO_ERR_OS_CALL_FAILED);
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(sao_plugins_entity_provider_invoke(provider_ids[0], generation, "run", "{}") == SAO_OK);

    counters = entity_provider_get_counters_for_testing();
    counters.catalog_revision = (std::numeric_limits<uint64_t>::max)() - 1;
    entity_provider_set_counters_for_testing(counters);
    REQUIRE(plugin_context_unregister_entity_providers(context, provider_ids, 1) == SAO_OK);
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.revision == (std::numeric_limits<uint64_t>::max)());

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity provider context and unregister admissions enforce count budgets",
          "[plugins][loader][entity-provider][budget][focused]") {
    TempDirectory temp(L"context_provider_budget");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_budget", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = empty_context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    std::vector<std::string> local_ids;
    local_ids.reserve(257);
    for (size_t index = 0; index < 257; ++index)
        local_ids.push_back("budget-provider-" + std::to_string(index));
    for (size_t index = 0; index < 256; ++index) {
        context_entity_provider_descriptor descriptor{};
        descriptor.struct_size = sizeof(descriptor);
        descriptor.provider_id_utf8 = local_ids[index].c_str();
        descriptor.snapshot = context_provider_snapshot;
        descriptor.action_handler = context_provider_action;
        descriptor.user_data = &probe;
        REQUIRE(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_OK);
    }
    context_entity_provider_descriptor overflow{};
    overflow.struct_size = sizeof(overflow);
    overflow.provider_id_utf8 = local_ids.back().c_str();
    overflow.snapshot = context_provider_snapshot;
    overflow.action_handler = context_provider_action;
    overflow.user_data = &probe;
    CHECK(sao_plugins_ctx_register_entity_provider(context, &overflow) == SAO_ERR_INVALID_ARGUMENT);

    std::vector<std::string> qualified_ids;
    std::vector<const char*> unregister_ids;
    qualified_ids.reserve(local_ids.size());
    unregister_ids.reserve(local_ids.size());
    for (const auto& local_id : local_ids)
        qualified_ids.push_back("context_provider_budget/" + local_id);
    for (const auto& qualified_id : qualified_ids)
        unregister_ids.push_back(qualified_id.c_str());
    CHECK(plugin_context_unregister_entity_providers(
              context, unregister_ids.data(), unregister_ids.size()) == SAO_ERR_INVALID_ARGUMENT);

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.size() == 256);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("zero-row entity provider snapshot invokes its builder once",
          "[plugins][loader][entity-provider][snapshot][focused]") {
    TempDirectory temp(L"context_provider_zero_rows");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_zero_rows", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    probe.zero_rows = true;
    const auto adapter = context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.providers[0].rows.empty());
    REQUIRE(catalog.roots.size() == 1);
    CHECK(catalog.roots[0].actions.empty());
    CHECK(probe.snapshot_calls == 1);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("catalog revision change during consumer callback returns busy without committing",
          "[plugins][loader][entity-provider][snapshot][revision][focused]") {
    TempDirectory temp(L"context_provider_revision_change");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_revision_change", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    entity_provider_catalog_snapshot committed;
    committed.revision = 777;
    int32_t disable_status = SAO_ERR_OS_CALL_FAILED;
    CHECK(snapshot_entity_providers_while_disabling(handle, committed, disable_status) ==
          SAO_PLUGINS_ERR_BUSY);
    CHECK(disable_status == SAO_OK);
    CHECK(committed.revision == 777);
    CHECK(committed.providers.empty());
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("provider registered by a failing enable hook remains unpublished and unloads cleanly",
          "[plugins][loader][entity-provider][enable][rollback][focused]") {
    TempDirectory temp(L"context_provider_enable_rollback");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_enable_rollback", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = failing_enable_context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    CHECK(sao_plugins_lifecycle_enable(handle) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);

    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_entity_provider_invoke("context_provider_enable_rollback/tools-provider", 1,
                                             "run", "{}") != SAO_OK);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke("context_provider_enable_rollback/tools-provider", 1,
                                             "run", "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("provider registration at enable publication boundary cannot remain unpublished",
          "[plugins][loader][entity-provider][registration][enable][concurrency][focused]") {
    TempDirectory temp(L"context_provider_register_enable");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_register_enable", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = enable_registration_race_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    constexpr size_t kRegistrationCount = 128;
    std::vector<std::string> provider_ids;
    std::vector<context_entity_provider_descriptor> providers;
    std::vector<int32_t> statuses(kRegistrationCount, SAO_ERR_OS_CALL_FAILED);
    provider_ids.reserve(kRegistrationCount);
    providers.reserve(kRegistrationCount);
    for (size_t index = 0; index < kRegistrationCount; ++index) {
        provider_ids.push_back("enable-race-provider-" + std::to_string(index));
        context_entity_provider_descriptor provider{};
        provider.struct_size = sizeof(provider);
        provider.provider_id_utf8 = provider_ids.back().c_str();
        provider.snapshot = context_provider_snapshot;
        provider.action_handler = context_provider_action;
        provider.user_data = &probe;
        providers.push_back(provider);
    }

    std::jthread registration([&] {
        while (!probe.enable_entered.load())
            std::this_thread::yield();
        probe.registration_started.store(true);
        for (size_t index = 0; index < providers.size(); ++index) {
            statuses[index] = sao_plugins_ctx_register_entity_provider(context, &providers[index]);
        }
    });
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    registration.join();

    const auto successful =
        static_cast<size_t>(std::count(statuses.begin(), statuses.end(), SAO_OK));
    CHECK(std::all_of(statuses.begin(), statuses.end(), [](int32_t status) {
        return status == SAO_OK || status == SAO_PLUGINS_ERR_BUSY;
    }));
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.size() == successful);
    CHECK(catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("active provider registration serializes with disable without stale publication",
          "[plugins][loader][entity-provider][registration][disable][concurrency][focused]") {
    TempDirectory temp(L"context_provider_register_disable");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("context_provider_register_disable", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe probe;
    const auto adapter = empty_context_provider_adapter(&probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    entity_root_contribution_descriptor root{};
    root.struct_size = sizeof(root);
    root.contribution_id_utf8 = "race-root";
    root.root_id_utf8 = "plugin:context-provider-race";
    root.name_utf8 = "竞态工具";
    root.icon_utf8 = "R";
    context_entity_provider_descriptor provider{};
    provider.struct_size = sizeof(provider);
    provider.provider_id_utf8 = "race-provider";
    provider.snapshot = context_provider_snapshot;
    provider.action_handler = context_provider_action;
    provider.user_data = &probe;
    provider.root_contribution = &root;

    std::atomic_bool begin{false};
    std::atomic_int registration_status{SAO_ERR_OS_CALL_FAILED};
    std::atomic_int disable_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread registration([&] {
        while (!begin.load())
            std::this_thread::yield();
        registration_status.store(sao_plugins_ctx_register_entity_provider(context, &provider));
    });
    std::jthread disable([&] {
        while (!begin.load())
            std::this_thread::yield();
        disable_status.store(sao_plugins_lifecycle_disable(handle));
    });
    begin.store(true);
    registration.join();
    disable.join();

    CHECK(disable_status == SAO_OK);
    CHECK((registration_status == SAO_OK || registration_status == SAO_PLUGINS_ERR_BUSY));
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    root.contribution_id_utf8 = "unload-race-root";
    root.root_id_utf8 = "plugin:context-provider-unload-race";
    root.name_utf8 = "卸载竞态工具";
    provider.provider_id_utf8 = "unload-race-provider";
    begin.store(false);
    registration_status.store(SAO_ERR_OS_CALL_FAILED);
    std::atomic_int unload_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread unload_registration([&] {
        while (!begin.load())
            std::this_thread::yield();
        registration_status.store(sao_plugins_ctx_register_entity_provider(context, &provider));
    });
    std::jthread unload([&] {
        while (!begin.load())
            std::this_thread::yield();
        unload_status.store(sao_plugins_lifecycle_unload(handle));
    });
    begin.store(true);
    unload_registration.join();
    unload.join();

    CHECK(unload_status == SAO_OK);
    CHECK((registration_status == SAO_OK || registration_status == SAO_PLUGINS_ERR_BUSY ||
           registration_status == SAO_ERR_HANDLE_INVALID));
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::unloaded);
    CHECK(sao_plugins_entity_provider_invoke("context_provider_register_disable/race-provider", 1,
                                             "run", "{}") == SAO_ERR_HANDLE_INVALID);
    CHECK(
        sao_plugins_entity_provider_invoke("context_provider_register_disable/unload-race-provider",
                                           1, "run", "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
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
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::angelscript, &adapter) ==
            SAO_OK);
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
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    REQUIRE(snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel).empty());
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::angelscript) == SAO_OK);
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
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::lua, &adapter) == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
    REQUIRE(probe.on_unload_calls == 1);
    REQUIRE(probe.unload_calls == 0);

    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context == probe.load_context);
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) ==
            SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_registry_find(sao_plugins_registry_instance(),
                                      manifest.plugin_id.c_str()) == handle);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::lua) ==
            SAO_PLUGINS_ERR_BUSY);

    probe.allow_unload = true;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::unloaded);
    REQUIRE(probe.on_unload_calls == 2);
    REQUIRE(probe.unload_calls == 1);
    REQUIRE(probe.extension_visible_during_on_unload);
    REQUIRE(probe.extension_removed_before_adapter_unload);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::lua) == SAO_OK);
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

    const auto control_module =
        LoadLibraryExW(copied.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    REQUIRE(control_module != nullptr);
    using set_lifecycle_statuses_fn = int32_t(SAO_PLUGINS_CALL*)(int32_t, int32_t);
    const auto set_lifecycle_statuses = reinterpret_cast<set_lifecycle_statuses_fn>(
        GetProcAddress(control_module, "sao_test_plugin_set_lifecycle_statuses"));
    REQUIRE(set_lifecycle_statuses != nullptr);
    REQUIRE(set_lifecycle_statuses(SAO_ERR_OS_CALL_FAILED, SAO_ERR_OS_CALL_FAILED) == SAO_OK);

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
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) ==
            SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_registry_find(sao_plugins_registry_instance(),
                                      manifest.plugin_id.c_str()) == handle);

    REQUIRE(set_lifecycle_statuses(SAO_ERR_OS_CALL_FAILED, SAO_OK) == SAO_OK);
    REQUIRE(FreeLibrary(control_module) == TRUE);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::unloaded);
    REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    remove_plugin(handle);
}

TEST_CASE("host adapter register and unregister reject duplicate operations",
          "[plugins][loader][lifecycle][adapter-registry]") {
    adapter_probe first_probe;
    adapter_probe second_probe;
    const auto first = probe_adapter_vtable(&first_probe);
    const auto second = probe_adapter_vtable(&second_probe);

    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::csharp, &first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::csharp, &second) ==
            SAO_PLUGINS_ERR_ALREADY_EXISTS);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::csharp) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::csharp) ==
            SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::csharp, &second) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::csharp) == SAO_OK);
}

TEST_CASE("native DLL lifecycle validates ABI and capabilities then frees the module",
          "[plugins][loader][native]") {
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
    const auto provider =
        std::find_if(providers.providers.begin(), providers.providers.end(),
                     [](const auto& item) { return item.provider_id == "native_case/fixture"; });
    REQUIRE(provider != providers.providers.end());
    REQUIRE(provider->generation > 0);
    const uint64_t provider_generation = provider->generation;
    REQUIRE(provider->revision == 7);
    REQUIRE(provider->rows.size() == 1);
    CHECK(provider->rows[0].category_id == "fixture-category");
    CHECK(provider->rows[0].action_id == "fixture.action");
    CHECK(provider->rows[0].payload_json == R"({"source":"fixture"})");
    REQUIRE(sao_plugins_entity_provider_invoke("native_case/fixture", provider_generation,
                                               "fixture.action",
                                               R"({"source":"fixture"})") == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    providers = {};
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    CHECK(std::none_of(providers.providers.begin(), providers.providers.end(),
                       [](const auto& item) { return item.provider_id == "native_case/fixture"; }));
    CHECK(sao_plugins_entity_provider_invoke("native_case/fixture", provider_generation,
                                             "fixture.action", "{}") == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    providers = {};
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    const auto reenabled =
        std::find_if(providers.providers.begin(), providers.providers.end(),
                     [](const auto& item) { return item.provider_id == "native_case/fixture"; });
    REQUIRE(reenabled != providers.providers.end());
    CHECK(reenabled->generation == provider_generation);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(GetModuleHandleW(copied.c_str()) == nullptr);
    CHECK(sao_plugins_entity_provider_invoke("native_case/fixture", provider_generation,
                                             "fixture.action", "{}") == SAO_ERR_HANDLE_INVALID);
    providers = {};
    REQUIRE(snapshot_entity_providers(providers) == SAO_OK);
    CHECK(std::none_of(providers.providers.begin(), providers.providers.end(),
                       [](const auto& item) { return item.provider_id == "native_case/fixture"; }));
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
    write_zip(
        archive,
        {
            {"plugin.json",
             R"({"id":"install_case","name":"Installed","version":"1.2.3","entry":"plugin.emma","language":"emma"})"},
            {"plugin.emma", "entry"},
        });
    install_result result;
    REQUIRE(sao_plugins_install_archive(archive.c_str(), user_plugins.c_str(), false, &result) ==
            SAO_OK);
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
    REQUIRE(sao_plugins_install_archive(compressed.c_str(), user_plugins.c_str(), false, &result) ==
            SAO_PLUGINS_ERR_UNSUPPORTED);

    const auto traversal = temp.path / L"traversal.zip";
    write_zip(traversal, {{"../escaped.txt", "escape"}, {"plugin.json", "{}"}});
    REQUIRE(sao_plugins_install_archive(traversal.c_str(), user_plugins.c_str(), false, &result) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE_FALSE(fs::exists(temp.path / L"escaped.txt"));
}

TEST_CASE("isolation reports unsupported while retaining failure diagnostics",
          "[plugins][loader][isolation]") {
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
