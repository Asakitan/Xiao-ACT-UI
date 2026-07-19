#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/lua_host/lua_call.h"
#include "sao/plugins/lua_host/lua_host.h"
#include "sao/plugins/lua_host/lua_sandbox.h"
#include "sao/plugins/lua_host/lua_stdlib.h"

#include <windows.h>

extern "C" {
#include <lua.h>
#include <lualib.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sao::plugins::lua_host;
namespace fs = std::filesystem;

namespace {

struct temp_directory final {
    explicit temp_directory(const wchar_t* suffix) {
        wchar_t base[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, base) > 0);
        path = fs::path(base) /
               (std::wstring(L"sao_lua_hardening_") + suffix + L"_" +
                std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        REQUIRE(fs::create_directories(path));
    }

    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

void write_text(const fs::path& path, const char* source) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << source;
    REQUIRE(output.good());
}

lua_host_handle_t create_host(bool stdlib = true, lua_host_config config = {}) {
    config.install_stdlib = stdlib;
    lua_host_handle_t host = nullptr;
    REQUIRE(sao_plugins_luahost_create(&config, &host) == SAO_OK);
    REQUIRE(host != nullptr);
    return host;
}

std::string execute_result(lua_host_handle_t host, const char* source, int32_t& status) {
    char* result = nullptr;
    char* error = nullptr;
    status = sao_plugins_luahost_execute(host, source, std::strlen(source), &result, &error);
    std::string value = result == nullptr ? "" : result;
    sao_plugins_luahost_free_string(result);
    sao_plugins_luahost_free_string(error);
    return value;
}

std::string call_hook(lua_plugin_handle_t plugin, const char* name, const char* arguments,
                      int32_t& status) {
    char* result = nullptr;
    status = sao_plugins_luahost_call_hook(plugin, name, arguments, &result);
    std::string value = result == nullptr ? "" : result;
    sao_plugins_luahost_free_string(result);
    return value;
}

struct callback_gate final {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

struct self_destroy_probe final {
    lua_host_handle_t host = nullptr;
    int32_t status = SAO_ERR_NOT_INITIALIZED;
    size_t calls = 0;
};

void self_destroy_message_callback(const char*, int, void* user_data) {
    auto* probe = static_cast<self_destroy_probe*>(user_data);
    ++probe->calls;
    probe->status = sao_plugins_luahost_destroy(probe->host);
}

void blocking_message_callback(const char*, int, void* user_data) {
    auto* gate = static_cast<callback_gate*>(user_data);
    std::unique_lock lock(gate->mutex);
    gate->entered = true;
    gate->condition.notify_all();
    gate->condition.wait(lock, [&] { return gate->released; });
}

void wait_for_callback(callback_gate& gate) {
    std::unique_lock lock(gate.mutex);
    gate.condition.wait(lock, [&] { return gate.entered; });
}

void release_callback(callback_gate& gate) {
    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.condition.notify_all();
}

std::atomic_uint32_t g_original_hook_calls{0};

void original_count_hook(lua_State*, lua_Debug*) {
    g_original_hook_calls.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("Lua direct operations serialize and restore the stack",
          "[lua][hardening][concurrency][stack]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    lua_host_handle_t host = create_host();
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    const int original_top = lua_gettop(state);

    constexpr int thread_count = 6;
    constexpr int iterations = 80;
    std::atomic_uint32_t failures{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (int worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([host, &failures] {
            for (int iteration = 0; iteration < iterations; ++iteration) {
                int32_t status = SAO_OK;
                const std::string result = execute_result(host, "return 20 + 22", status);
                if (status != SAO_OK || result != "42") {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    CHECK(failures.load(std::memory_order_relaxed) == 0);
    CHECK(lua_gettop(state) == original_top);

    for (int iteration = 0; iteration < 20; ++iteration) {
        int32_t status = SAO_OK;
        (void)execute_result(host, "error({code = 9})", status);
        CHECK(status == SAO_ERR_OS_CALL_FAILED);
        CHECK(lua_gettop(state) == original_top);
    }
    int32_t status = SAO_OK;
    CHECK(execute_result(host, "return 'recovered'", status) == "recovered");
    CHECK(status == SAO_OK);
    CHECK(lua_gettop(state) == original_top);
    CHECK(
        execute_result(host, "setmetatable(_G, {__index = function() error('lookup') end})", status)
            .empty());
    CHECK(status == SAO_OK);
    char* result = nullptr;
    char* error = nullptr;
    CHECK(sao_plugins_luahost_call_function(host, "missing", &result, &error) ==
          SAO_ERR_OS_CALL_FAILED);
    CHECK(result == nullptr);
    CHECK(error != nullptr);
    sao_plugins_luahost_free_string(result);
    sao_plugins_luahost_free_string(error);
    CHECK(lua_gettop(state) == original_top);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua SAO stdlib installs executable helpers", "[lua][stdlib][sao]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    lua_host_handle_t host = create_host();
    int32_t status = SAO_OK;
    const std::string result = execute_result(host,
                                              R"lua(
local decoded = sao_json.decode('{"answer":42}')
local encoded = sao_json.encode({answer = decoded.answer})
sao_log("stdlib-ready", "debug")
sao_sleep(0)
return type(sao_json) .. ":" .. type(sao_time) .. ":" ..
       tostring(decoded.answer) .. ":" .. encoded
)lua",
                                              status);
    REQUIRE(status == SAO_OK);
    CHECK(result.find("table:function:42:") == 0);
    CHECK(result.find("\"answer\":42") != std::string::npos);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua JSON rejects invalid UTF-8 and preserves embedded NUL keys",
          "[lua][hardening][json][utf8][stack]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    lua_host_handle_t host = create_host();
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    const int original_top = lua_gettop(state);
    int32_t status = SAO_OK;
    const std::string result = execute_result(host,
                                              R"lua(
local invalid = string.char(0xC3, 0x28)
local decode_ok = pcall(function() sao_json.decode(invalid) end)
local encode_ok = pcall(function() sao_json.encode({value = invalid}) end)
local key = "left" .. string.char(0) .. "right"
local encoded = sao_json.encode({[key] = 7})
local decoded = sao_json.decode(encoded)
return (not decode_ok) and (not encode_ok) and decoded[key] == 7
)lua",
                                              status);
    CHECK(status == SAO_OK);
    CHECK(result == "true");
    CHECK(lua_gettop(state) == original_top);

    temp_directory temp(L"invalid_utf8_arguments");
    write_text(temp.path / L"plugin.lua", "function echo(value) return value end");
    lua_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(state, temp.path.c_str(), "plugin.lua",
                                            "invalid_utf8_arguments", nullptr, &plugin) == SAO_OK);
    const char invalid_arguments[] = {'[', '"', static_cast<char>(0xC3), '(', '"', ']', '\0'};
    char* hook_result = reinterpret_cast<char*>(1);
    CHECK(sao_plugins_luahost_call_hook(plugin, "echo", invalid_arguments, &hook_result) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(hook_result == nullptr);
    CHECK(lua_gettop(state) == original_top);
    REQUIRE(sao_plugins_luahost_unload_script(plugin) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua dependency path is ordered and restored on unload",
          "[lua][path][precedence][unload]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    temp_directory temp(L"path_restore");
    for (const auto* name : {L"engine", L"libs", L"vendor"}) {
        REQUIRE(fs::create_directories(temp.path / name));
    }
    write_text(temp.path / L"engine" / L"priority.lua", "return 'engine'");
    write_text(temp.path / L"libs" / L"priority.lua", "return 'libs'");
    write_text(temp.path / L"vendor" / L"priority.lua", "return 'vendor'");
    write_text(temp.path / L"priority.lua", "return 'plugin'");
    write_text(temp.path / L"plugin.lua", R"lua(
local selected = require("priority")
function path_probe()
    return {selected = selected, path = package.path}
end
)lua");

    lua_host_handle_t host = create_host();
    int32_t status = SAO_OK;
    CHECK(execute_result(host, "saved_package_path = package.path; return true", status) == "true");
    REQUIRE(status == SAO_OK);
    lua_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), temp.path.c_str(),
                                            "plugin.lua", "lua_path_restore", nullptr,
                                            &plugin) == SAO_OK);
    const std::string probe = call_hook(plugin, "path_probe", nullptr, status);
    REQUIRE(status == SAO_OK);
    CHECK(probe.find("\"selected\":\"engine\"") != std::string::npos);
    const auto engine = probe.find("engine/?.lua");
    const auto libs = probe.find("libs/?.lua");
    const auto vendor = probe.find("vendor/?.lua");
    const auto plugin_root = probe.rfind("/?.lua");
    REQUIRE(engine != std::string::npos);
    REQUIRE(libs != std::string::npos);
    REQUIRE(vendor != std::string::npos);
    REQUIRE(plugin_root != std::string::npos);
    CHECK(engine < libs);
    CHECK(libs < vendor);
    CHECK(vendor < plugin_root);
    REQUIRE(sao_plugins_luahost_unload_script(plugin) == SAO_OK);
    CHECK(execute_result(host, "return package.path == saved_package_path", status) == "true");
    REQUIRE(status == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua dependency path layers recompute after non-LIFO unload",
          "[lua][path][layering][unload]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    temp_directory temp(L"path_layers");
    const fs::path first_root = temp.path / L"layer_a";
    const fs::path middle_root = temp.path / L"layer_b";
    const fs::path last_root = temp.path / L"layer_c";
    REQUIRE(fs::create_directories(first_root));
    REQUIRE(fs::create_directories(middle_root));
    REQUIRE(fs::create_directories(last_root));
    write_text(first_root / L"plugin.lua", "function layer_path() return package.path end");
    write_text(middle_root / L"plugin.lua", "function layer_path() return package.path end");
    write_text(last_root / L"plugin.lua", "function layer_path() return package.path end");

    lua_host_handle_t host = create_host();
    int32_t status = SAO_OK;
    CHECK(execute_result(host, "base_package_path = package.path; return true", status) == "true");
    lua_plugin_handle_t first = nullptr;
    lua_plugin_handle_t middle = nullptr;
    lua_plugin_handle_t last = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), first_root.c_str(),
                                            "plugin.lua", "path_layer_a", nullptr,
                                            &first) == SAO_OK);
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), middle_root.c_str(),
                                            "plugin.lua", "path_layer_b", nullptr,
                                            &middle) == SAO_OK);
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), last_root.c_str(),
                                            "plugin.lua", "path_layer_c", nullptr,
                                            &last) == SAO_OK);
    CHECK(call_hook(last, "layer_path", nullptr, status).find("layer_c") != std::string::npos);
    REQUIRE(status == SAO_OK);

    REQUIRE(sao_plugins_luahost_unload_script(middle) == SAO_OK);
    CHECK(call_hook(last, "layer_path", nullptr, status).find("layer_c") != std::string::npos);
    REQUIRE(status == SAO_OK);
    REQUIRE(sao_plugins_luahost_unload_script(last) == SAO_OK);
    const std::string recomputed = call_hook(first, "layer_path", nullptr, status);
    REQUIRE(status == SAO_OK);
    CHECK(recomputed.find("layer_a") != std::string::npos);
    CHECK(recomputed.find("layer_b") == std::string::npos);
    CHECK(recomputed.find("layer_c") == std::string::npos);
    REQUIRE(sao_plugins_luahost_unload_script(first) == SAO_OK);
    CHECK(execute_result(host, "return package.path == base_package_path", status) == "true");
    REQUIRE(status == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua host destroy waits for an active direct operation",
          "[lua][hardening][concurrency][closing]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    callback_gate gate;
    lua_host_config config{};
    config.message_callback = blocking_message_callback;
    config.callback_user_data = &gate;
    lua_host_handle_t host = create_host(true, config);

    std::atomic_int32_t execute_status{SAO_ERR_NOT_INITIALIZED};
    std::atomic_int32_t destroy_status{SAO_ERR_NOT_INITIALIZED};
    std::thread execute_thread([&] {
        int32_t status = SAO_OK;
        (void)execute_result(host, "print('blocked')", status);
        execute_status.store(status, std::memory_order_release);
    });
    wait_for_callback(gate);
    std::thread destroy_thread([&] {
        destroy_status.store(sao_plugins_luahost_destroy(host), std::memory_order_release);
    });
    release_callback(gate);
    execute_thread.join();
    destroy_thread.join();

    CHECK(execute_status.load(std::memory_order_acquire) == SAO_OK);
    CHECK(destroy_status.load(std::memory_order_acquire) == SAO_OK);
}

TEST_CASE("Lua print callback same-thread destroy returns BUSY and remains retryable",
          "[lua][hardening][concurrency][closing][reentry]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    self_destroy_probe probe;
    lua_host_config config{};
    config.message_callback = self_destroy_message_callback;
    config.callback_user_data = &probe;
    probe.host = create_host(true, config);

    int32_t execute_status = SAO_ERR_NOT_INITIALIZED;
    CHECK(execute_result(probe.host, "print('self-destroy'); return 42", execute_status) == "42");
    CHECK(execute_status == SAO_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.status == sao::plugins::loader::SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_luahost_destroy(probe.host) == SAO_OK);
}

TEST_CASE("Lua host destroy cancels an active sao_sleep",
          "[lua][hardening][concurrency][closing][sleep]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    callback_gate gate;
    lua_host_config config{};
    config.message_callback = blocking_message_callback;
    config.callback_user_data = &gate;
    lua_host_handle_t host = create_host(true, config);

    std::atomic_int32_t execute_status{SAO_ERR_NOT_INITIALIZED};
    std::atomic_int32_t destroy_status{SAO_ERR_NOT_INITIALIZED};
    std::thread execute_thread([&] {
        int32_t status = SAO_OK;
        (void)execute_result(host, "print('ready'); sao_sleep(30000)", status);
        execute_status.store(status, std::memory_order_release);
    });
    wait_for_callback(gate);
    const auto started = std::chrono::steady_clock::now();
    std::thread destroy_thread([&] {
        destroy_status.store(sao_plugins_luahost_destroy(host), std::memory_order_release);
    });
    const auto closing_deadline = started + std::chrono::seconds(2);
    while (sao_plugins_luahost_state(host) != nullptr &&
           std::chrono::steady_clock::now() < closing_deadline) {
        std::this_thread::yield();
    }
    CHECK(sao_plugins_luahost_state(host) == nullptr);
    release_callback(gate);
    execute_thread.join();
    destroy_thread.join();

    CHECK(execute_status.load(std::memory_order_acquire) == SAO_ERR_OS_CALL_FAILED);
    CHECK(destroy_status.load(std::memory_order_acquire) == SAO_OK);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
}

TEST_CASE("Lua plugin unload waits for an active hook", "[lua][hardening][concurrency][plugin]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    temp_directory temp(L"plugin_lease");
    write_text(temp.path / L"plugin.lua", "function blocking_hook() print('blocked') return 7 end");
    callback_gate gate;
    lua_host_config config{};
    config.message_callback = blocking_message_callback;
    config.callback_user_data = &gate;
    lua_host_handle_t host = create_host(true, config);
    lua_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), temp.path.c_str(),
                                            "plugin.lua", "plugin_lease", nullptr,
                                            &plugin) == SAO_OK);

    std::atomic_int32_t hook_status{SAO_ERR_NOT_INITIALIZED};
    std::atomic_int32_t unload_status{SAO_ERR_NOT_INITIALIZED};
    std::thread hook_thread([&] {
        int32_t status = SAO_OK;
        const std::string result = call_hook(plugin, "blocking_hook", nullptr, status);
        if (result != "7")
            status = SAO_ERR_OS_CALL_FAILED;
        hook_status.store(status, std::memory_order_release);
    });
    wait_for_callback(gate);
    std::thread unload_thread([&] {
        unload_status.store(sao_plugins_luahost_unload_script(plugin), std::memory_order_release);
    });
    release_callback(gate);
    hook_thread.join();
    unload_thread.join();

    CHECK(hook_status.load(std::memory_order_acquire) == SAO_OK);
    CHECK(unload_status.load(std::memory_order_acquire) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua host refuses close while a plugin is live", "[lua][hardening][closing][plugin]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    temp_directory temp(L"live_plugin");
    write_text(temp.path / L"plugin.lua", "function probe() return 31 end");
    lua_host_handle_t host = create_host();
    lua_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(sao_plugins_luahost_state(host), temp.path.c_str(),
                                            "plugin.lua", "live_plugin", nullptr,
                                            &plugin) == SAO_OK);
    CHECK(sao_plugins_luahost_destroy(host) != SAO_OK);
    int32_t status = SAO_OK;
    CHECK(call_hook(plugin, "probe", nullptr, status) == "31");
    CHECK(status == SAO_OK);
    REQUIRE(sao_plugins_luahost_unload_script(plugin) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua sandbox restores hooks and rejects stdlib injection",
          "[lua][hardening][sandbox][stdlib]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    lua_host_handle_t host = create_host();
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    g_original_hook_calls.store(0, std::memory_order_relaxed);
    lua_sethook(state, original_count_hook, LUA_MASKCOUNT, 3);

    lua_sandbox_config sandbox{};
    sandbox.max_instructions_per_run = 1000;
    REQUIRE(sao_plugins_luahost_sandbox_arm(state, &sandbox) == SAO_OK);
    CHECK(sao_plugins_luahost_install_one_stdlib(state, LUA_DBLIBNAME) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_luahost_sandbox_disarm(state) == SAO_OK);
    CHECK(lua_gethook(state) == original_count_hook);
    CHECK(lua_gethookmask(state) == LUA_MASKCOUNT);
    CHECK(lua_gethookcount(state) == 3);
    CHECK(sao_plugins_luahost_install_one_stdlib(state, LUA_DBLIBNAME) == SAO_OK);

    int32_t status = SAO_OK;
    (void)execute_result(host, "local x = 0 for i = 1, 30 do x = x + i end return x", status);
    CHECK(status == SAO_OK);
    CHECK(g_original_hook_calls.load(std::memory_order_relaxed) > 0);
    lua_sethook(state, nullptr, 0, 0);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua sandbox allocator lifecycle serializes with execution",
          "[lua][hardening][sandbox][concurrency]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    lua_host_handle_t host = create_host();
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    std::atomic_uint32_t failures{0};
    std::thread executor([&] {
        for (int iteration = 0; iteration < 160; ++iteration) {
            int32_t status = SAO_OK;
            const std::string value = execute_result(host, "return 'serialized'", status);
            if (status != SAO_OK || value != "serialized") {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    lua_sandbox_config sandbox{};
    sandbox.max_instructions_per_run = 0;
    sandbox.max_memory_bytes = 8 * 1024 * 1024;
    for (int iteration = 0; iteration < 40; ++iteration) {
        if (sao_plugins_luahost_sandbox_arm(state, &sandbox) != SAO_OK) {
            failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        (void)sao_plugins_luahost_sandbox_bytes_used(state);
        if (sao_plugins_luahost_sandbox_disarm(state) != SAO_OK) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    executor.join();
    CHECK(failures.load(std::memory_order_relaxed) == 0);
    CHECK_FALSE(sao_plugins_luahost_sandbox_is_armed(state));
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}

TEST_CASE("Lua require is limited to controlled pure Lua roots", "[lua][hardening][require]") {
    if (!sao_plugins_luahost_is_available()) {
        SUCCEED("Lua runtime is unavailable");
        return;
    }
    temp_directory plugin_root(L"require_plugin");
    temp_directory ambient_root(L"require_ambient");
    write_text(plugin_root.path / L"safe.lua", "return {value = 17}");
    write_text(ambient_root.path / L"ambient.lua", "return {value = 99}");
    write_text(plugin_root.path / L"plugin.lua", R"lua(
local safe = require("safe")
local ambient_ok = pcall(require, "ambient")
local traversal_ok = pcall(require, "../ambient")
function safe_value() return safe.value end
function ambient_loaded() return ambient_ok end
function traversal_loaded() return traversal_ok end
)lua");

    lua_host_handle_t host = create_host();
    lua_State* state = sao_plugins_luahost_state(host);
    REQUIRE(state != nullptr);
    const std::string ambient_path = ambient_root.path.string() + "/?.lua";
    lua_getglobal(state, "package");
    REQUIRE(lua_istable(state, -1));
    lua_pushlstring(state, ambient_path.data(), ambient_path.size());
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);

    lua_sandbox_config sandbox{};
    sandbox.allow_require = true;
    sandbox.max_instructions_per_run = 100000;
    REQUIRE(sao_plugins_luahost_sandbox_arm(state, &sandbox) == SAO_OK);
    lua_plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_luahost_load_script(state, plugin_root.path.c_str(), "plugin.lua",
                                            "controlled_require", nullptr, &plugin) == SAO_OK);

    int32_t status = SAO_OK;
    CHECK(call_hook(plugin, "safe_value", nullptr, status) == "17");
    CHECK(status == SAO_OK);
    CHECK(call_hook(plugin, "ambient_loaded", nullptr, status) == "false");
    CHECK(status == SAO_OK);
    CHECK(call_hook(plugin, "traversal_loaded", nullptr, status) == "false");
    CHECK(status == SAO_OK);

    REQUIRE(sao_plugins_luahost_unload_script(plugin) == SAO_OK);
    REQUIRE(sao_plugins_luahost_sandbox_disarm(state) == SAO_OK);
    REQUIRE(sao_plugins_luahost_destroy(host) == SAO_OK);
}
