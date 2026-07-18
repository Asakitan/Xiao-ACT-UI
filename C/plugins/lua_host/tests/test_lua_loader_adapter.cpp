#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/lua_host/lua_call.h"
#include "sao/plugins/lua_host/lua_host.h"
#include "sao/plugins/lua_host/lua_sandbox.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using namespace sao::plugins::loader;
using namespace sao::plugins::lua_host;
namespace fs = std::filesystem;

namespace {

struct temp_directory {
    fs::path path;

    explicit temp_directory(const wchar_t* label) {
        wchar_t base[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, base) > 0);
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::path(base) /
               (std::wstring(L"sao_lua_adapter_") + label + L"_" +
                std::to_wstring(stamp));
        REQUIRE(fs::create_directories(path));
    }

    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string path_utf8(const fs::path& path) {
    const auto& wide = path.native();
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    REQUIRE(needed > 0);
    std::string result(static_cast<size_t>(needed), '\0');
    REQUIRE(WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
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
    REQUIRE(sao_plugins_registry_add_plugin(
                sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    REQUIRE(handle != nullptr);
    return handle;
}

void remove_plugin(plugin_handle_t handle) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(),
                                        handle) == SAO_OK);
}

struct adapter_registration {
    lua_loader_adapter_owner_t owner = nullptr;

    adapter_registration() {
        lua_host_config config{};
        config.max_instructions_per_run = 1000000u;
        REQUIRE(sao_plugins_luahost_register_loader_adapter(&config, &owner) ==
                SAO_OK);
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

void gate_callback(const char*, const char*, void* user_data) {
    auto& value = *static_cast<gate*>(user_data);
    std::unique_lock lock(value.mutex);
    value.entered = true;
    value.condition.notify_all();
    value.condition.wait(lock, [&value] { return value.released; });
}

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
    REQUIRE(sao_plugins_luahost_load_script(
                sao_plugins_luahost_state(host), temp.path.c_str(),
                "plugin.lua", "lua_file_loader", nullptr, &plugin) == SAO_OK);
    REQUIRE(plugin != nullptr);
    CHECK(sao_plugins_luahost_has_hook(plugin, "custom"));
    CHECK_FALSE(sao_plugins_luahost_has_hook(plugin, "on_load"));
    CHECK(sao_plugins_luahost_call_on_load(plugin) == SAO_OK);
    char* result = nullptr;
    REQUIRE(sao_plugins_luahost_call_hook(plugin, "custom", "[4]",
                                          &result) == SAO_OK);
    REQUIRE(std::string(result) == "12");
    sao_plugins_luahost_free_string(result);
    result = reinterpret_cast<char*>(1);
    CHECK(sao_plugins_luahost_call_hook(plugin, "missing", nullptr, &result) ==
          SAO_OK);
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
function on_load(ctx)
    ctx:set_defaults({rate = 3})
    ctx:set_setting("rate", 7)
    loaded_value = ctx:get_setting("rate", 0)
    ctx:register_ui_panel("lua_panel", {title = "Lua Panel"})
    ctx:subscribe("lua_tick", function(event)
        observed = event.payload.value
    end)
    ctx:emit("lua_tick", {value = 11})
end
function on_enable(ctx) enabled = ctx ~= nil end
function on_disable(ctx) disabled = ctx ~= nil end
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
        if (sao_plugins_luahost_loader_adapter_get_last_error(
                adapter.owner, handle, &raw_error) == SAO_OK &&
            raw_error != nullptr) {
            load_error = raw_error;
            sao_plugins_luahost_free_string(raw_error);
        }
    }
    INFO("Lua adapter load error: " << load_error);
    REQUIRE(load_status == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(handle) ==
            lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 1);
        CHECK(sao_plugins_luahost_unregister_loader_adapter(adapter.owner) ==
            SAO_PLUGINS_ERR_BUSY);

    plugin_context_t* context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK);
    REQUIRE(context != nullptr);
    char* setting = nullptr;
    REQUIRE(sao_plugins_ctx_get_setting(context, "rate", &setting) == SAO_OK);
    REQUIRE(std::string(setting) == "7");
    sao_plugins_ctx_free_string(setting);

    const auto panels = snapshot_extensions(sao_plugins_registry_instance(),
                                            extension_kind::ui_panel);
    REQUIRE(std::any_of(panels.begin(), panels.end(), [](const auto& panel) {
        return panel.plugin_id == "lua_adapter_lifecycle" &&
               panel.id == "lua_panel";
    }));

    char* result = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, handle, "probe", "[2,5]", &result) == SAO_OK);
    REQUIRE(result != nullptr);
    const std::string result_text(result);
    sao_plugins_luahost_free_string(result);
    CHECK(result_text.find("\"sum\":7") != std::string::npos);
    CHECK(result_text.find("\"loaded\":7") != std::string::npos);
    CHECK(result_text.find("\"observed\":11") != std::string::npos);
    CHECK(result_text.find("\"io_type\":\"nil\"") != std::string::npos);
    CHECK(result_text.find("\"require_type\":\"nil\"") !=
          std::string::npos);

        result = nullptr;
        REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, handle, "unsupported_probe", nullptr,
                &result) == SAO_OK);
        REQUIRE(result != nullptr);
        CHECK(std::string(result) == "false");
        sao_plugins_luahost_free_string(result);

    result = reinterpret_cast<char*>(1);
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, handle, "missing_hook", nullptr, &result) ==
            SAO_OK);
    REQUIRE(result == nullptr);
    REQUIRE(sao_plugins_lifecycle_enable(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_disable(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    adapter.unregister();
    remove_plugin(handle);
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
    REQUIRE(sao_plugins_lifecycle_state(handle) ==
            lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, handle, "allow_unload", nullptr, nullptr) ==
            SAO_OK);
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
    REQUIRE(sao_plugins_ctx_subscribe(context, "lease_gate", gate_callback,
                                      &value, &token) == SAO_OK);
    std::atomic<int32_t> call_status{SAO_ERR_NOT_INITIALIZED};
    std::thread caller([&] {
        char* result = nullptr;
        call_status = sao_plugins_luahost_loader_adapter_call_hook(
            adapter.owner, handle, "hold", nullptr, &result);
        sao_plugins_luahost_free_string(result);
    });
    bool entered = false;
    {
        std::unique_lock lock(value.mutex);
        entered = value.condition.wait_for(
            lock, std::chrono::seconds(5), [&value] { return value.entered; });
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
    CHECK(sao_plugins_lifecycle_state(handle) ==
          lifecycle_state::loaded_disabled);
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

TEST_CASE("Lua adapter rolls back failed on_load and syntax failures",
          "[lua][adapter][rollback]") {
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
    auto failed =
        add_plugin(make_manifest("lua_adapter_hook_failure", hook_failure.path));
    REQUIRE(sao_plugins_lifecycle_load(failed) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(sao_plugins_lifecycle_state(failed) == lifecycle_state::failed);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    plugin_context_t* context = reinterpret_cast<plugin_context_t*>(1);
    REQUIRE(sao_plugins_lifecycle_get_context(failed, &context) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(context == nullptr);
    const auto panels = snapshot_extensions(sao_plugins_registry_instance(),
                                            extension_kind::ui_panel);
    CHECK(std::none_of(panels.begin(), panels.end(), [](const auto& panel) {
        return panel.plugin_id == "lua_adapter_hook_failure";
    }));
    char* error = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_get_last_error(
                adapter.owner, failed, &error) == SAO_OK);
    REQUIRE(error != nullptr);
    CHECK(std::string(error).find("on_load rollback marker") !=
          std::string::npos);
    sao_plugins_luahost_free_string(error);
    remove_plugin(failed);

    temp_directory syntax_failure(L"syntax_failure");
    write_text(syntax_failure.path / L"plugin.lua", "function on_load(");
    auto malformed = add_plugin(
        make_manifest("lua_adapter_syntax_failure", syntax_failure.path));
    REQUIRE(sao_plugins_lifecycle_load(malformed) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_luahost_loader_adapter_plugin_count(adapter.owner) == 0);
    remove_plugin(malformed);
    adapter.unregister();
}

TEST_CASE("Lua adapter maps manifest permissions and rejects duplicates",
          "[lua][adapter][sandbox][registration]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua 5.4 capability is disabled");
        return;
    }
    adapter_registration adapter;
    lua_loader_adapter_owner_t duplicate = reinterpret_cast<
        lua_loader_adapter_owner_t>(1);
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

TEST_CASE("Lua adapter gives every plugin an independent state",
          "[lua][adapter][isolation]") {
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
    auto first = add_plugin(make_manifest("lua_adapter_state_first",
                                          first_dir.path));
    auto second = add_plugin(make_manifest("lua_adapter_state_second",
                                           second_dir.path));
    REQUIRE(sao_plugins_lifecycle_load(first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_load(second) == SAO_OK);

    char* value = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, first, "bump", nullptr, &value) == SAO_OK);
    REQUIRE(std::string(value) == "1");
    sao_plugins_luahost_free_string(value);
    value = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, first, "bump", nullptr, &value) == SAO_OK);
    REQUIRE(std::string(value) == "2");
    sao_plugins_luahost_free_string(value);
    value = nullptr;
    REQUIRE(sao_plugins_luahost_loader_adapter_call_hook(
                adapter.owner, second, "bump", nullptr, &value) == SAO_OK);
    REQUIRE(std::string(value) == "1");
    sao_plugins_luahost_free_string(value);

    REQUIRE(sao_plugins_lifecycle_unload(first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(second) == SAO_OK);
    adapter.unregister();
    remove_plugin(first);
    remove_plugin(second);
}

TEST_CASE("Lua host destroy automatically disarms sandbox",
          "[lua][sandbox][destroy]") {
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
