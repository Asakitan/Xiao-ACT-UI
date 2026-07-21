#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/lua_host/lua_call.h"
#include "sao/plugins/lua_host/lua_host.h"
#include "sao/plugins/lua_host/lua_module_bridge.h"
#include "sao/plugins/lua_host/lua_sandbox.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" int lua_gc(lua_State* state, int what, ...);

using namespace sao::plugins::loader;
using namespace sao::plugins::lua_host;
namespace fs = std::filesystem;

namespace {

constexpr int kLuaGcCollect = 2;

struct temp_directory {
    fs::path path;

    explicit temp_directory(const wchar_t* label) {
        wchar_t base[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, base) > 0);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::path(base) /
               (std::wstring(L"sao_lua_adapter_") + label + L"_" + std::to_wstring(stamp));
        REQUIRE(fs::create_directories(path));
    }

    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string path_utf8(const fs::path& path) {
    const auto& wide = path.native();
    const int needed =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(needed > 0);
    std::string result(static_cast<size_t>(needed), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                static_cast<int>(wide.size()), result.data(), needed, nullptr,
                                nullptr) == needed);
    return result;
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(std::string id, const fs::path& source) {
    plugin_manifest manifest;
    manifest.plugin_id = std::move(id);
    manifest.name = manifest.plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.lua";
    manifest.language = engine_kind::lua;
    manifest.source_path = path_utf8(source);
    manifest.abi_version = 2;
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

struct adapter_registration {
    lua_loader_adapter_owner_t owner = nullptr;

    adapter_registration() {
        lua_host_config config{};
        config.max_instructions_per_run = 1000000u;
        REQUIRE(sao_plugins_luahost_register_loader_adapter(&config, &owner) == SAO_OK);
        REQUIRE(owner != nullptr);
    }

    ~adapter_registration() {
        if (owner != nullptr) {
            (void)sao_plugins_luahost_unregister_loader_adapter(owner);
        }
    }

    adapter_registration(const adapter_registration&) = delete;
    adapter_registration& operator=(const adapter_registration&) = delete;

    void unregister() {
        REQUIRE(sao_plugins_luahost_unregister_loader_adapter(owner) == SAO_OK);
        owner = nullptr;
    }
};

struct gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

struct lifecycle_unload_probe {
    plugin_handle_t plugin = nullptr;
    std::atomic_int status{SAO_ERR_OS_CALL_FAILED};
};

void gate_callback(const char*, const char*, void* user_data) {
    auto& value = *static_cast<gate*>(user_data);
    std::unique_lock lock(value.mutex);
    value.entered = true;
    value.condition.notify_all();
    value.condition.wait(lock, [&value] { return value.released; });
}

void reentrant_unload_callback(const char*, const char*, void* user_data) {
    auto& probe = *static_cast<lifecycle_unload_probe*>(user_data);
    probe.status.store(sao_plugins_lifecycle_unload(probe.plugin));
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

struct action_result_snapshot {
    int32_t callback_status = SAO_OK;
    std::uint32_t calls = 0;
    std::uint32_t struct_size = 0;
    std::uint32_t abi_version = 0;
    std::uint8_t handled = 0;
    bool has_result = false;
    std::string result_json;
};

int32_t SAO_PLUGINS_CALL capture_action_result(const entity_action_result_v2* result,
                                               void* user_data) {
    if (result == nullptr || user_data == nullptr ||
        result->struct_size < kEntityActionResultV2RequiredPrefixSize ||
        result->abi_version != kEntityActionAbiVersion2 || result->handled > 1) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    for (const std::uint8_t value : result->reserved) {
        if (value != 0)
            return SAO_ERR_INVALID_ARGUMENT;
    }
    auto& captured = *static_cast<action_result_snapshot*>(user_data);
    ++captured.calls;
    captured.struct_size = result->struct_size;
    captured.abi_version = result->abi_version;
    captured.handled = result->handled;
    captured.has_result = result->result_json_utf8 != nullptr;
    captured.result_json = captured.has_result ? result->result_json_utf8 : "";
    return captured.callback_status;
}

const menu_provider_snapshot* find_provider(const menu_catalog_snapshot& catalog,
                                            std::string_view provider_id) {
    const auto found = std::find_if(catalog.providers.begin(), catalog.providers.end(),
                                    [provider_id](const auto& provider) {
                                        return provider.provider_id == provider_id;
                                    });
    return found == catalog.providers.end() ? nullptr : &*found;
}

int32_t SAO_PLUGINS_CALL copy_menu_catalog(const entity_provider_catalog_view* view,
                                           void* user_data) {
    if (view == nullptr || user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    menu_catalog_snapshot candidate;
    candidate.providers.reserve(view->provider_count);
    for (std::uint32_t provider_index = 0; provider_index < view->provider_count;
         ++provider_index) {
        const auto& source = view->providers[provider_index];
        menu_provider_snapshot provider;
        provider.provider_id = source.provider_id_utf8;
        provider.owner_id = source.owner_plugin_id_utf8;
        provider.generation = source.generation;
        provider.revision = source.revision;
        provider.rows.reserve(source.row_count);
        for (std::uint32_t row_index = 0; row_index < source.row_count; ++row_index) {
            const auto& row = source.rows[row_index];
            provider.rows.push_back({
                row.row_label_utf8,
                row.row_icon_utf8,
                row.action_id_utf8,
                row.payload_json_utf8,
                row.can_activate != 0,
                row.keep_menu_open != 0,
                row.close_menu_before != 0,
            });
        }
        candidate.providers.push_back(std::move(provider));
    }
    candidate.roots.reserve(view->root_contribution_count);
    for (std::uint32_t root_index = 0; root_index < view->root_contribution_count; ++root_index) {
        const auto& source = view->root_contributions[root_index];
        menu_root_snapshot root;
        root.owner_id = source.owner_plugin_id_utf8;
        root.contribution_id = source.contribution_id_utf8;
        root.root_id = source.root_id_utf8;
        root.name = source.name_utf8;
        root.icon = source.icon_utf8;
        root.priority = source.priority;
        root.actions.reserve(source.action_count);
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

std::string call_hook(lua_loader_adapter_owner_t owner, plugin_handle_t plugin, const char* name,
                      const char* arguments = nullptr) {
    char* result = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(owner, plugin, name, arguments, &result) ==
            SAO_OK);
    REQUIRE(result != nullptr);
    std::string value(result);
    sao_plugins_luahost_free_string(result);
    return value;
}

std::string execute_host(lua_host_handle_t host, const char* source, int32_t& status) {
    char* result = nullptr;
    char* error = nullptr;
    status = sao_plugins_luahost_execute(host, source, std::strlen(source), &result, &error);
    std::string value = result == nullptr ? "" : result;
    sao_plugins_luahost_free_string(result);
    sao_plugins_luahost_free_string(error);
    return value;
}

struct platform_probe {
    struct hotkey_entry {
        hotkey_callback_fn callback = nullptr;
        void* user_data = nullptr;
        plugin_context_platform_token_t token = 0;
    } hotkey;
    struct timer_entry {
        timer_callback_fn callback = nullptr;
        void* user_data = nullptr;
        plugin_context_platform_token_t token = 0;
        bool one_shot = false;
    } interval, timeout;
    struct render_entry {
        render_hook_fn callback = nullptr;
        void* user_data = nullptr;
        plugin_context_platform_token_t token = 0;
    } render;
    std::mutex mutex;
    std::vector<std::string> calls;
    plugin_context_platform_token_t next_token = 1;
    int redraw_count = 0;
    bool fire_one_shot_during_registration = false;
    int32_t unregister_timer_status = SAO_OK;
};

void SAO_PLUGINS_CALL provider_retain(void*) {}
void SAO_PLUGINS_CALL provider_release(void*) {}

int32_t SAO_PLUGINS_CALL provider_create_session(void* user_data,
                                                 const plugin_context_platform_session_spec*,
                                                 plugin_context_platform_session_t* output) {
    if (user_data == nullptr || output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = user_data;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_quiesce_session(void*, plugin_context_platform_session_t) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_destroy_session(void*, plugin_context_platform_session_t) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_register_hotkey(void* user_data,
                                                  plugin_context_platform_session_t,
                                                  const char* hotkey_id, const char*, const char*,
                                                  hotkey_callback_fn callback,
                                                  void* callback_user_data,
                                                  plugin_context_platform_token_t* output) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    const auto token = probe.next_token++;
    probe.hotkey = {callback, callback_user_data, token};
    probe.calls.emplace_back(std::string("register_hotkey:") + hotkey_id);
    *output = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_unregister_hotkey(void* user_data,
                                                    plugin_context_platform_session_t,
                                                    plugin_context_platform_token_t token) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    probe.calls.emplace_back("unregister_hotkey:" + std::to_string(token));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_register_timer(void* user_data, plugin_context_platform_session_t,
                                                 double, bool one_shot, timer_callback_fn callback,
                                                 void* callback_user_data,
                                                 plugin_context_platform_token_t* output) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    const auto token = probe.next_token++;
    platform_probe::timer_entry entry{callback, callback_user_data, token, one_shot};
    if (one_shot) {
        probe.timeout = entry;
    } else {
        probe.interval = entry;
    }
    probe.calls.emplace_back(one_shot ? "register_timeout" : "register_interval");
    if (one_shot && probe.fire_one_shot_during_registration)
        callback(callback_user_data);
    *output = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_unregister_timer(void* user_data,
                                                   plugin_context_platform_session_t,
                                                   plugin_context_platform_token_t token) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    probe.calls.emplace_back("unregister_timer:" + std::to_string(token));
    return probe.unregister_timer_status;
}

int32_t SAO_PLUGINS_CALL provider_show_notify(void* user_data, plugin_context_platform_session_t,
                                              const char*, const char* message, double, const char*,
                                              plugin_context_platform_token_t* output) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    const auto token = probe.next_token++;
    probe.calls.emplace_back(std::string("notify:") + message);
    *output = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_dismiss_notify(void* user_data, plugin_context_platform_session_t,
                                                 plugin_context_platform_token_t token) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    probe.calls.emplace_back("dismiss_notify:" + std::to_string(token));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_register_render_hook(
    void* user_data, plugin_context_platform_session_t, const char* surface, float,
    render_hook_fn callback, void* callback_user_data, plugin_context_platform_token_t* output) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    const auto token = probe.next_token++;
    probe.render = {callback, callback_user_data, token};
    probe.calls.emplace_back(std::string("register_render:") + surface);
    *output = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_unregister_render_hook(void* user_data,
                                                         plugin_context_platform_session_t,
                                                         plugin_context_platform_token_t token) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    probe.calls.emplace_back("unregister_render:" + std::to_string(token));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_set_overlay(void* user_data, plugin_context_platform_session_t,
                                              const char* surface, const char*,
                                              plugin_context_platform_token_t* output) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    const auto token = probe.next_token++;
    probe.calls.emplace_back(std::string("set_overlay:") + surface);
    *output = token;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_clear_overlay(void* user_data, plugin_context_platform_session_t,
                                                plugin_context_platform_token_t token) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    probe.calls.emplace_back("clear_overlay:" + std::to_string(token));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_request_redraw(void* user_data, plugin_context_platform_session_t,
                                                 const char* surface, const char*) {
    auto& probe = *static_cast<platform_probe*>(user_data);
    std::lock_guard lock(probe.mutex);
    ++probe.redraw_count;
    probe.calls.emplace_back(std::string("redraw:") + surface);
    return SAO_OK;
}

struct platform_registration {
    explicit platform_registration(platform_probe& probe) {
        plugin_context_platform_provider provider{};
        provider.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
        provider.struct_size = sizeof(provider);
        provider.user_data = &probe;
        provider.retain = provider_retain;
        provider.release = provider_release;
        provider.create_session = provider_create_session;
        provider.quiesce_session = provider_quiesce_session;
        provider.destroy_session = provider_destroy_session;
        provider.register_hotkey = provider_register_hotkey;
        provider.unregister_hotkey = provider_unregister_hotkey;
        provider.register_timer = provider_register_timer;
        provider.unregister_timer = provider_unregister_timer;
        provider.show_notify = provider_show_notify;
        provider.dismiss_notify = provider_dismiss_notify;
        provider.register_render_hook = provider_register_render_hook;
        provider.unregister_render_hook = provider_unregister_render_hook;
        provider.set_overlay = provider_set_overlay;
        provider.clear_overlay = provider_clear_overlay;
        provider.request_redraw = provider_request_redraw;
        REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);
        active = true;
    }

    ~platform_registration() {
        if (active)
            (void)sao_plugins_ctx_unregister_platform_provider();
    }

    void unregister() {
        REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
        active = false;
    }

    bool active = false;
};

} // namespace

TEST_CASE("Lua file loader caches hooks and treats missing hooks as success",
          "[lua][call][file][registry]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"file_loader");
    write_text(temp.path / L"plugin.lua", R"lua(
function custom(a) return a * 3 end
)lua");
    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);
    lua_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), temp.path.c_str(),
                                            "plugin.lua", "lua_file_loader", nullptr,
                                            &plugin) == SAO_OK);
    REQUIRE(plugin != nullptr);
    CHECK(sao_plugins_luahost_has_hook(plugin, "custom"));
    CHECK_FALSE(sao_plugins_luahost_has_hook(plugin, "on_load"));
    CHECK(sao_plugins_luahost_call_on_load(plugin) == SAO_OK);
    char* result = nullptr;
    REQUIRE(sao_plugins_luahost_call_hook(plugin, "custom", "[4]", &result) == SAO_OK);
    REQUIRE(std::string(result) == "12");
    sao_plugins_luahost_free_string(result);
    result = reinterpret_cast<char*>(1);
    CHECK(sao_plugins_luahost_call_hook(plugin, "missing", nullptr, &result) == SAO_OK);
    CHECK(result == nullptr);
    REQUIRE(sao_plugins_luahost_unload_script(plugin) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua adapter runs lifecycle hooks and canonical ctx basics",
          "[lua][adapter][lifecycle][ctx]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"lifecycle");
    write_text(temp.path / L"plugin.lua", R"lua(
local loaded_value = 0
local observed = 0
local original_context = nil
local canonical_enable = false
local canonical_disable = false
function on_load(ctx)
    original_context = ctx
    ctx:set_defaults({rate = 3})
    ctx:set_setting("rate", 7)
    loaded_value = ctx:get_setting("rate", 0)
    ctx:register_ui_panel("lua_panel", {title = "Lua Panel"})
    ctx:subscribe("lua_tick", function(event)
        observed = event.payload.value
    end)
    ctx:emit("lua_tick", {value = 11})
    _G.ctx = {spoofed = true}
end
function on_enable(ctx) canonical_enable = ctx == original_context end
function on_disable(ctx) canonical_disable = ctx == original_context end
function canonical_probe()
    return canonical_enable and canonical_disable and _G.ctx ~= original_context
end
function probe(a, b)
    return {sum = a + b, loaded = loaded_value, observed = observed,
            io_type = type(io), require_type = type(require)}
end
function unsupported_probe()
    local ok = pcall(function() ctx:notify("title", "message", 1.0) end)
    return ok
end
function on_unload(ctx) return ctx ~= nil end
)lua");

    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_adapter_lifecycle", temp.path));
    const int32_t load_status = sao_plugins_lifecycle_load(handle);
    std::string load_error;
    if (load_status != SAO_OK) {
        char* raw_error = nullptr;
        if (sao_plugins_luahost_loader_adapter_get_last_error(adapter.owner, handle, &raw_error) ==
                SAO_OK &&
            raw_error != nullptr) {
            load_error = raw_error;
            sao_plugins_luahost_free_string(raw_error);
        }
    }
    INFO("Lua adapter load error: " << load_error);
    REQUIRE(load_status == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 1);
    CHECK(sao_plugins_luahost_unregister_loader_adapter(adapter.owner) == SAO_PLUGINS_ERR_BUSY);

    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);
    char* setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "rate", &setting) == SAO_OK);
    REQUIRE(std::string(setting) == "7");
    sao_plugins_ctx_free_string(setting);

    const auto panels =
        snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel);
    REQUIRE(std::any_of(panels.begin(), panels.end(), [](const auto& panel) {
        return panel.plugin_id == "lua_adapter_lifecycle" && panel.id == "lua_panel";
    }));

    char* result = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, handle, "probe", "[2,5]",
                                                         &result) == SAO_OK);
    REQUIRE(result != nullptr);
    const std::string result_text(result);
    sao_plugins_luahost_free_string(result);
    CHECK(result_text.find("\"sum\":7") != std::string::npos);
    CHECK(result_text.find("\"loaded\":7") != std::string::npos);
    CHECK(result_text.find("\"observed\":11") != std::string::npos);
    CHECK(result_text.find("\"io_type\":\"nil\"") != std::string::npos);
    CHECK(result_text.find("\"require_type\":\"nil\"") != std::string::npos);

    result = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, handle, "unsupported_probe",
                                                         nullptr, &result) == SAO_OK);
    REQUIRE(result != nullptr);
    CHECK(std::string(result) == "false");
    sao_plugins_luahost_free_string(result);

    result = reinterpret_cast<char*>(1);
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, handle, "missing_hook",
                                                         nullptr, &result) == SAO_OK);
    REQUIRE(result == nullptr);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    CHECK(call_hook(adapter.owner, handle, "canonical_probe") == "true");
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua context shim forwards callbacks and releases resources",
          "[lua][adapter][ctx][provider][callbacks]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    platform_probe probe;
    platform_registration provider(probe);
    adapter_registration adapter;
    temp_directory temp(L"context_callbacks");
    write_text(temp.path / L"plugin.lua", R"lua(
local hotkey_count = 0
local interval_count = 0
local timeout_count = 0
local interval_busy = false
local interval_token = nil

function on_load(context)
    ctx = context
    ctx:register_hotkey("toggle", function()
        hotkey_count = hotkey_count + 1
    end, "CTRL+F8", "Toggle")
    interval_token = ctx:set_interval(function()
        interval_count = interval_count + 1
        local ok, err = pcall(function() ctx:clear_timer(interval_token) end)
        interval_busy = (not ok) and string.find(err, "-1006", 1, true) ~= nil
    end, 60.0)
    ctx:set_timeout(function()
        timeout_count = timeout_count + 1
    end, 30.0)
    ctx:register_render_hook("hud", function(surface, payload)
        return {surface = surface, value = payload.value + 1}
    end, 2.0)
    ctx:notify("Lua", "notice", 2.0, "info")
    ctx:toast("toast")
    ctx:set_overlay("hud", {visible = true})
    ctx:request_redraw("hud", "load")
    ctx:register_engine("My-Engine", {answer = 42})
end

function capability_probe()
    local engine = ctx:get_engine("my_engine")
    return {hotkey = hotkey_count, interval = interval_count,
            timeout = timeout_count, busy = interval_busy,
            engine = engine and engine.answer or 0}
end
)lua");

    auto manifest = make_manifest("lua_context_callbacks", temp.path);
    manifest.permissions = {"hotkey", "engine_access"};
    auto handle = add_plugin(std::move(manifest));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);

    platform_probe::hotkey_entry hotkey;
    platform_probe::timer_entry interval;
    platform_probe::timer_entry timeout;
    platform_probe::render_entry render;
    {
        std::lock_guard lock(probe.mutex);
        hotkey = probe.hotkey;
        interval = probe.interval;
        timeout = probe.timeout;
        render = probe.render;
        CHECK(probe.redraw_count == 1);
    }
    REQUIRE(hotkey.callback != nullptr);
    REQUIRE(interval.callback != nullptr);
    REQUIRE(timeout.callback != nullptr);
    REQUIRE(render.callback != nullptr);
    hotkey.callback(hotkey.user_data);
    interval.callback(interval.user_data);
    timeout.callback(timeout.user_data);
    timeout.callback(timeout.user_data);

    char* render_output = nullptr;
    REQUIRE(render.callback("hud", "{\"value\":4}", &render_output, render.user_data) == SAO_OK);
    REQUIRE(render_output != nullptr);
    const std::string render_json(render_output);
    sao_plugins_ctx_free_string(render_output);
    CHECK(render_json.find("\"surface\":\"hud\"") != std::string::npos);
    CHECK(render_json.find("\"value\":5") != std::string::npos);

    const std::string state = call_hook(adapter.owner, handle, "capability_probe");
    CHECK(state.find("\"hotkey\":1") != std::string::npos);
    CHECK(state.find("\"interval\":1") != std::string::npos);
    CHECK(state.find("\"timeout\":1") != std::string::npos);
    CHECK(state.find("\"busy\":true") != std::string::npos);
    CHECK(state.find("\"engine\":42") != std::string::npos);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    remove_plugin(handle);

    temp_directory denied_temp(L"context_permissions");
    write_text(denied_temp.path / L"plugin.lua", R"lua(
local hotkey_denied = false
local engine_denied = false
function on_load(context)
    ctx = context
    hotkey_denied = not pcall(function()
        ctx:register_hotkey("denied", function() end, "CTRL+F9", "Denied")
    end)
    engine_denied = not pcall(function()
        ctx:register_engine("denied", {})
    end)
end
function permission_probe()
    return hotkey_denied and engine_denied
end
)lua");
    auto denied = add_plugin(make_manifest("lua_context_permissions", denied_temp.path));
    REQUIRE(sao_plugins_lifecycle_load(denied) == SAO_OK);
    CHECK(call_hook(adapter.owner, denied, "permission_probe") == "true");
    REQUIRE(sao_plugins_lifecycle_unload(denied) == SAO_OK);
    remove_plugin(denied);

    {
        std::lock_guard lock(probe.mutex);
        const auto overlay =
            std::find_if(probe.calls.begin(), probe.calls.end(),
                         [](const std::string& call) { return call.find("clear_overlay:") == 0; });
        const auto notification =
            std::find_if(probe.calls.begin(), probe.calls.end(),
                         [](const std::string& call) { return call.find("dismiss_notify:") == 0; });
        const auto render_release =
            std::find_if(probe.calls.begin(), probe.calls.end(), [](const std::string& call) {
                return call.find("unregister_render:") == 0;
            });
        const auto timer_release =
            std::find_if(probe.calls.begin(), probe.calls.end(), [](const std::string& call) {
                return call.find("unregister_timer:") == 0;
            });
        const auto hotkey_release =
            std::find_if(probe.calls.begin(), probe.calls.end(), [](const std::string& call) {
                return call.find("unregister_hotkey:") == 0;
            });
        REQUIRE(overlay != probe.calls.end());
        REQUIRE(notification != probe.calls.end());
        REQUIRE(render_release != probe.calls.end());
        REQUIRE(timer_release != probe.calls.end());
        REQUIRE(hotkey_release != probe.calls.end());
        CHECK(overlay < notification);
        CHECK(notification < render_release);
        CHECK(render_release < timer_release);
        CHECK(timer_release < hotkey_release);
    }
    adapter.unregister();
    provider.unregister();
}

TEST_CASE("Lua coroutine one-shot registration handles synchronous provider callbacks",
          "[lua][adapter][ctx][coroutine][one-shot]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    platform_probe probe;
    probe.fire_one_shot_during_registration = true;
    platform_registration provider(probe);
    adapter_registration adapter;
    temp_directory temp(L"coroutine_sync_timeout");
    write_text(temp.path / L"plugin.lua", R"lua(
local timeout_count = 0

function on_load(ctx)
    local worker = coroutine.create(function()
        ctx:set_timeout(function()
            timeout_count = timeout_count + 1
        end, 1.0)
    end)
    local ok, error_message = coroutine.resume(worker)
    if not ok then error(error_message) end
end

function timeout_probe()
    return timeout_count
end
)lua");

    auto handle = add_plugin(make_manifest("lua_coroutine_sync_timeout", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    CHECK(call_hook(adapter.owner, handle, "timeout_probe") == "1");
    platform_probe::timer_entry timeout;
    {
        std::lock_guard lock(probe.mutex);
        timeout = probe.timeout;
    }
    REQUIRE(timeout.callback != nullptr);
    timeout.callback(timeout.user_data);
    CHECK(call_hook(adapter.owner, handle, "timeout_probe") == "1");
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    remove_plugin(handle);

    {
        std::lock_guard lock(probe.mutex);
        CHECK(std::count(probe.calls.begin(), probe.calls.end(), "register_timeout") == 1);
    }
    adapter.unregister();
    provider.unregister();
}

TEST_CASE("Lua host destroy rolls back a busy ctx teardown",
          "[lua][adapter][ctx][destroy][busy][rollback]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    platform_probe probe;
    platform_registration provider(probe);
    adapter_registration adapter;
    temp_directory temp(L"destroy_busy_owner");
    write_text(temp.path / L"plugin.lua", "function on_load(ctx) end");
    auto handle = add_plugin(make_manifest("lua_destroy_busy_owner", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    REQUIRE(sao_plugins_luahost_register_ctx(state, context) == SAO_OK);
    int32_t status = SAO_OK;
    const std::string menu_extension = execute_host(host, R"lua(
timer_count = 0
menu_action_count = 0
ctx:set_interval(function() timer_count = timer_count + 1 end, 60.0)
local function build_menu()
    return {{label = "busy rollback", command = function()
        menu_action_count = menu_action_count + 1
    end}}
end
return ctx:register_menu_category("Busy rollback", "", build_menu, 2.0)
)lua",
                                                    status);
    REQUIRE(status == SAO_OK);
    REQUIRE_FALSE(menu_extension.empty());
    const std::string menu_provider_id = "lua_destroy_busy_owner/" + menu_extension;
    INFO("expected menu provider: " << menu_provider_id);
    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    const auto original_menu = std::find_if(catalog.providers.begin(), catalog.providers.end(),
                                            [&menu_provider_id](const auto& provider) {
                                                return provider.provider_id == menu_provider_id;
                                            });
    REQUIRE(original_menu != catalog.providers.end());
    const std::uint64_t original_generation = original_menu->generation;
    platform_probe::timer_entry interval;
    {
        std::lock_guard lock(probe.mutex);
        interval = probe.interval;
        probe.unregister_timer_status = SAO_PLUGINS_ERR_BUSY;
    }
    REQUIRE(interval.callback != nullptr);

    CHECK(sao_plugins_luahost_destroy(host) == SAO_PLUGINS_ERR_BUSY);
    CHECK(execute_host(host, "return type(ctx) .. ':' .. tostring(timer_count)", status) ==
          "userdata:0");
    REQUIRE(status == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    const auto restored_menu = std::find_if(catalog.providers.begin(), catalog.providers.end(),
                                            [&menu_provider_id](const auto& provider) {
                                                return provider.provider_id == menu_provider_id;
                                            });
    REQUIRE(restored_menu != catalog.providers.end());
    REQUIRE(restored_menu->generation != original_generation);
    REQUIRE(restored_menu->rows.size() == 1);
    CHECK(sao_plugins_entity_provider_invoke(menu_provider_id.c_str(), restored_menu->generation,
                                             restored_menu->rows[0].action_id.c_str(),
                                             "{}") == SAO_OK);
    CHECK(execute_host(host, "return menu_action_count", status) == "1");
    REQUIRE(status == SAO_OK);
    interval.callback(interval.user_data);
    CHECK(execute_host(host, "return timer_count", status) == "1");
    REQUIRE(status == SAO_OK);
    {
        std::lock_guard lock(probe.mutex);
        probe.unregister_timer_status = SAO_OK;
    }
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    remove_plugin(handle);
    adapter.unregister();
    provider.unregister();
}

TEST_CASE("Lua on_unload false vetoes until the plugin permits teardown",
          "[lua][adapter][lifecycle][veto]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"hook_veto");
    write_text(temp.path / L"plugin.lua", R"lua(
local permit = false
function allow_unload() permit = true end
function on_unload() return permit end
)lua");

    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_adapter_hook_veto", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    CHECK(sao_plugins_lifecycle_unload(handle) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, handle, "allow_unload",
                                                         nullptr, nullptr) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua adapter vetoes unload while a generic hook lease is active",
          "[lua][adapter][veto]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"veto");
    write_text(temp.path / L"plugin.lua", R"lua(
function hold()
    ctx:emit("lease_gate", {})
    return true
end
function on_unload() return true end
)lua");

    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_adapter_veto", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);

    gate value;
    uint32_t token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(context, "lease_gate", gate_callback, &value, &token) ==
            SAO_OK);
    std::atomic<int32_t> call_status{SAO_ERR_NOT_INITIALIZED};
    std::thread caller([&] {
        char* result = nullptr;
        call_status = sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, handle, "hold",
                                                                   nullptr, &result);
        sao_plugins_luahost_free_string(result);
    });
    bool entered = false;
    {
        std::unique_lock lock(value.mutex);
        entered = value.condition.wait_for(lock, std::chrono::seconds(5),
                                           [&value] { return value.entered; });
    }
    if (!entered) {
        {
            std::lock_guard lock(value.mutex);
            value.released = true;
        }
        value.condition.notify_all();
        caller.join();
        FAIL("generic hook did not acquire its active-call lease");
    }

    CHECK(sao_plugins_lifecycle_unload(handle) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_disabled);
    {
        std::lock_guard lock(value.mutex);
        value.released = true;
    }
    value.condition.notify_all();
    caller.join();
    REQUIRE(call_status.load() == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua adapter rolls back failed on_load and syntax failures", "[lua][adapter][rollback]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    adapter_registration adapter;

    temp_directory hook_failure(L"hook_failure");
    write_text(hook_failure.path / L"plugin.lua", R"lua(
function on_load(ctx)
    ctx:register_ui_panel("rollback_panel", {title = "Rollback"})
    error("on_load rollback marker")
end
)lua");
    auto failed = add_plugin(make_manifest("lua_adapter_hook_failure", hook_failure.path));
    REQUIRE(sao_plugins_lifecycle_load(failed) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(sao_plugins_lifecycle_state(failed) == lifecycle_state::failed);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    plugin_context_t* context = reinterpret_cast<plugin_context_t*>(1);
    REQUIRE(sao_plugins_lifecycle_get_context(failed, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    const auto panels =
        snapshot_extensions(sao_plugins_registry_instance(), extension_kind::ui_panel);
    CHECK(std::none_of(panels.begin(), panels.end(), [](const auto& panel) {
        return panel.plugin_id == "lua_adapter_hook_failure";
    }));
    char* error = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_get_last_error(adapter.owner, failed, &error) ==
            SAO_OK);
    REQUIRE(error != nullptr);
    CHECK(std::string(error).find("on_load rollback marker") != std::string::npos);
    sao_plugins_luahost_free_string(error);
    remove_plugin(failed);

    temp_directory syntax_failure(L"syntax_failure");
    write_text(syntax_failure.path / L"plugin.lua", "function on_load(");
    auto malformed = add_plugin(make_manifest("lua_adapter_syntax_failure", syntax_failure.path));
    REQUIRE(sao_plugins_lifecycle_load(malformed) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    remove_plugin(malformed);
    adapter.unregister();
}

TEST_CASE("Lua failed-load cleanup retains ownership until lifecycle retry",
          "[lua][adapter][rollback][retry]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    platform_probe probe;
    probe.unregister_timer_status = SAO_PLUGINS_ERR_BUSY;
    platform_registration provider(probe);
    adapter_registration adapter;
    temp_directory temp(L"load_cleanup_retry");
    write_text(temp.path / L"plugin.lua", R"lua(
ctx:set_interval(function() end, 60.0)
error("load cleanup retry marker")
)lua");

    auto handle = add_plugin(make_manifest("lua_load_cleanup_retry", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 1);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    {
        std::lock_guard lock(probe.mutex);
        probe.unregister_timer_status = SAO_OK;
    }
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    context = reinterpret_cast<plugin_context_t*>(1);
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);

    remove_plugin(handle);
    adapter.unregister();
    provider.unregister();
}

TEST_CASE("Lua adapter maps manifest permissions and rejects duplicates",
          "[lua][adapter][sandbox][registration]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    adapter_registration adapter;
    lua_loader_adapter_owner_t duplicate = reinterpret_cast<lua_loader_adapter_owner_t>(1);
    REQUIRE(sao_plugins_luahost_register_loader_adapter(nullptr, &duplicate) ==
            SAO_PLUGINS_ERR_ALREADY_EXISTS);
    REQUIRE(duplicate == nullptr);

    temp_directory temp(L"permissions");
    write_text(temp.path / L"plugin.lua", R"lua(
function on_load()
    assert(type(io) == "table")
    assert(type(require) == "function")
    assert(type(os) == "table")
    assert(type(os.execute) == "nil")
end
)lua");
    auto manifest = make_manifest("lua_adapter_permissions", temp.path);
    manifest.permissions = {"fs", "require"};
    auto handle = add_plugin(std::move(manifest));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    remove_plugin(handle);
    adapter.unregister();
}

TEST_CASE("Lua adapter gives every plugin an independent state", "[lua][adapter][isolation]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory first_dir(L"state_first");
    temp_directory second_dir(L"state_second");
    constexpr std::string_view source = R"lua(
local counter = 0
function bump() counter = counter + 1 return counter end
)lua";
    write_text(first_dir.path / L"plugin.lua", source);
    write_text(second_dir.path / L"plugin.lua", source);

    adapter_registration adapter;
    auto first = add_plugin(make_manifest("lua_adapter_state_first", first_dir.path));
    auto second = add_plugin(make_manifest("lua_adapter_state_second", second_dir.path));
    REQUIRE(sao_plugins_lifecycle_load(first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(second) == SAO_OK);

    char* value = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, first, "bump", nullptr,
                                                         &value) == SAO_OK);
    REQUIRE(std::string(value) == "1");
    sao_plugins_luahost_free_string(value);
    value = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, first, "bump", nullptr,
                                                         &value) == SAO_OK);
    REQUIRE(std::string(value) == "2");
    sao_plugins_luahost_free_string(value);
    value = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(adapter.owner, second, "bump", nullptr,
                                                         &value) == SAO_OK);
    REQUIRE(std::string(value) == "1");
    sao_plugins_luahost_free_string(value);

    REQUIRE(sao_plugins_lifecycle_unload(first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(second) == SAO_OK);
    adapter.unregister();
    remove_plugin(first);
    remove_plugin(second);
}

TEST_CASE("Lua action handler bridges Entity action-v2 results and replacement rundown",
          "[lua][adapter][entity-provider][action-v2][coroutine][replacement][reentry][repeat]["
          "focused]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"action_v2");
    write_text(temp.path / L"plugin.lua", R"lua(
local context = nil
local current_handler = 0
local calls = 0
local replacement_busy = false
local weak_handlers = setmetatable({}, {__mode = "v"})
local next_weak_handler = 0

local function track(handler)
    next_weak_handler = next_weak_handler + 1
    weak_handlers[next_weak_handler] = handler
    return handler
end

local function make_handler(identity)
    return track(function(action_id, payload)
        current_handler = identity
        calls = calls + 1
        if action_id == "decline" then return nil end
        if action_id == "echo" then return payload end
        if action_id == "scalar" then return payload + 1 end
        if action_id == "nested" then
            return {
                handler = identity,
                nested = {
                    payload = payload,
                    values = {true, payload.null_value},
                },
            }
        end
        if action_id == "bad_function" then return function() end end
        if action_id == "bad_thread" then return coroutine.create(function() end) end
        if action_id == "bad_userdata" then return context end
        if action_id == "nan" then return math.huge - math.huge end
        if action_id == "positive_inf" then return math.huge end
        if action_id == "negative_inf" then return -math.huge end
        if action_id == "cyclic" then
            local value = {}
            value.self = value
            return value
        end
        if action_id == "error" then error("Lua action fixture failure") end
        if action_id == "replace_busy" then
            local ok, error_message = pcall(function()
                context:register_action_handler(make_handler(identity + 100))
            end)
            replacement_busy = (not ok) and
                string.find(error_message, "-1006", 1, true) ~= nil
            return {handler = identity, busy = replacement_busy}
        end
        if action_id == "reentrant_unload" then
            context:emit("lua_action_reentrant_unload", {})
        end
        return {handler = identity, action = action_id, payload = payload}
    end)
end

function on_load(ctx)
    context = ctx
    local worker = coroutine.create(function()
        assert(ctx:register_action_handler(make_handler(1)) == "lua_action_adapter")
    end)
    local ok, error_message = coroutine.resume(worker)
    if not ok then error(error_message) end
    assert(ctx:register_action_handler(make_handler(2)) == "lua_action_adapter")
end

function replace_handler(identity)
    return context:register_action_handler(make_handler(identity))
end

function handler_stats()
    local live_handlers = 0
    for _, handler in pairs(weak_handlers) do
        if handler ~= nil then live_handlers = live_handlers + 1 end
    end
    return {
        current_handler = current_handler,
        calls = calls,
        replacement_busy = replacement_busy,
        live_handlers = live_handlers,
    }
end
)lua");

    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_action_adapter", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);

    const std::string provider_id = "lua_action_adapter/action-handler";
    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.roots.empty());
    const auto* provider = find_provider(catalog, provider_id);
    REQUIRE(provider != nullptr);
    CHECK(provider->rows.empty());
    const std::uint64_t generation = provider->generation;

    action_result_snapshot captured;
    const auto invoke = [&](const char* action_id, const char* payload) {
        captured = {};
        return sao_plugins_entity_provider_invoke_v2(provider_id.c_str(), generation, action_id,
                                                     payload, capture_action_result, &captured);
    };

    REQUIRE(invoke("echo", R"({"name":"雪","items":[1,null,true]})") == SAO_OK);
    CHECK(captured.calls == 1);
    CHECK(captured.struct_size == sizeof(entity_action_result_v2));
    CHECK(captured.abi_version == kEntityActionAbiVersion2);
    CHECK(captured.handled == 1);
    CHECK(captured.has_result);
    CHECK(captured.result_json == R"({"items":[1,null,true],"name":"雪"})");

    REQUIRE(invoke("scalar", "41") == SAO_OK);
    CHECK(captured.handled == 1);
    CHECK(captured.result_json == "42");

    REQUIRE(invoke("echo", "null") == SAO_OK);
    CHECK(captured.handled == 1);
    CHECK(captured.result_json == "null");

    REQUIRE(invoke("nested", R"({"value":7,"null_value":null})") == SAO_OK);
    CHECK(captured.result_json ==
          R"({"handler":2,"nested":{"payload":{"null_value":null,"value":7},"values":[true,null]}})");

    REQUIRE(invoke("decline", "{}") == SAO_OK);
    CHECK(captured.calls == 1);
    CHECK(captured.handled == 0);
    CHECK_FALSE(captured.has_result);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), generation, "decline", "{}") ==
          SAO_PLUGINS_ERR_NOT_FOUND);

    for (const char* action_id : {"bad_function", "bad_thread", "bad_userdata", "nan",
                                  "positive_inf", "negative_inf", "cyclic"}) {
        INFO("action_id=" << action_id);
        CHECK(invoke(action_id, "{}") == SAO_ERR_INVALID_ARGUMENT);
        CHECK(captured.calls == 0);
    }
    CHECK(invoke("error", "{}") == SAO_ERR_OS_CALL_FAILED);
    CHECK(captured.calls == 0);

    captured = {};
    captured.callback_status = SAO_ERR_BUFFER_TOO_SMALL;
    CHECK(sao_plugins_entity_provider_invoke_v2(
              provider_id.c_str(), generation, "echo", "true", capture_action_result,
              &captured) == SAO_ERR_BUFFER_TOO_SMALL);
    CHECK(captured.calls == 1);
    CHECK(captured.result_json == "true");

    CHECK(call_hook(adapter.owner, handle, "replace_handler", "[3]") ==
          "\"lua_action_adapter\"");
    REQUIRE(invoke("normal", R"({"value":9})") == SAO_OK);
    CHECK(captured.result_json ==
          R"({"action":"normal","handler":3,"payload":{"value":9}})");
    REQUIRE(invoke("replace_busy", "{}") == SAO_OK);
    CHECK(captured.result_json == R"({"busy":true,"handler":3})");
    REQUIRE(invoke("normal", "{}") == SAO_OK);
    CHECK(captured.result_json.find("\"handler\":3") != std::string::npos);
    const std::string replacement_stats = call_hook(adapter.owner, handle, "handler_stats");
    CHECK(replacement_stats.find("\"replacement_busy\":true") != std::string::npos);

    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    lifecycle_unload_probe unload_probe;
    unload_probe.plugin = handle;
    std::uint32_t unload_token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(context, "lua_action_reentrant_unload",
                                      reentrant_unload_callback, &unload_probe,
                                      &unload_token) == SAO_OK);
    REQUIRE(invoke("reentrant_unload", "{}") == SAO_OK);
    CHECK(unload_probe.status.load() == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_active);
    REQUIRE(sao_plugins_ctx_unsubscribe(context, unload_token) == SAO_OK);

    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    captured = {};
    CHECK(sao_plugins_entity_provider_invoke_v2(provider_id.c_str(), generation, "normal", "{}",
                                                capture_action_result,
                                                &captured) == SAO_PLUGINS_ERR_BUSY);
    CHECK(captured.calls == 0);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    provider = find_provider(catalog, provider_id);
    REQUIRE(provider != nullptr);
    CHECK(provider->generation == generation);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke_v2(provider_id.c_str(), generation, "normal", "{}",
                                                capture_action_result,
                                                &captured) == SAO_ERR_HANDLE_INVALID);

    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    provider = find_provider(catalog, provider_id);
    REQUIRE(provider != nullptr);
    const std::uint64_t reloaded_generation = provider->generation;
    CHECK(reloaded_generation != generation);
    captured = {};
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                provider_id.c_str(), reloaded_generation, "normal", "{}", capture_action_result,
                &captured) == SAO_OK);
    CHECK(captured.result_json.find("\"handler\":2") != std::string::npos);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua action handler scopes roll back failed enables and clean enable-only providers",
          "[lua][adapter][entity-provider][action-v2][lifecycle][rollback][registry][focused]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    adapter_registration adapter;

    temp_directory scoped_temp(L"action_v2_scopes");
    write_text(scoped_temp.path / L"plugin.lua", R"lua(
local context = nil
local enable_attempt = 0
local weak_handlers = setmetatable({}, {__mode = "v"})
local weak_index = 0

local function handler(identity)
    local value = function(action_id, payload)
        return {handler = identity, action = action_id, payload = payload}
    end
    weak_index = weak_index + 1
    weak_handlers[weak_index] = value
    return value
end

function on_load(ctx)
    context = ctx
    ctx:register_action_handler(handler(10))
end

function on_enable()
    enable_attempt = enable_attempt + 1
    if enable_attempt == 1 then
        context:register_action_handler(handler(20))
        error("action enable rollback fixture")
    end
    if enable_attempt == 3 then
        context:register_action_handler(handler(30))
    end
end

function scope_stats()
    local live_handlers = 0
    for _, value in pairs(weak_handlers) do
        if value ~= nil then live_handlers = live_handlers + 1 end
    end
    return {attempt = enable_attempt, live_handlers = live_handlers}
end
)lua");

    auto scoped = add_plugin(make_manifest("lua_action_scopes", scoped_temp.path));
    REQUIRE(sao_plugins_lifecycle_load(scoped) == SAO_OK);
    CHECK(sao_plugins_lifecycle_enable(scoped) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(scoped) == lifecycle_state::loaded_disabled);
        CHECK(call_hook(adapter.owner, scoped, "scope_stats").find("\"attempt\":1") !=
            std::string::npos);

    const std::string scoped_provider_id = "lua_action_scopes/action-handler";
    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_enable(scoped) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    const auto* provider = find_provider(catalog, scoped_provider_id);
    REQUIRE(provider != nullptr);
    CHECK(provider->rows.empty());
    const std::uint64_t persistent_generation = provider->generation;
    action_result_snapshot captured;
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                scoped_provider_id.c_str(), persistent_generation, "persistent", "{}",
                capture_action_result, &captured) == SAO_OK);
    CHECK(captured.result_json.find("\"handler\":10") != std::string::npos);

    REQUIRE(sao_plugins_lifecycle_disable(scoped) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(scoped) == SAO_OK);
    captured = {};
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                scoped_provider_id.c_str(), persistent_generation, "enable", "{}",
                capture_action_result, &captured) == SAO_OK);
    CHECK(captured.result_json.find("\"handler\":30") != std::string::npos);
    REQUIRE(sao_plugins_lifecycle_disable(scoped) == SAO_OK);
        CHECK(call_hook(adapter.owner, scoped, "scope_stats").find("\"attempt\":3") !=
            std::string::npos);
    REQUIRE(sao_plugins_lifecycle_enable(scoped) == SAO_OK);
    captured = {};
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                scoped_provider_id.c_str(), persistent_generation, "restored", "{}",
                capture_action_result, &captured) == SAO_OK);
    CHECK(captured.result_json.find("\"handler\":10") != std::string::npos);
    REQUIRE(sao_plugins_lifecycle_unload(scoped) == SAO_OK);
    remove_plugin(scoped);

    temp_directory enable_only_temp(L"action_v2_enable_only");
    write_text(enable_only_temp.path / L"plugin.lua", R"lua(
local context = nil
local generation = 0
local weak_handler = setmetatable({}, {__mode = "v"})

function on_load(ctx) context = ctx end

function on_enable()
    generation = generation + 1
    local identity = generation
    local handler = function()
        return {handler = identity}
    end
    weak_handler[1] = handler
    context:register_action_handler(handler)
end

function enable_only_stats()
    return {generation = generation, live_handler = weak_handler[1] ~= nil}
end
)lua");

    auto enable_only =
        add_plugin(make_manifest("lua_action_enable_only", enable_only_temp.path));
    REQUIRE(sao_plugins_lifecycle_load(enable_only) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(enable_only) == SAO_OK);
    const std::string enable_only_provider_id = "lua_action_enable_only/action-handler";
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    provider = find_provider(catalog, enable_only_provider_id);
    REQUIRE(provider != nullptr);
    const std::uint64_t first_generation = provider->generation;
    captured = {};
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                enable_only_provider_id.c_str(), first_generation, "first", "{}",
                capture_action_result, &captured) == SAO_OK);
    CHECK(captured.result_json == R"({"handler":1})");

    REQUIRE(sao_plugins_lifecycle_disable(enable_only) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(find_provider(catalog, enable_only_provider_id) == nullptr);
        CHECK(call_hook(adapter.owner, enable_only, "enable_only_stats").find("\"generation\":1") !=
            std::string::npos);
    captured = {};
    CHECK(sao_plugins_entity_provider_invoke_v2(
              enable_only_provider_id.c_str(), first_generation, "stale", "{}",
              capture_action_result, &captured) == SAO_ERR_HANDLE_INVALID);

    REQUIRE(sao_plugins_lifecycle_enable(enable_only) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    provider = find_provider(catalog, enable_only_provider_id);
    REQUIRE(provider != nullptr);
    CHECK(provider->generation != first_generation);
    captured = {};
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                enable_only_provider_id.c_str(), provider->generation, "second", "{}",
                capture_action_result, &captured) == SAO_OK);
    CHECK(captured.result_json == R"({"handler":2})");
    REQUIRE(sao_plugins_lifecycle_unload(enable_only) == SAO_OK);
    remove_plugin(enable_only);
    adapter.unregister();
}

TEST_CASE("Lua action handler registry refs are released on replacement and direct unload",
          "[lua][entity-provider][action-v2][registry-cleanup][direct_unload][focused]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory owner_temp(L"action_registry_owner");
    write_text(owner_temp.path / L"plugin.lua", "function on_load(ctx) end");
    adapter_registration adapter;
    auto owner = add_plugin(make_manifest("lua_action_registry", owner_temp.path));
    REQUIRE(sao_plugins_lifecycle_load(owner) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(owner) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(owner, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    temp_directory direct_temp(L"action_registry_direct");
    write_text(direct_temp.path / L"plugin.lua", R"lua(
weak_handlers = setmetatable({}, {__mode = "v"})

local function make_handler(identity)
    local handler = function()
        return {handler = identity}
    end
    weak_handlers[identity] = handler
    return handler
end

ctx:register_action_handler(make_handler(1))

function replace_handler()
    return ctx:register_action_handler(make_handler(2))
end

function weak_state()
    return {
        first = weak_handlers[1] ~= nil,
        second = weak_handlers[2] ~= nil,
    }
end
)lua");

    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    lua_plugin_handle_t direct_plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(state, direct_temp.path.c_str(), "plugin.lua",
                                            "lua_action_registry", context,
                                            &direct_plugin) == SAO_OK);
    REQUIRE(direct_plugin != nullptr);

    const std::string provider_id = "lua_action_registry/action-handler";
    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    const auto* provider = find_provider(catalog, provider_id);
    REQUIRE(provider != nullptr);
    CHECK(provider->rows.empty());
    const std::uint64_t generation = provider->generation;

    char* hook_result = nullptr;
    REQUIRE(sao_plugins_luahost_call_hook(direct_plugin, "replace_handler", nullptr,
                                          &hook_result) == SAO_OK);
    REQUIRE(hook_result != nullptr);
    CHECK(std::string(hook_result) == "\"lua_action_registry\"");
    sao_plugins_luahost_free_string(hook_result);
    REQUIRE(lua_gc(state, kLuaGcCollect) == 0);
    hook_result = nullptr;
    REQUIRE(sao_plugins_luahost_call_hook(direct_plugin, "weak_state", nullptr,
                                          &hook_result) == SAO_OK);
    REQUIRE(hook_result != nullptr);
    CHECK(std::string(hook_result) == R"({"first":false,"second":true})");
    sao_plugins_luahost_free_string(hook_result);

    action_result_snapshot captured;
    REQUIRE(sao_plugins_entity_provider_invoke_v2(
                provider_id.c_str(), generation, "current", "{}", capture_action_result,
                &captured) == SAO_OK);
    CHECK(captured.result_json == R"({"handler":2})");

    REQUIRE(sao_plugins_luahost_unload_script(direct_plugin) == SAO_OK);
    REQUIRE(lua_gc(state, kLuaGcCollect) == 0);
    int32_t status = SAO_OK;
    CHECK(execute_host(host,
                       "return weak_handlers[1] == nil and weak_handlers[2] == nil",
                       status) == "true");
    REQUIRE(status == SAO_OK);
    captured = {};
    CHECK(sao_plugins_entity_provider_invoke_v2(
              provider_id.c_str(), generation, "stale", "{}", capture_action_result,
              &captured) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(owner) == SAO_OK);
    adapter.unregister();
    remove_plugin(owner);
}

TEST_CASE("Lua menu category adapter preserves semantic identity and lifecycle",
          "[lua][adapter][menu][entity_provider]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"menu_category");
    write_text(temp.path / L"plugin.lua", R"lua(
local mode = 0
local build_calls = 0
local action_calls = 0
local action_fail = false
local reentrant_unload = false
local duplicate_rejected = false
local extension_id = ""
local context = nil

local function shared_command()
    if action_fail then error("menu action fixture failure") end
    if reentrant_unload then
        context:emit("lua_menu_reentrant_unload", {})
    end
    action_calls = action_calls + 1
end

local function build_menu()
    build_calls = build_calls + 1
    if mode == 3 then error("menu builder fixture failure") end
    if mode == 4 then return {} end
    local explicit = {
        action_id = "explicit", label = "显式", icon = "界",
        command = shared_command, payload = {value = "雪"},
        keep_menu_open = true,
    }
    local shared_a = {
        label = "共享 A", icon = "A", command = shared_command,
        payload = {slot = "a"}, close_menu_before = true,
    }
    local duplicate_a = {
        label = "共享 A", icon = "A", command = shared_command,
        payload = {slot = "a"}, close_menu_before = true,
    }
    local shared_b = {
        label = "共享 B", icon = "B", command = shared_command,
        payload = {slot = "b"}, can_activate = false,
    }
    local inserted = {
        label = "插入项", icon = "I", command = shared_command,
        payload = {slot = "inserted"},
    }
    if mode == 0 then return {explicit, shared_a, duplicate_a, shared_b} end
    if mode == 1 then
        return {inserted, explicit, shared_a, duplicate_a, shared_b}
    end
    return {shared_b, explicit, shared_a, duplicate_a}
end

function on_load(ctx)
    context = ctx
    extension_id = ctx:register_menu_category(
        "工具 α", "⚙", build_menu, 10.5)
    duplicate_rejected = not pcall(function()
        ctx:register_menu_category("工具 α", "⚙", build_menu, 10.5)
    end)
end

function set_mode(value) mode = value end
function set_action_fail(value) action_fail = value end
function set_reentrant_unload(value) reentrant_unload = value end
function action_call_count() return action_calls end
function stats()
    return {
        build_calls = build_calls,
        action_calls = action_calls,
        duplicate_rejected = duplicate_rejected,
        extension_id = extension_id,
    }
end
)lua");

    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_menu_adapter", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);

    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());

    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    std::atomic_int worker_snapshot_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread snapshot_worker([&] { worker_snapshot_status = snapshot_menu_catalog(catalog); });
    snapshot_worker.join();
    REQUIRE(worker_snapshot_status.load() == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.roots.size() == 1);
    const auto initial = catalog;
    const auto& provider = initial.providers[0];
    const auto& root = initial.roots[0];
    CHECK(provider.owner_id == "lua_menu_adapter");
    CHECK(provider.provider_id.rfind("lua_menu_adapter/menu-", 0) == 0);
    CHECK(provider.generation > 0);
    CHECK(provider.revision == 1);
    REQUIRE(provider.rows.size() == 4);
    CHECK(provider.rows[0].label == "显式");
    CHECK(provider.rows[0].icon == "界");
    CHECK(provider.rows[0].payload.find("雪") != std::string::npos);
    CHECK(provider.rows[0].can_activate);
    CHECK(provider.rows[0].keep_menu_open);
    CHECK_FALSE(provider.rows[0].close_menu_before);
    CHECK(provider.rows[1].close_menu_before);
    CHECK_FALSE(provider.rows[3].can_activate);
    CHECK(root.owner_id == "lua_menu_adapter");
    CHECK(root.contribution_id.rfind("menu-", 0) == 0);
    CHECK(root.root_id.rfind("plugin:", 0) == 0);
    CHECK(root.name == "工具 α");
    CHECK(root.icon == "⚙");
    CHECK(root.priority == 10.5);
    REQUIRE(root.actions.size() == 4);

    const std::string explicit_action = provider.rows[0].action_id;
    const std::string shared_a0 = provider.rows[1].action_id;
    const std::string shared_a1 = provider.rows[2].action_id;
    const std::string shared_b = provider.rows[3].action_id;
    CHECK(explicit_action.rfind("menu-action-", 0) == 0);
    CHECK(shared_a0 != shared_a1);
    CHECK(shared_a0 != shared_b);
    CHECK(shared_a1 != shared_b);
    const std::string stats_initial = call_hook(adapter.owner, handle, "stats");
    CHECK(stats_initial.find("\"build_calls\":1") != std::string::npos);
    CHECK(stats_initial.find("\"duplicate_rejected\":true") != std::string::npos);
    CHECK(stats_initial.find("\"extension_id\":\"menu-") != std::string::npos);

    std::atomic_int worker_action_status{SAO_ERR_OS_CALL_FAILED};
    std::jthread action_worker([&] {
        worker_action_status = sao_plugins_entity_provider_invoke(
            provider.provider_id.c_str(), provider.generation, explicit_action.c_str(),
            provider.rows[0].payload.c_str());
    });
    action_worker.join();
    CHECK(worker_action_status.load() == SAO_OK);
    CHECK(call_hook(adapter.owner, handle, "stats").find("\"action_calls\":1") !=
          std::string::npos);

    REQUIRE(call_hook(adapter.owner, handle, "set_action_fail", "[true]") == "null");
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             "{}") == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(call_hook(adapter.owner, handle, "set_action_fail", "[false]") == "null");
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             "menu-action-0000000000000000",
                                             "{}") == SAO_ERR_HANDLE_INVALID);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[1]") == "null");
    menu_catalog_snapshot inserted;
    REQUIRE(snapshot_menu_catalog(inserted) == SAO_OK);
    REQUIRE(inserted.providers.size() == 1);
    REQUIRE(inserted.providers[0].rows.size() == 5);
    CHECK(inserted.providers[0].revision == 2);
    CHECK(inserted.providers[0].rows[1].action_id == explicit_action);
    CHECK(inserted.providers[0].rows[2].action_id == shared_a0);
    CHECK(inserted.providers[0].rows[3].action_id == shared_a1);
    CHECK(inserted.providers[0].rows[4].action_id == shared_b);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[2]") == "null");
    menu_catalog_snapshot reordered;
    REQUIRE(snapshot_menu_catalog(reordered) == SAO_OK);
    REQUIRE(reordered.providers.size() == 1);
    REQUIRE(reordered.providers[0].rows.size() == 4);
    CHECK(reordered.providers[0].revision == 3);
    CHECK(reordered.providers[0].rows[0].action_id == shared_b);
    CHECK(reordered.providers[0].rows[1].action_id == explicit_action);
    CHECK(reordered.providers[0].rows[2].action_id == shared_a0);
    CHECK(reordered.providers[0].rows[3].action_id == shared_a1);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[3]") == "null");
    const auto retained = reordered;
    CHECK(snapshot_menu_catalog(reordered) == SAO_ERR_OS_CALL_FAILED);
    CHECK(reordered == retained);
    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[2]") == "null");
    REQUIRE(snapshot_menu_catalog(reordered) == SAO_OK);
    CHECK(reordered.providers[0].revision == 3);

    const std::string before_empty = call_hook(adapter.owner, handle, "stats");
    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[4]") == "null");
    menu_catalog_snapshot empty;
    REQUIRE(snapshot_menu_catalog(empty) == SAO_OK);
    REQUIRE(empty.providers.size() == 1);
    CHECK(empty.providers[0].rows.empty());
    REQUIRE(empty.roots.size() == 1);
    CHECK(empty.roots[0].actions.empty());
    const std::string after_empty = call_hook(adapter.owner, handle, "stats");
    CHECK(before_empty != after_empty);
    CHECK(after_empty.find("\"build_calls\":6") != std::string::npos);
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(), "{}") == SAO_OK);
    CHECK(call_hook(adapter.owner, handle, "stats").find("\"action_calls\":2") !=
          std::string::npos);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[0]") == "null");
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.providers[0].rows.size() == 4);
    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             "{}") == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    CHECK(catalog.providers[0].generation == provider.generation);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);
    lifecycle_unload_probe unload_probe;
    unload_probe.plugin = handle;
    std::uint32_t unload_token = 0;
    REQUIRE(sao_plugins_ctx_subscribe(context, "lua_menu_reentrant_unload",
                                      reentrant_unload_callback, &unload_probe,
                                      &unload_token) == SAO_OK);
    REQUIRE(call_hook(adapter.owner, handle, "set_reentrant_unload", "[true]") == "null");
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(), "{}") == SAO_OK);
    CHECK(unload_probe.status.load() == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::loaded_active);
    menu_catalog_snapshot after_reentrant_busy;
    REQUIRE(snapshot_menu_catalog(after_reentrant_busy) == SAO_OK);
    CHECK(after_reentrant_busy == catalog);
    CHECK(call_hook(adapter.owner, handle, "action_call_count") == "3");
    REQUIRE(call_hook(adapter.owner, handle, "set_reentrant_unload", "[false]") == "null");
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(), "{}") == SAO_OK);
    CHECK(call_hook(adapter.owner, handle, "action_call_count") == "4");
    REQUIRE(sao_plugins_ctx_unsubscribe(context, unload_token) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke(provider.provider_id.c_str(), provider.generation,
                                             explicit_action.c_str(),
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua menu category registration rolls back failed lifecycle hooks",
          "[lua][adapter][menu][rollback]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    adapter_registration adapter;

    temp_directory load_failure(L"menu_load_failure");
    write_text(load_failure.path / L"plugin.lua", R"lua(
local function build_menu()
    return {{label = "回滚", command = function() end}}
end
function on_load(ctx)
    ctx:register_menu_category("回滚分类", "退", build_menu, 1.25)
    error("menu load rollback fixture")
end
)lua");
    auto failed_load = add_plugin(make_manifest("lua_menu_load_failure", load_failure.path));
    CHECK(sao_plugins_lifecycle_load(failed_load) == SAO_ERR_OS_CALL_FAILED);
    menu_catalog_snapshot catalog;
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    remove_plugin(failed_load);

    temp_directory script_failure(L"menu_script_failure");
    write_text(script_failure.path / L"plugin.lua", R"lua(
local function build_menu()
    return {{label = "脚本回滚", command = function() end}}
end
ctx:register_menu_category("脚本回滚分类", "脚", build_menu, 1.5)
error("menu script rollback fixture")
)lua");
    auto failed_script = add_plugin(make_manifest("lua_menu_script_failure", script_failure.path));
    CHECK(sao_plugins_lifecycle_load(failed_script) == SAO_ERR_OS_CALL_FAILED);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    CHECK(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    plugin_context_t* failed_context = reinterpret_cast<plugin_context_t*>(1);
    CHECK(sao_plugins_lifecycle_get_context(failed_script, &failed_context) ==
          SAO_ERR_NOT_INITIALIZED);
    CHECK(failed_context == nullptr);
    remove_plugin(failed_script);

    temp_directory enable_failure(L"menu_enable_failure");
    write_text(enable_failure.path / L"plugin.lua", R"lua(
local context = nil
local extension_id = ""
local fail_enable = true
local function build_menu()
    return {{label = "启用回滚", command = function() end}}
end
function on_load(ctx) context = ctx end
function on_enable()
    extension_id = context:register_menu_category(
        "启用回滚分类", "启", build_menu, 3.0)
    if fail_enable then
        fail_enable = false
        error("menu enable rollback fixture")
    end
end
function get_extension_id() return extension_id end
)lua");
    auto failed_enable = add_plugin(make_manifest("lua_menu_enable_failure", enable_failure.path));
    REQUIRE(sao_plugins_lifecycle_load(failed_enable) == SAO_OK);
    CHECK(sao_plugins_lifecycle_enable(failed_enable) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(failed_enable) == lifecycle_state::loaded_disabled);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    const std::string extension_id = call_hook(adapter.owner, failed_enable, "get_extension_id");
    REQUIRE(extension_id.rfind("\"menu-", 0) == 0);
    const std::string provider_id =
        "lua_menu_enable_failure/" + extension_id.substr(1, extension_id.size() - 2);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), 1, "unused", "{}") ==
          SAO_ERR_HANDLE_INVALID);
    const int32_t retry_status = sao_plugins_lifecycle_enable(failed_enable);
    if (retry_status != SAO_OK) {
        char* error = nullptr;
        if (sao_plugins_luahost_loader_adapter_get_last_error(adapter.owner, failed_enable,
                                                              &error) == SAO_OK &&
            error != nullptr) {
            INFO("Lua retry error: " << error);
            sao_plugins_luahost_free_string(error);
        }
    }
    REQUIRE(retry_status == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK(catalog.providers[0].provider_id == provider_id);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), catalog.providers[0].generation,
                                             catalog.providers[0].rows[0].action_id.c_str(),
                                             "{}") == SAO_OK);
    const auto enabled = catalog;
    REQUIRE(sao_plugins_lifecycle_disable(failed_enable) == SAO_OK);
    catalog = {};
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    CHECK(catalog.providers.empty());
    CHECK(catalog.roots.empty());
    REQUIRE(sao_plugins_lifecycle_enable(failed_enable) == SAO_OK);
    REQUIRE(snapshot_menu_catalog(catalog) == SAO_OK);
    REQUIRE(catalog.providers.size() == 1);
    REQUIRE(catalog.providers[0].rows.size() == 1);
    CHECK(catalog.providers[0].provider_id == provider_id);
    CHECK(catalog.providers[0].generation != enabled.providers[0].generation);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), enabled.providers[0].generation,
                                             enabled.providers[0].rows[0].action_id.c_str(),
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_lifecycle_unload(failed_enable) == SAO_OK);
    CHECK(sao_plugins_entity_provider_invoke(provider_id.c_str(), catalog.providers[0].generation,
                                             catalog.providers[0].rows[0].action_id.c_str(),
                                             "{}") == SAO_ERR_HANDLE_INVALID);
    remove_plugin(failed_enable);
    adapter.unregister();
}

TEST_CASE("Lua menu category rejects invalid candidates atomically",
          "[lua][adapter][menu][budget]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory temp(L"menu_budget");
    write_text(temp.path / L"plugin.lua", R"lua(
local mode = 0
local invalid_priority_rejected = false
local embedded_nul_rejected = false

local function command() end

local function build_menu()
    if mode == 0 then
        return {{
            id = "baseline", label = "baseline",
            payload_json = "{\"raw\":true}",
        }}
    end
    if mode == 1 then
        return {[1] = {label = "row"}, extra = true}
    end
    if mode == 2 then
        return {{label = string.rep("x", 16385)}}
    end
    if mode == 3 then
        return {{label = "nul\0label"}}
    end
    if mode == 4 then
        local rows = {}
        for index = 1, 4097 do rows[index] = {label = "row"} end
        return rows
    end
    if mode == 5 then
        local rows = {}
        local payload = string.rep("x", 2048)
        for index = 1, 4096 do
            rows[index] = {label = "row", payload_json = payload}
        end
        return rows
    end
    if mode == 6 then
        local rows = {}
        for index = 1, 4096 do
            rows[index] = {
                action_id = "remembered-" .. index,
                label = "row",
                command = command,
            }
        end
        return rows
    end
    if mode == 7 then
        return {{action_id = "overflow", label = "row", command = command}}
    end
    if mode == 8 then
        return {{label = "invalid flag", keep_menu_open = "false"}}
    end
    if mode == 9 then
        return {{label = "invalid payload", payload_json = "{invalid"}}
    end
    return {{label = "not actionable", can_activate = true}}
end

function on_load(ctx)
    invalid_priority_rejected = not pcall(function()
        ctx:register_menu_category("bad-priority", "", build_menu, 0 / 0)
    end)
    embedded_nul_rejected = not pcall(function()
        ctx:register_menu_category("bad\0name", "", build_menu)
    end)
    ctx:register_menu_category("Budget", "", build_menu)
end

function set_mode(value) mode = value end
function validation_stats()
    local surface_ok, surface_error = pcall(function()
        ctx:register_menu_surface("unsupported", {})
    end)
    return {
        invalid_priority_rejected = invalid_priority_rejected,
        embedded_nul_rejected = embedded_nul_rejected,
        menu_surface_present = type(ctx.register_menu_surface) == "function",
        menu_surface_unsupported = (not surface_ok) and
            string.find(surface_error, "-1000", 1, true) ~= nil,
        action_handler_present = type(ctx.register_action_handler) == "function",
    }
end
)lua");

    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_menu_budget", temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    const std::string validation = call_hook(adapter.owner, handle, "validation_stats");
    CHECK(validation.find("\"invalid_priority_rejected\":true") != std::string::npos);
    CHECK(validation.find("\"embedded_nul_rejected\":true") != std::string::npos);
    CHECK(validation.find("\"menu_surface_present\":true") != std::string::npos);
    CHECK(validation.find("\"menu_surface_unsupported\":true") != std::string::npos);
    CHECK(validation.find("\"action_handler_present\":true") != std::string::npos);

    menu_catalog_snapshot baseline;
    REQUIRE(snapshot_menu_catalog(baseline) == SAO_OK);
    REQUIRE(baseline.providers.size() == 1);
    REQUIRE(baseline.providers[0].rows.size() == 1);
    CHECK(baseline.providers[0].rows[0].payload == "{\"raw\":true}");
    CHECK(baseline.providers[0].revision == 1);

    for (int mode = 1; mode <= 5; ++mode) {
        REQUIRE(call_hook(adapter.owner, handle, "set_mode",
                          ("[" + std::to_string(mode) + "]").c_str()) == "null");
        menu_catalog_snapshot candidate = baseline;
        CHECK(snapshot_menu_catalog(candidate) == SAO_ERR_OS_CALL_FAILED);
        CHECK(candidate == baseline);
    }

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[6]") == "null");
    menu_catalog_snapshot remembered;
    REQUIRE(snapshot_menu_catalog(remembered) == SAO_OK);
    REQUIRE(remembered.providers.size() == 1);
    REQUIRE(remembered.providers[0].rows.size() == 4096);
    CHECK(remembered.providers[0].revision == 2);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[7]") == "null");
    menu_catalog_snapshot overflow = remembered;
    CHECK(snapshot_menu_catalog(overflow) == SAO_ERR_OS_CALL_FAILED);
    CHECK(overflow == remembered);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[8]") == "null");
    menu_catalog_snapshot invalid_flag = remembered;
    CHECK(snapshot_menu_catalog(invalid_flag) == SAO_ERR_OS_CALL_FAILED);
    CHECK(invalid_flag == remembered);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[9]") == "null");
    menu_catalog_snapshot invalid_payload = remembered;
    CHECK(snapshot_menu_catalog(invalid_payload) == SAO_ERR_OS_CALL_FAILED);
    CHECK(invalid_payload == remembered);

    REQUIRE(call_hook(adapter.owner, handle, "set_mode", "[10]") == "null");
    menu_catalog_snapshot not_actionable;
    REQUIRE(snapshot_menu_catalog(not_actionable) == SAO_OK);
    REQUIRE(not_actionable.providers.size() == 1);
    REQUIRE(not_actionable.providers[0].rows.size() == 1);
    CHECK_FALSE(not_actionable.providers[0].rows[0].can_activate);

    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua direct unload detaches context menu providers",
          "[lua][adapter][menu][direct_unload]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    temp_directory owner_temp(L"menu_direct_owner");
    write_text(owner_temp.path / L"plugin.lua", "function on_load(ctx) end");
    adapter_registration adapter;
    auto handle = add_plugin(make_manifest("lua_menu_direct_owner", owner_temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);

    sao::plugins::loader::plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);

    temp_directory direct_temp(L"menu_direct_script");
    write_text(direct_temp.path / L"plugin.lua", R"lua(
ctx:register_menu_category("Direct", "", function()
    return {{label = "row", command = function() end}}
end)
)lua");
    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);

    for (int iteration = 0; iteration < 2; ++iteration) {
        lua_plugin_handle_t direct_plugin = nullptr;
        REQUIRE(sao_plugins_luahost_load_script(
                    sao_plugins_luahost_state(host), direct_temp.path.c_str(), "plugin.lua",
                    "lua_menu_direct_owner", context, &direct_plugin) == SAO_OK);
        REQUIRE(direct_plugin != nullptr);
        REQUIRE(sao_plugins_luahost_unload_script(direct_plugin) == SAO_OK);
    }

    lua_plugin_handle_t retained_direct_plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(
                sao_plugins_luahost_state(host), direct_temp.path.c_str(), "plugin.lua",
                "lua_menu_direct_owner", context, &retained_direct_plugin) == SAO_OK);
    REQUIRE(retained_direct_plugin != nullptr);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_lifecycle_state(handle) == lifecycle_state::failed);
    REQUIRE(sao_plugins_luahost_unload_script(retained_direct_plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
    adapter.unregister();
    remove_plugin(handle);
}

TEST_CASE("Lua direct unload restores ctx menus after resource teardown failure",
          "[lua][adapter][menu][direct_unload][rollback]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    platform_probe probe;
    platform_registration provider(probe);
    adapter_registration adapter;
    temp_directory owner_temp(L"menu_direct_retry_owner");
    write_text(owner_temp.path / L"plugin.lua", "function on_load(ctx) end");
    auto handle = add_plugin(make_manifest("lua_menu_direct_retry_owner", owner_temp.path));
    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);

    temp_directory direct_temp(L"menu_direct_retry_script");
    write_text(direct_temp.path / L"plugin.lua", R"lua(
menu_action_count = 0
ctx:set_interval(function() end, 60.0)
ctx:register_menu_category("Direct retry", "", function()
    return {{label = "row", command = function()
        menu_action_count = menu_action_count + 1
    end}}
end)
)lua");
    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);
    lua_plugin_handle_t direct_plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(
                sao_plugins_luahost_state(host), direct_temp.path.c_str(), "plugin.lua",
                "lua_menu_direct_retry_owner", context, &direct_plugin) == SAO_OK);
    const std::string provider_id = "lua_menu_direct_retry_owner/menu-";
    menu_catalog_snapshot before;
    REQUIRE(snapshot_menu_catalog(before) == SAO_OK);
    const auto original = std::find_if(
        before.providers.begin(), before.providers.end(),
        [&provider_id](const auto& item) { return item.provider_id.starts_with(provider_id); });
    REQUIRE(original != before.providers.end());
    const std::string full_provider_id = original->provider_id;
    const std::uint64_t original_generation = original->generation;

    {
        std::lock_guard lock(probe.mutex);
        probe.unregister_timer_status = SAO_PLUGINS_ERR_BUSY;
    }
    REQUIRE(sao_plugins_luahost_unload_script(direct_plugin) == SAO_PLUGINS_ERR_BUSY);
    menu_catalog_snapshot restored;
    REQUIRE(snapshot_menu_catalog(restored) == SAO_OK);
    const auto current = std::find_if(
        restored.providers.begin(), restored.providers.end(),
        [&full_provider_id](const auto& item) { return item.provider_id == full_provider_id; });
    REQUIRE(current != restored.providers.end());
    REQUIRE(current->generation != original_generation);
    REQUIRE(current->rows.size() == 1);
    REQUIRE(sao_plugins_entity_provider_invoke(full_provider_id.c_str(), current->generation,
                                               current->rows[0].action_id.c_str(), "{}") == SAO_OK);
    int32_t status = SAO_OK;
    CHECK(execute_host(host, "return type(ctx) .. ':' .. menu_action_count", status) ==
          "userdata:1");
    REQUIRE(status == SAO_OK);

    {
        std::lock_guard lock(probe.mutex);
        probe.unregister_timer_status = SAO_OK;
    }
    REQUIRE(sao_plugins_luahost_unload_script(direct_plugin) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    remove_plugin(handle);
    adapter.unregister();
    provider.unregister();
}

TEST_CASE("Lua host destroy automatically disarms sandbox", "[lua][sandbox][destroy]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    lua_host_config config{};
    config.install_stdlib = true;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    lua_sandbox_config sandbox{};
    REQUIRE(sao_plugins_luahost_sandbox_arm(state, &sandbox) == SAO_OK);
    REQUIRE(sao_plugins_luahost_sandbox_is_armed(state));
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
    CHECK_FALSE(sao_plugins_luahost_sandbox_is_armed(state));
}
