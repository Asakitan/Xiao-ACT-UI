#include <catch2/catch_test_macros.hpp>

#include "entity_provider_internal.h"
#include "plugin_internal.h"
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

struct entity_provider_row_snapshot_v2 {
    uint32_t struct_size = 0;
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

struct entity_provider_snapshot_record_v2 {
    uint32_t struct_size = 0;
    uint32_t snapshot_abi_version = 0;
    std::string provider_id;
    std::string owner_plugin_id;
    uint64_t generation = 0;
    uint64_t revision = 0;
    entity_snapshot_content_token_t content_token = kInvalidEntitySnapshotContentToken;
    uint32_t row_stride_bytes = 0;
    std::vector<entity_provider_row_snapshot_v2> rows;
};

struct entity_root_action_snapshot_record_v2 {
    uint32_t struct_size = 0;
    std::string provider_id;
    std::string action_id;
};

struct entity_root_snapshot_record_v2 {
    uint32_t struct_size = 0;
    std::string owner_plugin_id;
    std::string contribution_id;
    std::string root_id;
    std::string name;
    std::string icon;
    double priority = 0.0;
    uint32_t action_stride_bytes = 0;
    std::vector<entity_root_action_snapshot_record_v2> actions;
};

struct entity_provider_catalog_snapshot_v2 {
    uint32_t struct_size = 0;
    uint32_t abi_version = 0;
    uint64_t revision = 0;
    entity_snapshot_content_token_t content_token = kInvalidEntitySnapshotContentToken;
    uint32_t provider_stride_bytes = 0;
    uint32_t root_contribution_stride_bytes = 0;
    std::vector<entity_provider_snapshot_record_v2> providers;
    std::vector<entity_root_snapshot_record_v2> roots;
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

template <typename T>
const T* stride_element(const void* base, uint32_t stride_bytes, uint32_t index) {
    const auto* bytes = static_cast<const std::byte*>(base);
    return reinterpret_cast<const T*>(bytes + static_cast<size_t>(index) * stride_bytes);
}

int32_t SAO_PLUGINS_CALL
copy_entity_provider_catalog_v2(const entity_provider_catalog_view_v2* catalog, void* user_data) {
    if (catalog == nullptr || user_data == nullptr ||
        catalog->struct_size < kEntityProviderCatalogViewV2RequiredPrefixSize ||
        catalog->abi_version != kEntitySnapshotAbiVersion2 ||
        catalog->content_token == kInvalidEntitySnapshotContentToken) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if ((catalog->provider_count > 0 && catalog->providers == nullptr) ||
        catalog->provider_stride_bytes < kEntityProviderViewV2RequiredPrefixSize ||
        catalog->provider_stride_bytes % alignof(entity_provider_view_v2) != 0 ||
        (catalog->root_contribution_count > 0 && catalog->root_contributions == nullptr) ||
        catalog->root_contribution_stride_bytes < kEntityRootContributionViewV2RequiredPrefixSize ||
        catalog->root_contribution_stride_bytes % alignof(entity_root_contribution_view_v2) != 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }

    auto& out = *static_cast<entity_provider_catalog_snapshot_v2*>(user_data);
    entity_provider_catalog_snapshot_v2 candidate;
    candidate.struct_size = catalog->struct_size;
    candidate.abi_version = catalog->abi_version;
    candidate.revision = catalog->revision;
    candidate.content_token = catalog->content_token;
    candidate.provider_stride_bytes = catalog->provider_stride_bytes;
    candidate.root_contribution_stride_bytes = catalog->root_contribution_stride_bytes;
    candidate.providers.reserve(catalog->provider_count);
    for (uint32_t provider_index = 0; provider_index < catalog->provider_count; ++provider_index) {
        const auto* provider = stride_element<entity_provider_view_v2>(
            catalog->providers, catalog->provider_stride_bytes, provider_index);
        if (provider->struct_size < kEntityProviderViewV2RequiredPrefixSize ||
            provider->struct_size > catalog->provider_stride_bytes ||
            provider->content_token == kInvalidEntitySnapshotContentToken ||
            (provider->row_count > 0 && provider->rows == nullptr) ||
            provider->row_stride_bytes < kEntityMenuRowV2RequiredPrefixSize ||
            provider->row_stride_bytes % alignof(entity_menu_row_v2) != 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        entity_provider_snapshot_record_v2 copied;
        copied.struct_size = provider->struct_size;
        copied.snapshot_abi_version = provider->snapshot_abi_version;
        copied.provider_id = provider->provider_id_utf8;
        copied.owner_plugin_id = provider->owner_plugin_id_utf8;
        copied.generation = provider->generation;
        copied.revision = provider->revision;
        copied.content_token = provider->content_token;
        copied.row_stride_bytes = provider->row_stride_bytes;
        copied.rows.reserve(provider->row_count);
        for (uint32_t row_index = 0; row_index < provider->row_count; ++row_index) {
            const auto* row = stride_element<entity_menu_row_v2>(
                provider->rows, provider->row_stride_bytes, row_index);
            if (row->struct_size < kEntityMenuRowV2RequiredPrefixSize ||
                row->struct_size > provider->row_stride_bytes) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            copied.rows.push_back({
                row->struct_size,
                row->category_id_utf8,
                row->category_label_utf8,
                row->category_icon_utf8,
                row->category_priority,
                row->row_label_utf8,
                row->row_icon_utf8,
                row->action_id_utf8,
                row->payload_json_utf8,
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
            root->struct_size > catalog->root_contribution_stride_bytes ||
            (root->action_count > 0 && root->actions == nullptr) ||
            root->action_stride_bytes < kEntityRootActionRefViewV2RequiredPrefixSize ||
            root->action_stride_bytes % alignof(entity_root_action_ref_view_v2) != 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        entity_root_snapshot_record_v2 copied;
        copied.struct_size = root->struct_size;
        copied.owner_plugin_id = root->owner_plugin_id_utf8;
        copied.contribution_id = root->contribution_id_utf8;
        copied.root_id = root->root_id_utf8;
        copied.name = root->name_utf8;
        copied.icon = root->icon_utf8;
        copied.priority = root->priority;
        copied.action_stride_bytes = root->action_stride_bytes;
        copied.actions.reserve(root->action_count);
        for (uint32_t action_index = 0; action_index < root->action_count; ++action_index) {
            const auto* action = stride_element<entity_root_action_ref_view_v2>(
                root->actions, root->action_stride_bytes, action_index);
            if (action->struct_size < kEntityRootActionRefViewV2RequiredPrefixSize ||
                action->struct_size > root->action_stride_bytes) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            copied.actions.push_back(
                {action->struct_size, action->provider_id_utf8, action->action_id_utf8});
        }
        candidate.roots.push_back(std::move(copied));
    }
    out = std::move(candidate);
    return SAO_OK;
}

int32_t snapshot_entity_providers_v2(entity_provider_catalog_snapshot_v2& out) {
    return sao_plugins_entity_provider_snapshot_v2(copy_entity_provider_catalog_v2, &out);
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

bool create_test_symlink(const fs::path& link, const fs::path& target, bool directory = false) {
    fs::create_directories(link.parent_path());
    constexpr DWORD kAllowUnprivilegedCreate = 0x2;
    const DWORD flags = (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) |
                        kAllowUnprivilegedCreate;
    return CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) != FALSE;
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

enum class entity_snapshot_v2_mode {
    stable,
    probe_ok,
    token_mismatch,
    stride_mismatch,
    probe_zero_token,
    fill_buffer_too_small,
    fill_non_retry_error,
    fill_zero_token,
    fill_zero_stride,
    short_stride,
    misaligned_stride,
    oversized_stride,
    short_row_struct,
    zero_rows,
    zero_rows_nonzero_stride,
};

struct alignas(entity_menu_row_v2) physical_entity_menu_row_v2 {
    entity_menu_row_v2 row{};
    std::array<uint64_t, 4> future_tail{};
};

static_assert(sizeof(physical_entity_menu_row_v2) > sizeof(entity_menu_row_v2));
static_assert(sizeof(physical_entity_menu_row_v2) % alignof(entity_menu_row_v2) == 0);

struct entity_provider_v2_probe {
    entity_snapshot_v2_mode mode = entity_snapshot_v2_mode::stable;
    std::atomic_int snapshot_calls{0};
    std::atomic_int actions{0};
    uint64_t revision = 0xd1501;
    entity_snapshot_content_token_t source_content_token = 0xd1502;
    uint32_t fill_input_stride = 0;
    uint32_t fill_count = 0;
    uint32_t fill_output_stride = 0;
    uint64_t fill_revision = 0;
    entity_snapshot_content_token_t fill_content_token = kInvalidEntitySnapshotContentToken;
    double category_priority = 15.25;
    std::string last_payload;
};

void set_v2_physical_row(physical_entity_menu_row_v2& row, uint32_t struct_size,
                         const char* category_id, const char* category_label,
                         double category_priority, const char* row_label, const char* action_id,
                         const char* payload_json, uint8_t can_activate, uint8_t keep_menu_open,
                         uint8_t close_menu_before) {
    row = {};
    row.row = {
        struct_size,
        category_id,
        category_label,
        "future-icon",
        category_priority,
        row_label,
        "row-icon",
        action_id,
        payload_json,
        can_activate,
        keep_menu_open,
        close_menu_before,
        {},
    };
    row.future_tail = {0xd1500001, 0xd1500002, 0xd1500003, 0xd1500004};
}

int32_t SAO_PLUGINS_CALL entity_provider_v2_snapshot(
    void* rows, uint32_t capacity, uint32_t row_stride_bytes, uint32_t* out_count,
    uint64_t* out_revision, entity_snapshot_content_token_t* out_content_token,
    uint32_t* out_row_stride_bytes, void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || out_content_token == nullptr ||
        out_row_stride_bytes == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& probe = *static_cast<entity_provider_v2_probe*>(user_data);
    ++probe.snapshot_calls;
    const bool is_probe = rows == nullptr && capacity == 0 && row_stride_bytes == 0;
    *out_revision = probe.revision;
    *out_content_token = probe.mode == entity_snapshot_v2_mode::probe_zero_token
                             ? kInvalidEntitySnapshotContentToken
                             : probe.source_content_token;
    if (probe.mode == entity_snapshot_v2_mode::zero_rows) {
        *out_count = 0;
        *out_row_stride_bytes = 0;
        return SAO_OK;
    }
    if (probe.mode == entity_snapshot_v2_mode::zero_rows_nonzero_stride) {
        *out_count = 0;
        *out_row_stride_bytes = sizeof(physical_entity_menu_row_v2);
        return SAO_OK;
    }

    *out_count = 2;
    switch (probe.mode) {
    case entity_snapshot_v2_mode::short_stride:
        *out_row_stride_bytes = static_cast<uint32_t>(kEntityMenuRowV2RequiredPrefixSize - 1);
        break;
    case entity_snapshot_v2_mode::misaligned_stride:
        *out_row_stride_bytes = sizeof(entity_menu_row_v2) + 1;
        break;
    case entity_snapshot_v2_mode::oversized_stride:
        *out_row_stride_bytes = 32U * 1024U * 1024U;
        break;
    default:
        *out_row_stride_bytes = sizeof(physical_entity_menu_row_v2);
        break;
    }
    if (is_probe)
        return probe.mode == entity_snapshot_v2_mode::probe_ok ? SAO_OK : SAO_ERR_BUFFER_TOO_SMALL;
    if (rows == nullptr || capacity < 2 || row_stride_bytes < sizeof(physical_entity_menu_row_v2)) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }

    probe.fill_input_stride = row_stride_bytes;
    if (probe.mode == entity_snapshot_v2_mode::token_mismatch)
        *out_content_token = probe.source_content_token + 1;
    if (probe.mode == entity_snapshot_v2_mode::stride_mismatch)
        *out_row_stride_bytes = row_stride_bytes + alignof(entity_menu_row_v2);
    if (probe.mode == entity_snapshot_v2_mode::fill_zero_token)
        *out_content_token = kInvalidEntitySnapshotContentToken;
    if (probe.mode == entity_snapshot_v2_mode::fill_zero_stride)
        *out_row_stride_bytes = 0;
    if (probe.mode == entity_snapshot_v2_mode::fill_non_retry_error) {
        *out_count = capacity + 1;
        *out_revision = probe.revision + 1;
        *out_content_token = kInvalidEntitySnapshotContentToken;
        *out_row_stride_bytes = 0;
    }
    probe.fill_count = *out_count;
    probe.fill_output_stride = *out_row_stride_bytes;
    probe.fill_revision = *out_revision;
    probe.fill_content_token = *out_content_token;
    if (probe.mode == entity_snapshot_v2_mode::fill_buffer_too_small)
        return SAO_ERR_BUFFER_TOO_SMALL;
    if (probe.mode == entity_snapshot_v2_mode::fill_non_retry_error)
        return SAO_ERR_OS_CALL_FAILED;

    physical_entity_menu_row_v2 first;
    physical_entity_menu_row_v2 second;
    const uint32_t row_struct_size =
        probe.mode == entity_snapshot_v2_mode::short_row_struct
            ? static_cast<uint32_t>(kEntityMenuRowV2RequiredPrefixSize - 1)
            : sizeof(physical_entity_menu_row_v2);
    set_v2_physical_row(first, row_struct_size, "v2-tools", "V2 Tools", probe.category_priority,
                        "First V2", "first", R"({"index":1})", 1, 1, 0);
    set_v2_physical_row(second, row_struct_size, "v2-tools", "V2 Tools", probe.category_priority,
                        "Second V2", "second", R"({"index":2})", 0, 0, 1);
    auto* raw_rows = static_cast<std::byte*>(rows);
    std::memcpy(raw_rows, &first, sizeof(first));
    std::memcpy(raw_rows + row_stride_bytes, &second, sizeof(second));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL entity_provider_v2_action(const char*, const char* payload_json,
                                                   void* user_data) {
    auto& probe = *static_cast<entity_provider_v2_probe*>(user_data);
    ++probe.actions;
    probe.last_payload = payload_json == nullptr ? "" : payload_json;
    return SAO_OK;
}

struct counting_v2_catalog_context {
    std::atomic_int calls{0};
    entity_provider_catalog_snapshot_v2 snapshot;
};

int32_t SAO_PLUGINS_CALL count_and_copy_entity_provider_catalog_v2(
    const entity_provider_catalog_view_v2* catalog, void* user_data) {
    auto& context = *static_cast<counting_v2_catalog_context*>(user_data);
    ++context.calls;
    return copy_entity_provider_catalog_v2(catalog, &context.snapshot);
}

struct v1_content_token_probe {
    std::atomic_int calls{0};
    bool content_b = false;
    uint64_t revision = 0xd1503;
};

int32_t SAO_PLUGINS_CALL v1_content_token_snapshot(entity_menu_row* rows, uint32_t capacity,
                                                   uint32_t* out_count, uint64_t* out_revision,
                                                   void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto& probe = *static_cast<v1_content_token_probe*>(user_data);
    ++probe.calls;
    *out_count = 1;
    *out_revision = probe.revision;
    if (rows == nullptr && capacity == 0)
        return SAO_ERR_BUFFER_TOO_SMALL;
    if (rows == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (capacity < 1)
        return SAO_ERR_BUFFER_TOO_SMALL;
    rows[0] = {
        sizeof(entity_menu_row),
        "v1-token",
        "V1 Token",
        "",
        1.0,
        probe.content_b ? "Content B" : "Content A",
        "",
        "replace",
        probe.content_b ? R"({"content":"B"})" : R"({"content":"A"})",
        1,
        0,
        0,
        {},
    };
    return SAO_OK;
}

struct same_revision_content_probe {
    std::atomic_int calls{0};
    uint64_t revision = 0xd14;
    entity_menu_row row{
        sizeof(entity_menu_row),
        "same-revision",
        "Same Revision",
        "",
        0.0,
        "Probe content A",
        "",
        "replace",
        R"({"content":"A"})",
        1,
        0,
        0,
        {},
    };
};

int32_t SAO_PLUGINS_CALL same_revision_content_snapshot(entity_menu_row* rows, uint32_t capacity,
                                                        uint32_t* out_count, uint64_t* out_revision,
                                                        void* user_data) {
    if (out_count == nullptr || out_revision == nullptr || user_data == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto& probe = *static_cast<same_revision_content_probe*>(user_data);
    ++probe.calls;
    *out_count = 1;
    *out_revision = probe.revision;
    if (rows == nullptr && capacity == 0) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (capacity < 1)
        return SAO_ERR_BUFFER_TOO_SMALL;
    if (rows == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    // Characterization of the v1 limitation: revision is metadata, not an immutable content
    // token, so the provider can replace content at fill time without changing revision.
    probe.row.row_label_utf8 = "Fill replacement B";
    probe.row.payload_json_utf8 = R"({"content":"B"})";
    rows[0] = probe.row;
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

struct event_unload_probe {
    plugin_handle_t plugin = nullptr;
    plugin_context_t* context = nullptr;
    std::atomic_int calls{0};
    int32_t unload_status = SAO_ERR_OS_CALL_FAILED;
    lifecycle_state state = lifecycle_state::unknown;
    bool should_stop = true;
};

void event_unload_callback(const char*, const char*, void* user_data) {
    auto& probe = *static_cast<event_unload_probe*>(user_data);
    ++probe.calls;
    probe.unload_status = sao_plugins_lifecycle_unload(probe.plugin);
    probe.state = sao_plugins_lifecycle_state(probe.plugin);
    probe.should_stop = sao_plugins_ctx_should_stop(probe.context);
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

TEST_CASE("manifest parses and validates normalized fields",
          "[plugins][loader][manifest][hardening]") {
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

TEST_CASE("manifest parse and load enforce bounded candidate transactions",
          "[plugins][loader][manifest][json][bounds][transaction][hardening][focused]") {
    const auto with_payload = [](std::string payload) {
        return std::string(
                   R"({"id":"manifest_budget","entry":"plugin.emma","language":"emma","payload":)") +
               std::move(payload) + "}";
    };

    plugin_manifest manifest;
    const std::string maximum_depth = with_payload(nested_array_json(63));
    const std::string excessive_depth = with_payload(nested_array_json(64));
    REQUIRE(sao_plugins_manifest_parse(maximum_depth.data(), maximum_depth.size(), &manifest) ==
            SAO_OK);
    CHECK(manifest.plugin_id == "manifest_budget");
    manifest = make_manifest("sentinel", fs::path{});
    CHECK(sao_plugins_manifest_parse(excessive_depth.data(), excessive_depth.size(), &manifest) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(manifest.plugin_id.empty());
    CHECK_FALSE(manifest.parse_error.empty());

    const std::string maximum_nodes = with_payload(flat_array_json(16379));
    const std::string excessive_nodes = with_payload(flat_array_json(16380));
    REQUIRE(sao_plugins_manifest_parse(maximum_nodes.data(), maximum_nodes.size(), &manifest) ==
            SAO_OK);
    manifest = make_manifest("sentinel", fs::path{});
    CHECK(sao_plugins_manifest_parse(excessive_nodes.data(), excessive_nodes.size(), &manifest) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(manifest.plugin_id.empty());
    CHECK_FALSE(manifest.parse_error.empty());

    const std::string maximum_string =
        with_payload("\"" + std::string(kMaximumManifestStringBytes, 'x') + "\"");
    const std::string excessive_string =
        with_payload("\"" + std::string(kMaximumManifestStringBytes + 1, 'x') + "\"");
    REQUIRE(sao_plugins_manifest_parse(maximum_string.data(), maximum_string.size(), &manifest) ==
            SAO_OK);
    manifest = make_manifest("sentinel", fs::path{});
    CHECK(sao_plugins_manifest_parse(excessive_string.data(), excessive_string.size(), &manifest) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(manifest.plugin_id.empty());
    CHECK_FALSE(manifest.parse_error.empty());

    const auto aggregate_strings = [&](size_t final_string_bytes) {
        std::string payload = "[";
        for (size_t index = 0; index < 7; ++index) {
            if (index != 0)
                payload += ',';
            payload += "\"" + std::string(kMaximumManifestStringBytes, 'x') + "\"";
        }
        payload += ",\"" + std::string(final_string_bytes, 'x') + "\"]";
        return with_payload(std::move(payload));
    };
    constexpr size_t kManifestFixedStringBytes = 52;
    constexpr size_t kFinalAggregateStringBytes =
        kMaximumManifestAggregateStringBytes -
        (7 * kMaximumManifestStringBytes) - kManifestFixedStringBytes;
    const std::string maximum_aggregate =
        aggregate_strings(kFinalAggregateStringBytes);
    const std::string excessive_aggregate =
        aggregate_strings(kFinalAggregateStringBytes + 1);
    REQUIRE(sao_plugins_manifest_parse(maximum_aggregate.data(), maximum_aggregate.size(),
                                       &manifest) == SAO_OK);
    manifest = make_manifest("sentinel", fs::path{});
    CHECK(sao_plugins_manifest_parse(excessive_aggregate.data(), excessive_aggregate.size(),
                                     &manifest) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(manifest.plugin_id.empty());
    CHECK_FALSE(manifest.parse_error.empty());

    std::string maximum_bytes =
        R"({"id":"manifest_raw","entry":"plugin.emma","language":"emma"})";
    REQUIRE(maximum_bytes.size() < kMaximumManifestRawBytes);
    maximum_bytes.append(kMaximumManifestRawBytes - maximum_bytes.size(), ' ');
    REQUIRE(sao_plugins_manifest_parse(maximum_bytes.data(), maximum_bytes.size(), &manifest) ==
            SAO_OK);
    CHECK(manifest.plugin_id == "manifest_raw");
    const std::string excessive_bytes = maximum_bytes + " ";
    manifest = make_manifest("sentinel", fs::path{});
    CHECK(sao_plugins_manifest_parse(excessive_bytes.data(), excessive_bytes.size(), &manifest) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(manifest.plugin_id.empty());
    CHECK_FALSE(manifest.parse_error.empty());

    TempDirectory temp(L"manifest_budget");
    write_text(temp.path / L"plugin.emma", "entry");
    write_text(temp.path / L"plugin.json", maximum_bytes);
    REQUIRE(sao_plugins_manifest_load_from_file((temp.path / L"plugin.json").c_str(),
                                                &manifest) == SAO_OK);
    CHECK(manifest.plugin_id == "manifest_raw");
    CHECK(fs::equivalent(fs::u8path(manifest.source_path), temp.path));

    write_text(temp.path / L"plugin.json", excessive_bytes);
    manifest = make_manifest("sentinel", fs::path{});
    CHECK(sao_plugins_manifest_load_from_file((temp.path / L"plugin.json").c_str(),
                                              &manifest) == SAO_ERR_INVALID_ARGUMENT);
    CHECK(manifest.plugin_id.empty());
    CHECK_FALSE(manifest.parse_error.empty());
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

TEST_CASE("scanner rejects entry and native reparse targets outside the plugin root",
          "[plugins][loader][scanner][containment][reparse][hardening][focused]") {
    TempDirectory temp(L"scanner_reparse");
    const auto outside_entry = temp.path / L"outside" / L"entry.emma";
    const auto outside_native = temp.path / L"outside" / L"native.dll";
    write_text(outside_entry, "entry");
    write_text(outside_native, "native");

    const auto script_plugin = temp.path / L"plugins" / L"script";
    write_text(script_plugin / L"plugin.json",
               R"({"id":"scanner_reparse_script","entry":"plugin.emma","language":"emma"})");
    if (create_test_symlink(script_plugin / L"plugin.emma", outside_entry)) {
        scanned_plugin scanned;
        CHECK(sao_plugins_scanner_refresh_one(script_plugin.c_str(), &scanned) ==
              SAO_ERR_HANDLE_INVALID);
    } else {
        WARN("file symlink creation unavailable; script reparse assertion skipped");
    }

    const auto native_plugin = temp.path / L"plugins" / L"native";
    write_text(native_plugin / L"plugin.json",
               R"({"id":"scanner_reparse_native","native_entry":"native.dll","native_abi":"sao_plugin_v2","abi_version":2})");
    if (create_test_symlink(native_plugin / L"native.dll", outside_native)) {
        scanned_plugin scanned;
        CHECK(sao_plugins_scanner_refresh_one(native_plugin.c_str(), &scanned) ==
              SAO_ERR_HANDLE_INVALID);
    } else {
        WARN("file symlink creation unavailable; native reparse assertion skipped");
    }
}

TEST_CASE("registry owns records and context implements settings and local events",
          "[plugins][loader][context][lifetime][hardening]") {
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

TEST_CASE("direct event callback unload is busy before lifecycle state mutation",
          "[plugins][loader][context][event][lifecycle][reentry][hardening][focused]") {
    TempDirectory temp(L"event_unload_reentry");
    write_text(temp.path / L"plugin.emma", "entry");
    auto manifest = make_manifest("event_unload_reentry", temp.path);
    manifest.enabled = true;
    auto handle = add_plugin(manifest);
    install_emma_adapter();
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_active);

    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);
    event_unload_probe probe{handle, context};
    uint32_t token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(context, "unload", event_unload_callback, &probe, &token) ==
            SAO_OK);
    REQUIRE(sao_plugins_ctx_emit(context, "unload", "{}") == SAO_OK);
    CHECK(probe.calls.load() == 1);
    CHECK(probe.unload_status == SAO_PLUGINS_ERR_BUSY);
    CHECK(probe.state == lifecycle_state::loaded_active);
    CHECK_FALSE(probe.should_stop);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_active);
    CHECK_FALSE(sao_plugins_ctx_should_stop(context));

    REQUIRE(sao_plugins_ctx_unsubscribe(context, token) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::emma) == SAO_OK);
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

TEST_CASE("dependency requirements reject path names and escaped package targets",
          "[plugins][loader][deps][requirements][containment][hardening][focused]") {
    TempDirectory temp(L"deps_containment");
    fs::create_directories(temp.path / L"libs");
    const std::vector<std::string> invalid_requirements = {
        ".",
        "..",
        "../escape",
        "nested/package",
        "nested\\package",
        "C:\\absolute",
        "/absolute",
        std::string(256, 'a'),
    };
    for (const auto& requirement : invalid_requirements) {
        INFO("requirement=" << requirement);
        write_text(temp.path / L"requirements.txt", requirement + "\n");
        deps_bootstrap_record record;
        CHECK(sao_plugins_deps_ensure(temp.path.c_str(), false, &record) ==
              SAO_ERR_INVALID_ARGUMENT);
        CHECK(record.added_paths.empty());
        CHECK(record.deps_summary.empty());
    }

    const auto outside = temp.path / L"outside" / L"escaped";
    fs::create_directories(outside);
    if (create_test_symlink(temp.path / L"libs" / L"escaped", outside, true)) {
        write_text(temp.path / L"requirements.txt", "escaped\n");
        deps_bootstrap_record record;
        CHECK(sao_plugins_deps_ensure(temp.path.c_str(), false, &record) ==
              SAO_ERR_INVALID_ARGUMENT);
        CHECK(record.added_paths.empty());
        CHECK(record.deps_summary.empty());
    } else {
        WARN("directory symlink creation unavailable; dependency reparse assertion skipped");
    }
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

TEST_CASE("entity provider same-revision content replacement characterizes v1 limitation",
          "[plugins][loader][entity-provider][same-revision][focused]") {
    TempDirectory temp(L"same_revision_content_replacement");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("same_revision_content_replacement", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    same_revision_content_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "same-revision-provider";
    descriptor.snapshot = same_revision_content_snapshot;
    descriptor.action_handler = snapshot_protocol_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    const uint64_t declared_catalog_revision =
        entity_provider_get_counters_for_testing().catalog_revision;

    entity_provider_catalog_snapshot catalog;
    REQUIRE(snapshot_entity_providers(catalog) == SAO_OK);
    CHECK(probe.calls == 2);
    CHECK(catalog.revision == declared_catalog_revision);
    REQUIRE(catalog.providers.size() == 1);
    const auto& provider = catalog.providers[0];
    CHECK(provider.provider_id == "same_revision_content_replacement/same-revision-provider");
    CHECK(provider.revision == probe.revision);
    REQUIRE(provider.rows.size() == 1);
    CHECK(provider.rows[0].category_id == "same-revision");
    CHECK(provider.rows[0].row_label == "Fill replacement B");
    CHECK(provider.rows[0].action_id == "replace");
    CHECK(provider.rows[0].payload_json == R"({"content":"B"})");

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity provider v1 and v2 ABI offsets remain frozen",
          "[plugins][loader][entity-provider][v1][abi][focused]") {
#if INTPTR_MAX == INT64_MAX
    STATIC_REQUIRE(alignof(native_entity_provider_descriptor) == 8);
    STATIC_REQUIRE(sizeof(native_entity_provider_descriptor) == 40);
    STATIC_REQUIRE(offsetof(native_entity_provider_descriptor, struct_size) == 0);
    STATIC_REQUIRE(offsetof(native_entity_provider_descriptor, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(native_entity_provider_descriptor, snapshot) == 16);
    STATIC_REQUIRE(offsetof(native_entity_provider_descriptor, action_handler) == 24);
    STATIC_REQUIRE(offsetof(native_entity_provider_descriptor, user_data) == 32);

    STATIC_REQUIRE(alignof(entity_root_contribution_descriptor) == 8);
    STATIC_REQUIRE(sizeof(entity_root_contribution_descriptor) == 48);
    STATIC_REQUIRE(offsetof(entity_root_contribution_descriptor, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_root_contribution_descriptor, contribution_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_root_contribution_descriptor, root_id_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_root_contribution_descriptor, name_utf8) == 24);
    STATIC_REQUIRE(offsetof(entity_root_contribution_descriptor, icon_utf8) == 32);
    STATIC_REQUIRE(offsetof(entity_root_contribution_descriptor, priority) == 40);

    STATIC_REQUIRE(alignof(context_entity_provider_descriptor) == 8);
    STATIC_REQUIRE(sizeof(context_entity_provider_descriptor) == 48);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor, struct_size) == 0);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor, snapshot) == 16);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor, action_handler) == 24);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor, user_data) == 32);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor, root_contribution) == 40);

    STATIC_REQUIRE(alignof(context_entity_provider_descriptor_v2) == 8);
    STATIC_REQUIRE(sizeof(context_entity_provider_descriptor_v2) == 48);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor_v2, struct_size) == 0);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor_v2, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor_v2, snapshot) == 16);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor_v2, action_handler) == 24);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor_v2, user_data) == 32);
    STATIC_REQUIRE(offsetof(context_entity_provider_descriptor_v2, root_contribution) == 40);

    STATIC_REQUIRE(alignof(entity_menu_row) == 8);
    STATIC_REQUIRE(sizeof(entity_menu_row) == 80);
    STATIC_REQUIRE(offsetof(entity_menu_row, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_menu_row, category_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_menu_row, category_label_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_menu_row, category_icon_utf8) == 24);
    STATIC_REQUIRE(offsetof(entity_menu_row, category_priority) == 32);
    STATIC_REQUIRE(offsetof(entity_menu_row, row_label_utf8) == 40);
    STATIC_REQUIRE(offsetof(entity_menu_row, row_icon_utf8) == 48);
    STATIC_REQUIRE(offsetof(entity_menu_row, action_id_utf8) == 56);
    STATIC_REQUIRE(offsetof(entity_menu_row, payload_json_utf8) == 64);
    STATIC_REQUIRE(offsetof(entity_menu_row, can_activate) == 72);
    STATIC_REQUIRE(offsetof(entity_menu_row, keep_menu_open) == 73);
    STATIC_REQUIRE(offsetof(entity_menu_row, close_menu_before) == 74);
    STATIC_REQUIRE(offsetof(entity_menu_row, reserved) == 75);

    STATIC_REQUIRE(alignof(entity_menu_row_v2) == 8);
    STATIC_REQUIRE(sizeof(entity_menu_row_v2) == 80);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_label_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_icon_utf8) == 24);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_priority) == 32);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, row_label_utf8) == 40);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, row_icon_utf8) == 48);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, action_id_utf8) == 56);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, payload_json_utf8) == 64);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, can_activate) == 72);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, keep_menu_open) == 73);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, close_menu_before) == 74);
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, reserved) == 75);
    STATIC_REQUIRE(sizeof(entity_menu_row_v2) == sizeof(entity_menu_row));
    STATIC_REQUIRE(alignof(entity_menu_row_v2) == alignof(entity_menu_row));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, struct_size) ==
                   offsetof(entity_menu_row, struct_size));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_id_utf8) ==
                   offsetof(entity_menu_row, category_id_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_label_utf8) ==
                   offsetof(entity_menu_row, category_label_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_icon_utf8) ==
                   offsetof(entity_menu_row, category_icon_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, category_priority) ==
                   offsetof(entity_menu_row, category_priority));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, row_label_utf8) ==
                   offsetof(entity_menu_row, row_label_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, row_icon_utf8) ==
                   offsetof(entity_menu_row, row_icon_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, action_id_utf8) ==
                   offsetof(entity_menu_row, action_id_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, payload_json_utf8) ==
                   offsetof(entity_menu_row, payload_json_utf8));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, can_activate) ==
                   offsetof(entity_menu_row, can_activate));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, keep_menu_open) ==
                   offsetof(entity_menu_row, keep_menu_open));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, close_menu_before) ==
                   offsetof(entity_menu_row, close_menu_before));
    STATIC_REQUIRE(offsetof(entity_menu_row_v2, reserved) == offsetof(entity_menu_row, reserved));

    STATIC_REQUIRE(alignof(entity_provider_view) == 8);
    STATIC_REQUIRE(sizeof(entity_provider_view) == 56);
    STATIC_REQUIRE(offsetof(entity_provider_view, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_provider_view, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_provider_view, owner_plugin_id_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_provider_view, generation) == 24);
    STATIC_REQUIRE(offsetof(entity_provider_view, revision) == 32);
    STATIC_REQUIRE(offsetof(entity_provider_view, row_count) == 40);
    STATIC_REQUIRE(offsetof(entity_provider_view, rows) == 48);

    STATIC_REQUIRE(alignof(entity_provider_view_v2) == 8);
    STATIC_REQUIRE(sizeof(entity_provider_view_v2) == 64);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, snapshot_abi_version) == 4);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, owner_plugin_id_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, generation) == 24);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, revision) == 32);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, content_token) == 40);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, row_count) == 48);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, row_stride_bytes) == 52);
    STATIC_REQUIRE(offsetof(entity_provider_view_v2, rows) == 56);

    STATIC_REQUIRE(alignof(entity_root_action_ref_view) == 8);
    STATIC_REQUIRE(sizeof(entity_root_action_ref_view) == 24);
    STATIC_REQUIRE(offsetof(entity_root_action_ref_view, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_root_action_ref_view, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_root_action_ref_view, action_id_utf8) == 16);

    STATIC_REQUIRE(alignof(entity_root_action_ref_view_v2) == 8);
    STATIC_REQUIRE(sizeof(entity_root_action_ref_view_v2) == 24);
    STATIC_REQUIRE(offsetof(entity_root_action_ref_view_v2, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_root_action_ref_view_v2, provider_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_root_action_ref_view_v2, action_id_utf8) == 16);

    STATIC_REQUIRE(alignof(entity_root_contribution_view) == 8);
    STATIC_REQUIRE(sizeof(entity_root_contribution_view) == 72);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, owner_plugin_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, contribution_id_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, root_id_utf8) == 24);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, name_utf8) == 32);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, icon_utf8) == 40);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, priority) == 48);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, action_count) == 56);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view, actions) == 64);

    STATIC_REQUIRE(alignof(entity_root_contribution_view_v2) == 8);
    STATIC_REQUIRE(sizeof(entity_root_contribution_view_v2) == 72);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, owner_plugin_id_utf8) == 8);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, contribution_id_utf8) == 16);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, root_id_utf8) == 24);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, name_utf8) == 32);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, icon_utf8) == 40);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, priority) == 48);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, action_count) == 56);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, action_stride_bytes) == 60);
    STATIC_REQUIRE(offsetof(entity_root_contribution_view_v2, actions) == 64);

    STATIC_REQUIRE(alignof(entity_provider_catalog_view) == 8);
    STATIC_REQUIRE(sizeof(entity_provider_catalog_view) == 48);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view, revision) == 8);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view, provider_count) == 16);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view, providers) == 24);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view, root_contribution_count) == 32);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view, root_contributions) == 40);

    STATIC_REQUIRE(alignof(entity_provider_catalog_view_v2) == 8);
    STATIC_REQUIRE(sizeof(entity_provider_catalog_view_v2) == 56);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, struct_size) == 0);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, abi_version) == 4);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, revision) == 8);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, content_token) == 16);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, provider_count) == 24);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, provider_stride_bytes) == 28);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, providers) == 32);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, root_contribution_count) == 40);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, root_contribution_stride_bytes) == 44);
    STATIC_REQUIRE(offsetof(entity_provider_catalog_view_v2, root_contributions) == 48);
#endif
    SUCCEED();
}

TEST_CASE("entity snapshot v2 copies future-tail producer rows and lifecycle roots",
          "[plugins][loader][entity-provider][v2][focused]") {
    entity_provider_counter_guard counter_guard;
    static_assert(kContextEntityProviderDescriptorV2RequiredPrefixSize == 40);
    static_assert(sizeof(context_entity_provider_descriptor_v2) == 48);
    static_assert(kEntityProviderCatalogViewV2RequiredPrefixSize == 56);
    static_assert(kEntityProviderViewV2RequiredPrefixSize == 64);
    static_assert(kEntityRootContributionViewV2RequiredPrefixSize == 72);
    static_assert(kEntityRootActionRefViewV2RequiredPrefixSize == 24);
    static_assert(kEntityMenuRowV2RequiredPrefixSize == 75);

    TempDirectory temp(L"entity_snapshot_v2_stable");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_stable", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    entity_provider_v2_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    entity_root_contribution_descriptor root{};
    root.struct_size = sizeof(root);
    root.contribution_id_utf8 = "v2-root";
    root.root_id_utf8 = "plugin:entity-snapshot-v2";
    root.name_utf8 = "V2 Root";
    root.icon_utf8 = "V2";
    root.priority = 15.25;
    context_entity_provider_descriptor_v2 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "future-stride";
    descriptor.snapshot = entity_provider_v2_snapshot;
    descriptor.action_handler = entity_provider_v2_action;
    descriptor.user_data = &probe;
    descriptor.root_contribution = &root;
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    entity_provider_catalog_snapshot_v2 catalog;
    REQUIRE(snapshot_entity_providers_v2(catalog) == SAO_OK);
    CHECK(probe.snapshot_calls == 2);
    CHECK(probe.fill_input_stride == sizeof(physical_entity_menu_row_v2));
    CHECK(probe.fill_output_stride == sizeof(physical_entity_menu_row_v2));
    CHECK(probe.fill_revision == probe.revision);
    CHECK(probe.fill_content_token == probe.source_content_token);
    CHECK(sizeof(physical_entity_menu_row_v2) > sizeof(entity_menu_row_v2));

    CHECK(catalog.struct_size == sizeof(entity_provider_catalog_view_v2));
    CHECK(catalog.abi_version == kEntitySnapshotAbiVersion2);
    CHECK(catalog.content_token != kInvalidEntitySnapshotContentToken);
    CHECK(catalog.provider_stride_bytes == sizeof(entity_provider_view_v2));
    CHECK(catalog.root_contribution_stride_bytes == sizeof(entity_root_contribution_view_v2));
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.roots.size() == 1);
    const auto& provider = catalog.providers[0];
    CHECK(provider.struct_size == sizeof(entity_provider_view_v2));
    CHECK(provider.snapshot_abi_version == kEntitySnapshotAbiVersion2);
    CHECK(provider.provider_id == "entity_snapshot_v2_stable/future-stride");
    CHECK(provider.owner_plugin_id == "entity_snapshot_v2_stable");
    CHECK(provider.generation != 0);
    CHECK(provider.revision == probe.revision);
    CHECK(provider.content_token != kInvalidEntitySnapshotContentToken);
    CHECK(provider.row_stride_bytes == sizeof(entity_menu_row_v2));
    REQUIRE(provider.rows.size() == 2);
    CHECK(provider.rows[0].struct_size == sizeof(entity_menu_row_v2));
    CHECK(provider.rows[0].category_id == "v2-tools");
    CHECK(provider.rows[0].category_label == "V2 Tools");
    CHECK(provider.rows[0].category_icon == "future-icon");
    CHECK(provider.rows[0].category_priority == 15.25);
    CHECK(provider.rows[0].row_label == "First V2");
    CHECK(provider.rows[0].row_icon == "row-icon");
    CHECK(provider.rows[0].action_id == "first");
    CHECK(provider.rows[0].payload_json == R"({"index":1})");
    CHECK(provider.rows[0].can_activate);
    CHECK(provider.rows[0].keep_menu_open);
    CHECK_FALSE(provider.rows[0].close_menu_before);
    CHECK(provider.rows[1].row_label == "Second V2");
    CHECK(provider.rows[1].action_id == "second");
    CHECK(provider.rows[1].payload_json == R"({"index":2})");
    CHECK_FALSE(provider.rows[1].can_activate);
    CHECK_FALSE(provider.rows[1].keep_menu_open);
    CHECK(provider.rows[1].close_menu_before);

    const auto& copied_root = catalog.roots[0];
    CHECK(copied_root.struct_size == sizeof(entity_root_contribution_view_v2));
    CHECK(copied_root.owner_plugin_id == "entity_snapshot_v2_stable");
    CHECK(copied_root.contribution_id == "v2-root");
    CHECK(copied_root.root_id == "plugin:entity-snapshot-v2");
    CHECK(copied_root.name == "V2 Root");
    CHECK(copied_root.icon == "V2");
    CHECK(copied_root.priority == 15.25);
    CHECK(copied_root.action_stride_bytes == sizeof(entity_root_action_ref_view_v2));
    REQUIRE(copied_root.actions.size() == 2);
    CHECK(copied_root.actions[0].struct_size == sizeof(entity_root_action_ref_view_v2));
    CHECK(copied_root.actions[0].provider_id == provider.provider_id);
    CHECK(copied_root.actions[0].action_id == "first");
    CHECK(copied_root.actions[1].provider_id == provider.provider_id);
    CHECK(copied_root.actions[1].action_id == "second");

    entity_provider_catalog_snapshot legacy_catalog;
    REQUIRE(snapshot_entity_providers(legacy_catalog) == SAO_OK);
    CHECK(probe.snapshot_calls == 4);
    REQUIRE(legacy_catalog.providers.size() == 1);
    REQUIRE(legacy_catalog.roots.size() == 1);
    CHECK(legacy_catalog.providers[0].provider_id == provider.provider_id);
    CHECK(legacy_catalog.providers[0].owner_plugin_id == provider.owner_plugin_id);
    CHECK(legacy_catalog.providers[0].generation == provider.generation);
    CHECK(legacy_catalog.providers[0].revision == provider.revision);
    REQUIRE(legacy_catalog.providers[0].rows.size() == 2);
    CHECK(legacy_catalog.providers[0].rows[0].row_label == "First V2");
    CHECK(legacy_catalog.providers[0].rows[1].row_label == "Second V2");
    REQUIRE(legacy_catalog.roots[0].actions.size() == 2);
    CHECK(legacy_catalog.roots[0].actions[0] ==
          std::pair{provider.provider_id, std::string("first")});
    CHECK(legacy_catalog.roots[0].actions[1] ==
          std::pair{provider.provider_id, std::string("second")});

    const auto provider_id = provider.provider_id;
    const uint64_t generation = provider.generation;
    const auto first_provider_content_token = provider.content_token;
    const auto first_catalog_content_token = catalog.content_token;
    ++probe.source_content_token;
    entity_provider_catalog_snapshot_v2 token_changed_catalog;
    REQUIRE(snapshot_entity_providers_v2(token_changed_catalog) == SAO_OK);
    CHECK(probe.snapshot_calls == 6);
    REQUIRE(token_changed_catalog.providers.size() == 1);
    CHECK(token_changed_catalog.revision == catalog.revision);
    CHECK(token_changed_catalog.providers[0].revision == provider.revision);
    CHECK(token_changed_catalog.providers[0].content_token == first_provider_content_token);
    CHECK(token_changed_catalog.content_token == first_catalog_content_token);
    CHECK(token_changed_catalog.providers[0].rows[0].row_label == "First V2");
    CHECK(token_changed_catalog.providers[0].rows[1].row_label == "Second V2");

    probe.category_priority = 16.25;
    entity_provider_catalog_snapshot_v2 row_changed_catalog;
    REQUIRE(snapshot_entity_providers_v2(row_changed_catalog) == SAO_OK);
    CHECK(probe.snapshot_calls == 8);
    REQUIRE(row_changed_catalog.providers.size() == 1);
    REQUIRE(row_changed_catalog.roots.size() == 1);
    CHECK(row_changed_catalog.revision == token_changed_catalog.revision);
    CHECK(row_changed_catalog.providers[0].revision == token_changed_catalog.providers[0].revision);
    CHECK(row_changed_catalog.providers[0].rows[0].category_priority == 16.25);
    CHECK(row_changed_catalog.providers[0].rows[1].category_priority == 16.25);
    CHECK(row_changed_catalog.providers[0].content_token !=
          token_changed_catalog.providers[0].content_token);
    CHECK(row_changed_catalog.content_token != token_changed_catalog.content_token);

    const char* provider_ids[] = {provider_id.c_str()};
    REQUIRE(plugin_context_unregister_entity_providers(context, provider_ids, 1) == SAO_OK);
    auto counters = entity_provider_get_counters_for_testing();
    counters.next_generation = generation;
    entity_provider_set_counters_for_testing(counters);
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);

    entity_provider_catalog_snapshot_v2 same_root_catalog;
    REQUIRE(snapshot_entity_providers_v2(same_root_catalog) == SAO_OK);
    CHECK(probe.snapshot_calls == 10);
    REQUIRE(same_root_catalog.providers.size() == 1);
    REQUIRE(same_root_catalog.roots.size() == 1);
    CHECK(same_root_catalog.revision != row_changed_catalog.revision);
    CHECK(same_root_catalog.providers[0].generation == generation);
    CHECK(same_root_catalog.providers[0].content_token ==
          row_changed_catalog.providers[0].content_token);
    CHECK(same_root_catalog.content_token == row_changed_catalog.content_token);

    REQUIRE(plugin_context_unregister_entity_providers(context, provider_ids, 1) == SAO_OK);
    counters = entity_provider_get_counters_for_testing();
    counters.next_generation = generation;
    entity_provider_set_counters_for_testing(counters);
    root.name_utf8 = "V2 Root Changed";
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);

    entity_provider_catalog_snapshot_v2 root_changed_catalog;
    REQUIRE(snapshot_entity_providers_v2(root_changed_catalog) == SAO_OK);
    CHECK(probe.snapshot_calls == 12);
    REQUIRE(root_changed_catalog.providers.size() == 1);
    REQUIRE(root_changed_catalog.roots.size() == 1);
    CHECK(root_changed_catalog.providers[0].generation == generation);
    CHECK(root_changed_catalog.providers[0].rows[0].category_priority == 16.25);
    CHECK(root_changed_catalog.roots[0].name == "V2 Root Changed");
    CHECK(root_changed_catalog.providers[0].content_token !=
          same_root_catalog.providers[0].content_token);
    CHECK(root_changed_catalog.content_token != same_root_catalog.content_token);

    REQUIRE(sao_plugins_entity_provider_invoke(provider_id.c_str(), generation, "first",
                                               R"({"from":"v2"})") == SAO_OK);
    CHECK(probe.actions == 1);
    CHECK(probe.last_payload == R"({"from":"v2"})");

    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_entity_providers_v2(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_entity_provider_invoke("entity_snapshot_v2_stable/future-stride", generation,
                                             "first", "{}") == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke("entity_snapshot_v2_stable/future-stride", generation,
                                             "first", "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity snapshot v2 accepts SAO_OK probes and canonical zero-row output",
          "[plugins][loader][entity-provider][v2][focused]") {
    TempDirectory temp(L"entity_snapshot_v2_positive_protocol");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_positive_protocol", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    entity_provider_v2_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor_v2 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "positive-protocol";
    descriptor.snapshot = entity_provider_v2_snapshot;
    descriptor.action_handler = entity_provider_v2_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    probe.mode = entity_snapshot_v2_mode::probe_ok;
    counting_v2_catalog_context probe_ok_context;
    REQUIRE(sao_plugins_entity_provider_snapshot_v2(count_and_copy_entity_provider_catalog_v2,
                                                    &probe_ok_context) == SAO_OK);
    CHECK(probe.snapshot_calls == 2);
    CHECK(probe_ok_context.calls == 1);
    REQUIRE(probe_ok_context.snapshot.providers.size() == 1);
    CHECK(probe_ok_context.snapshot.provider_stride_bytes == sizeof(entity_provider_view_v2));
    CHECK(probe_ok_context.snapshot.root_contribution_stride_bytes ==
          sizeof(entity_root_contribution_view_v2));
    CHECK(probe_ok_context.snapshot.providers[0].row_stride_bytes == sizeof(entity_menu_row_v2));
    CHECK(probe_ok_context.snapshot.providers[0].rows.size() == 2);

    probe.mode = entity_snapshot_v2_mode::zero_rows;
    probe.snapshot_calls = 0;
    probe.fill_input_stride = 0;
    probe.fill_count = 0;
    probe.fill_output_stride = 0;
    counting_v2_catalog_context zero_rows_context;
    REQUIRE(sao_plugins_entity_provider_snapshot_v2(count_and_copy_entity_provider_catalog_v2,
                                                    &zero_rows_context) == SAO_OK);
    CHECK(probe.snapshot_calls == 1);
    CHECK(probe.fill_input_stride == 0);
    CHECK(probe.fill_count == 0);
    CHECK(probe.fill_output_stride == 0);
    CHECK(zero_rows_context.calls == 1);
    REQUIRE(zero_rows_context.snapshot.providers.size() == 1);
    CHECK(zero_rows_context.snapshot.provider_stride_bytes == sizeof(entity_provider_view_v2));
    CHECK(zero_rows_context.snapshot.root_contribution_stride_bytes ==
          sizeof(entity_root_contribution_view_v2));
    CHECK(zero_rows_context.snapshot.providers[0].snapshot_abi_version ==
          kEntitySnapshotAbiVersion2);
    CHECK(zero_rows_context.snapshot.providers[0].content_token !=
          kInvalidEntitySnapshotContentToken);
    CHECK(zero_rows_context.snapshot.providers[0].row_stride_bytes == sizeof(entity_menu_row_v2));
    CHECK(zero_rows_context.snapshot.providers[0].rows.empty());
    CHECK(zero_rows_context.snapshot.roots.empty());

    probe.mode = entity_snapshot_v2_mode::stable;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity snapshot v2 retries fill protocol contradictions without publishing",
          "[plugins][loader][entity-provider][v2][focused]") {
    TempDirectory temp(L"entity_snapshot_v2_fill_retry");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_fill_retry", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    entity_provider_v2_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor_v2 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "fill-retry";
    descriptor.snapshot = entity_provider_v2_snapshot;
    descriptor.action_handler = entity_provider_v2_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    for (const auto mode :
         {entity_snapshot_v2_mode::fill_buffer_too_small, entity_snapshot_v2_mode::fill_zero_token,
          entity_snapshot_v2_mode::fill_zero_stride}) {
        INFO("mode=" << static_cast<int>(mode));
        probe.mode = mode;
        probe.snapshot_calls = 0;
        probe.fill_input_stride = 0;
        probe.fill_count = 0;
        probe.fill_output_stride = 0;
        probe.fill_revision = 0;
        probe.fill_content_token = kInvalidEntitySnapshotContentToken;
        counting_v2_catalog_context callback_context;
        callback_context.snapshot.revision = 0xd15d1;
        callback_context.snapshot.content_token = 0xd15d2;
        CHECK(sao_plugins_entity_provider_snapshot_v2(count_and_copy_entity_provider_catalog_v2,
                                                      &callback_context) == SAO_PLUGINS_ERR_BUSY);
        CHECK(probe.snapshot_calls == 18);
        CHECK(probe.fill_input_stride == sizeof(physical_entity_menu_row_v2));
        CHECK(probe.fill_count == 2);
        CHECK(probe.fill_revision == probe.revision);
        if (mode == entity_snapshot_v2_mode::fill_zero_token) {
            CHECK(probe.fill_content_token == kInvalidEntitySnapshotContentToken);
        } else {
            CHECK(probe.fill_content_token == probe.source_content_token);
        }
        if (mode == entity_snapshot_v2_mode::fill_zero_stride) {
            CHECK(probe.fill_output_stride == 0);
        } else {
            CHECK(probe.fill_output_stride == sizeof(physical_entity_menu_row_v2));
        }
        CHECK(callback_context.calls == 0);
        CHECK(callback_context.snapshot.revision == 0xd15d1);
        CHECK(callback_context.snapshot.content_token == 0xd15d2);
        CHECK(callback_context.snapshot.providers.empty());
    }

    probe.mode = entity_snapshot_v2_mode::stable;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity snapshot v2 propagates non-retry fill errors before output validation",
          "[plugins][loader][entity-provider][v2][focused]") {
    TempDirectory temp(L"entity_snapshot_v2_fill_error");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_fill_error", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    entity_provider_v2_probe probe;
    probe.mode = entity_snapshot_v2_mode::fill_non_retry_error;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor_v2 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "fill-error";
    descriptor.snapshot = entity_provider_v2_snapshot;
    descriptor.action_handler = entity_provider_v2_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    counting_v2_catalog_context callback_context;
    callback_context.snapshot.revision = 0xd15c1;
    callback_context.snapshot.content_token = 0xd15c2;
    CHECK(sao_plugins_entity_provider_snapshot_v2(count_and_copy_entity_provider_catalog_v2,
                                                  &callback_context) == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.snapshot_calls == 2);
    CHECK(probe.fill_input_stride == sizeof(physical_entity_menu_row_v2));
    CHECK(probe.fill_count == 3);
    CHECK(probe.fill_revision == probe.revision + 1);
    CHECK(probe.fill_content_token == kInvalidEntitySnapshotContentToken);
    CHECK(probe.fill_output_stride == 0);
    CHECK(callback_context.calls == 0);
    CHECK(callback_context.snapshot.revision == 0xd15c1);
    CHECK(callback_context.snapshot.content_token == 0xd15c2);
    CHECK(callback_context.snapshot.providers.empty());

    probe.mode = entity_snapshot_v2_mode::stable;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity snapshot v2 token and stride mismatches exhaust nested retry",
          "[plugins][loader][entity-provider][v2][focused]") {
    TempDirectory temp(L"entity_snapshot_v2_retry");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_retry", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    entity_provider_v2_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor_v2 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "retry";
    descriptor.snapshot = entity_provider_v2_snapshot;
    descriptor.action_handler = entity_provider_v2_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    for (const auto mode :
         {entity_snapshot_v2_mode::token_mismatch, entity_snapshot_v2_mode::stride_mismatch}) {
        INFO("mode=" << static_cast<int>(mode));
        probe.mode = mode;
        probe.snapshot_calls = 0;
        counting_v2_catalog_context callback_context;
        callback_context.snapshot.revision = 0xd15f1;
        callback_context.snapshot.content_token = 0xd15f2;
        CHECK(sao_plugins_entity_provider_snapshot_v2(count_and_copy_entity_provider_catalog_v2,
                                                      &callback_context) == SAO_PLUGINS_ERR_BUSY);
        CHECK(probe.snapshot_calls == 18);
        CHECK(callback_context.calls == 0);
        CHECK(callback_context.snapshot.revision == 0xd15f1);
        CHECK(callback_context.snapshot.content_token == 0xd15f2);
        CHECK(callback_context.snapshot.providers.empty());
    }

    probe.mode = entity_snapshot_v2_mode::stable;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity snapshot v2 rejects invalid token stride and row layouts atomically",
          "[plugins][loader][entity-provider][v2][focused]") {
    TempDirectory temp(L"entity_snapshot_v2_invalid");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_invalid", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    entity_provider_v2_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor_v2 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "invalid";
    descriptor.snapshot = entity_provider_v2_snapshot;
    descriptor.action_handler = entity_provider_v2_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider_v2(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    struct invalid_case {
        entity_snapshot_v2_mode mode;
        int32_t expected_status;
        int expected_calls;
    };
    const std::array cases{
        invalid_case{entity_snapshot_v2_mode::probe_zero_token, SAO_ERR_INVALID_ARGUMENT, 1},
        invalid_case{entity_snapshot_v2_mode::short_stride, SAO_PLUGINS_ERR_ABI_MISMATCH, 1},
        invalid_case{entity_snapshot_v2_mode::misaligned_stride, SAO_ERR_INVALID_ARGUMENT, 1},
        invalid_case{entity_snapshot_v2_mode::oversized_stride, SAO_ERR_INVALID_ARGUMENT, 1},
        invalid_case{entity_snapshot_v2_mode::short_row_struct, SAO_PLUGINS_ERR_ABI_MISMATCH, 2},
        invalid_case{entity_snapshot_v2_mode::zero_rows_nonzero_stride, SAO_ERR_INVALID_ARGUMENT,
                     1},
    };
    for (const auto& invalid : cases) {
        INFO("mode=" << static_cast<int>(invalid.mode));
        probe.mode = invalid.mode;
        probe.snapshot_calls = 0;
        counting_v2_catalog_context callback_context;
        callback_context.snapshot.revision = 0xd15e1;
        callback_context.snapshot.content_token = 0xd15e2;
        CHECK(sao_plugins_entity_provider_snapshot_v2(count_and_copy_entity_provider_catalog_v2,
                                                      &callback_context) ==
              invalid.expected_status);
        CHECK(probe.snapshot_calls == invalid.expected_calls);
        CHECK(callback_context.calls == 0);
        CHECK(callback_context.snapshot.revision == 0xd15e1);
        CHECK(callback_context.snapshot.content_token == 0xd15e2);
        CHECK(callback_context.snapshot.providers.empty());
    }

    probe.mode = entity_snapshot_v2_mode::stable;
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::python) == SAO_OK);
    remove_plugin(handle);
}

TEST_CASE("entity snapshot v2 maps v1 content changes to canonical tokens",
          "[plugins][loader][entity-provider][v2][focused]") {
    TempDirectory temp(L"entity_snapshot_v2_v1_token");
    write_text(temp.path / L"plugin.py", "entry");
    auto manifest = make_manifest("entity_snapshot_v2_v1_token", temp.path);
    manifest.entry = "plugin.py";
    manifest.language = engine_kind::python;
    manifest.enabled = false;
    auto handle = add_plugin(manifest);

    context_provider_probe adapter_probe;
    v1_content_token_probe probe;
    const auto adapter = empty_context_provider_adapter(&adapter_probe);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::python, &adapter) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    context_entity_provider_descriptor descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.provider_id_utf8 = "v1-source";
    descriptor.snapshot = v1_content_token_snapshot;
    descriptor.action_handler = snapshot_protocol_action;
    descriptor.user_data = &probe;
    REQUIRE(sao_plugins_ctx_register_entity_provider(context, &descriptor) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);

    entity_provider_catalog_snapshot legacy_a;
    REQUIRE(snapshot_entity_providers(legacy_a) == SAO_OK);
    REQUIRE(legacy_a.providers.size() == 1);
    REQUIRE(legacy_a.providers[0].rows.size() == 1);
    CHECK(legacy_a.providers[0].rows[0].row_label == "Content A");
    CHECK(legacy_a.providers[0].rows[0].payload_json == R"({"content":"A"})");

    entity_provider_catalog_snapshot_v2 v2_a;
    REQUIRE(snapshot_entity_providers_v2(v2_a) == SAO_OK);
    REQUIRE(v2_a.providers.size() == 1);
    REQUIRE(v2_a.providers[0].rows.size() == 1);
    CHECK(v2_a.providers[0].snapshot_abi_version == kEntitySnapshotAbiVersion1);
    CHECK(v2_a.providers[0].rows[0].row_label == "Content A");
    CHECK(v2_a.providers[0].content_token != kInvalidEntitySnapshotContentToken);
    CHECK(v2_a.content_token != kInvalidEntitySnapshotContentToken);

    probe.content_b = true;
    entity_provider_catalog_snapshot_v2 v2_b;
    REQUIRE(snapshot_entity_providers_v2(v2_b) == SAO_OK);
    REQUIRE(v2_b.providers.size() == 1);
    REQUIRE(v2_b.providers[0].rows.size() == 1);
    CHECK(v2_b.providers[0].snapshot_abi_version == kEntitySnapshotAbiVersion1);
    CHECK(v2_b.providers[0].rows[0].row_label == "Content B");
    CHECK(v2_b.providers[0].rows[0].payload_json == R"({"content":"B"})");
    CHECK(v2_b.revision == v2_a.revision);
    CHECK(v2_b.providers[0].provider_id == v2_a.providers[0].provider_id);
    CHECK(v2_b.providers[0].owner_plugin_id == v2_a.providers[0].owner_plugin_id);
    CHECK(v2_b.providers[0].generation == v2_a.providers[0].generation);
    CHECK(v2_b.providers[0].revision == v2_a.providers[0].revision);
    CHECK(v2_b.providers[0].content_token != v2_a.providers[0].content_token);
    CHECK(v2_b.content_token != v2_a.content_token);

    entity_provider_catalog_snapshot legacy_b;
    REQUIRE(snapshot_entity_providers(legacy_b) == SAO_OK);
    REQUIRE(legacy_b.providers.size() == 1);
    REQUIRE(legacy_b.providers[0].rows.size() == 1);
    CHECK(legacy_b.revision == legacy_a.revision);
    CHECK(legacy_b.providers[0].provider_id == legacy_a.providers[0].provider_id);
    CHECK(legacy_b.providers[0].owner_plugin_id == legacy_a.providers[0].owner_plugin_id);
    CHECK(legacy_b.providers[0].generation == legacy_a.providers[0].generation);
    CHECK(legacy_b.providers[0].revision == legacy_a.providers[0].revision);
    CHECK(legacy_b.providers[0].rows[0].row_label == "Content B");
    CHECK(legacy_b.providers[0].rows[0].payload_json == R"({"content":"B"})");

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

TEST_CASE("native lifecycle rejects a reparse entry escaping the plugin root",
          "[plugins][loader][lifecycle][native][containment][reparse][hardening][focused]") {
    TempDirectory temp(L"native_reparse");
    const fs::path fixture = SAO_TEST_NATIVE_PLUGIN_PATH;
    REQUIRE(fs::is_regular_file(fixture));
    if (!create_test_symlink(temp.path / L"native_fixture.dll", fixture)) {
        WARN("file symlink creation unavailable; native lifecycle reparse assertion skipped");
        return;
    }
    write_text(temp.path / L"plugin.emma", "entry");
    auto manifest = make_manifest("native_reparse_escape", temp.path);
    manifest.native_entry = "native_fixture.dll";
    manifest.native_abi = "sao_plugin_v2";
    manifest.capabilities.push_back({"native_test"});
    auto handle = add_plugin(manifest);

    CHECK(sao_plugins_lifecycle_load(handle) == SAO_ERR_HANDLE_INVALID);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
    remove_plugin(handle);
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
