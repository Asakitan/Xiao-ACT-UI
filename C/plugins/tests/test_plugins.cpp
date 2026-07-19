#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao_plugins/sao_plugins.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace sao::plugins::loader;

#ifndef SAO_TEST_NATIVE_PLUGIN_PATH
#define SAO_TEST_NATIVE_PLUGIN_PATH L""
#endif

namespace {

struct TempDirectory {
    fs::path path;

    explicit TempDirectory(const wchar_t* label) {
        wchar_t base[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, base) > 0);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::path(base) /
               (std::wstring(L"sao_legacy_loader_") + label + L"_" + std::to_wstring(stamp));
        REQUIRE(fs::create_directories(path));
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

fs::path prepare_native_plugin(TempDirectory& temp, std::string_view plugin_id) {
    const fs::path fixture = SAO_TEST_NATIVE_PLUGIN_PATH;
    REQUIRE(fs::is_regular_file(fixture));
    const auto native_path = temp.path / L"native_fixture.dll";
    REQUIRE(CopyFileW(fixture.c_str(), native_path.c_str(), FALSE) == TRUE);
    write_text(temp.path / L"plugin.emma", "entry");
    write_text(temp.path / L"plugin.json",
               std::string{"{\"id\":\""} + std::string(plugin_id) +
                   "\",\"name\":\"Legacy facade\",\"version\":\"1.0.0\","
                   "\"entry\":\"plugin.emma\",\"language\":\"emma\","
                   "\"enabled\":true,\"abi_version\":2,"
                   "\"native_entry\":\"native_fixture.dll\","
                   "\"native_abi\":\"sao_plugin_v2\","
                   "\"capabilities\":[\"native_test\"]}");
    return native_path;
}

struct facade_platform_probe;

struct facade_platform_session {
    facade_platform_probe* owner = nullptr;
};

struct facade_platform_probe {
    std::mutex mutex;
    std::condition_variable condition;
    bool block_quiesce = false;
    bool quiesce_entered = false;
    bool release_quiesce = false;
    bool throw_release = false;
    int retain_calls = 0;
    int release_calls = 0;
    int create_calls = 0;
    int quiesce_calls = 0;
    int destroy_calls = 0;
};

void SAO_PLUGINS_CALL facade_provider_retain(void* user_data) {
    ++static_cast<facade_platform_probe*>(user_data)->retain_calls;
}

void SAO_PLUGINS_CALL facade_provider_release(void* user_data) {
    auto* probe = static_cast<facade_platform_probe*>(user_data);
    ++probe->release_calls;
    if (probe->throw_release)
        throw std::runtime_error("fixture release failure");
}

int32_t SAO_PLUGINS_CALL facade_provider_create(void* user_data,
                                                const plugin_context_platform_session_spec*,
                                                plugin_context_platform_session_t* out_session) {
    auto* probe = static_cast<facade_platform_probe*>(user_data);
    ++probe->create_calls;
    *out_session = new facade_platform_session{probe};
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL facade_provider_quiesce(void*, plugin_context_platform_session_t session) {
    auto* probe = static_cast<facade_platform_session*>(session)->owner;
    std::unique_lock lock(probe->mutex);
    ++probe->quiesce_calls;
    if (probe->block_quiesce) {
        probe->quiesce_entered = true;
        probe->condition.notify_all();
        probe->condition.wait(lock, [probe] { return probe->release_quiesce; });
    }
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL facade_provider_destroy(void*, plugin_context_platform_session_t session) {
    auto owned =
        std::unique_ptr<facade_platform_session>(static_cast<facade_platform_session*>(session));
    ++owned->owner->destroy_calls;
    return SAO_OK;
}

plugin_context_platform_provider make_facade_provider(facade_platform_probe& probe) {
    plugin_context_platform_provider provider{};
    provider.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &probe;
    provider.retain = facade_provider_retain;
    provider.release = facade_provider_release;
    provider.create_session = facade_provider_create;
    provider.quiesce_session = facade_provider_quiesce;
    provider.destroy_session = facade_provider_destroy;
    return provider;
}

} // namespace

#if !defined(SAO_TEST_LEGACY_RUNTIME_FACADE_ONLY)
TEST_CASE("sao_plugins_abi_version reports 1.0", "[abi]") {
    REQUIRE(sao_plugins_abi_version() == 0x00010000u);
}

TEST_CASE("Lua runtime create/destroy round-trip", "[lua]") {
    sao_plugins_lua_handle_t handle = nullptr;
    REQUIRE(sao_plugins_lua_create(&handle) == SAO_OK);
    REQUIRE(handle != nullptr);
    sao_plugins_lua_destroy(handle);
}

TEST_CASE("Lua runtime create rejects a null out-handle", "[lua]") {
    REQUIRE(sao_plugins_lua_create(nullptr) == SAO_ERR_INVALID_ARGUMENT);
}
#endif

TEST_CASE("legacy loader validates arguments and ABI status mapping", "[loader][compat]") {
    sao_plugins_native_handle_t handle = reinterpret_cast<sao_plugins_native_handle_t>(1);
    REQUIRE(sao_plugins_native_load(nullptr, 2, &handle) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
    REQUIRE(sao_plugins_native_load(L"", 2, &handle) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_native_load(L"missing-plugin.dll", 2, &handle) == SAO_ERR_HANDLE_INVALID);

    TempDirectory temp(L"abi");
    prepare_native_plugin(temp, "legacy_facade_abi_case");
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 1, &handle) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 0x00020001U, &handle) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
}

TEST_CASE("legacy loader maps descriptor and dependency failures", "[loader][compat][status]") {
    TempDirectory version_temp(L"version");
    prepare_native_plugin(version_temp, "legacy_facade_version_case");
    write_text(
        version_temp.path / L"plugin.json",
        R"({"id":"legacy_facade_version_case","name":"Version","version":"2.0.0","entry":"plugin.emma","language":"emma","abi_version":2,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2"})");
    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(version_temp.path.c_str(), 2, &handle) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    TempDirectory capability_temp(L"capability");
    prepare_native_plugin(capability_temp, "legacy_facade_capability_case");
    write_text(
        capability_temp.path / L"plugin.json",
        R"({"id":"legacy_facade_capability_case","name":"Capability","version":"1.0.0","entry":"plugin.emma","language":"emma","abi_version":2,"native_entry":"native_fixture.dll","native_abi":"sao_plugin_v2","capabilities":["missing"]})");
    REQUIRE(sao_plugins_native_load(capability_temp.path.c_str(), 2, &handle) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    TempDirectory dependency_temp(L"dependency");
    write_text(dependency_temp.path / L"plugin.emma", "entry");
    write_text(
        dependency_temp.path / L"plugin.json",
        R"({"id":"legacy_facade_dependency_case","name":"Dependency","version":"1.0.0","entry":"plugin.emma","language":"emma","abi_version":2,"requires":["missing_dependency"]})");
    REQUIRE(sao_plugins_native_load(dependency_temp.path.c_str(), 2, &handle) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(handle == nullptr);
}

TEST_CASE("legacy loader accepts directory descriptor and DLL paths idempotently",
          "[loader][compat][native]") {
    TempDirectory temp(L"native");
    const auto native_path = prepare_native_plugin(temp, "legacy_facade_native_case");

    sao_plugins_native_handle_t directory_handle = nullptr;
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &directory_handle) == SAO_OK);
    REQUIRE(directory_handle != nullptr);
    REQUIRE(GetModuleHandleW(native_path.c_str()) != nullptr);

    sao_plugins_native_handle_t descriptor_handle = nullptr;
    REQUIRE(sao_plugins_native_load((temp.path / L"plugin.json").c_str(), 0x00020000U,
                                    &descriptor_handle) == SAO_OK);
    REQUIRE(descriptor_handle == directory_handle);

    sao_plugins_native_handle_t dll_handle = nullptr;
    REQUIRE(sao_plugins_native_load(native_path.c_str(), 2, &dll_handle) == SAO_OK);
    REQUIRE(dll_handle == directory_handle);

    sao_plugins_native_unload(directory_handle);
    REQUIRE(GetModuleHandleW(native_path.c_str()) == nullptr);
    sao_plugins_native_unload(descriptor_handle);
    sao_plugins_native_unload(nullptr);

    sao_plugins_native_handle_t reloaded = nullptr;
    REQUIRE(sao_plugins_native_load(native_path.c_str(), 2, &reloaded) == SAO_OK);
    REQUIRE(reloaded != nullptr);
    REQUIRE(reloaded != directory_handle);
    sao_plugins_native_unload(reloaded);
    REQUIRE(GetModuleHandleW(native_path.c_str()) == nullptr);
}

TEST_CASE("legacy loader exposes unload status and retained diagnostics",
          "[loader][compat][status][unload]") {
    TempDirectory temp(L"unload_status");
    const auto native_path = prepare_native_plugin(temp, "legacy_facade_unload_status");

    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &handle) == SAO_OK);
    REQUIRE(handle != nullptr);
    int32_t last_status = SAO_ERR_NOT_INITIALIZED;
    REQUIRE(sao_plugins_native_last_status(handle, &last_status) == SAO_OK);
    CHECK(last_status == SAO_OK);

    const HMODULE control =
        LoadLibraryExW(native_path.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    REQUIRE(control != nullptr);
    using set_lifecycle_statuses_fn = int32_t(SAO_PLUGINS_CALL*)(int32_t, int32_t);
    const auto set_lifecycle_statuses = reinterpret_cast<set_lifecycle_statuses_fn>(
        GetProcAddress(control, "sao_test_plugin_set_lifecycle_statuses"));
    REQUIRE(set_lifecycle_statuses != nullptr);
    REQUIRE(set_lifecycle_statuses(SAO_OK, SAO_ERR_OS_CALL_FAILED) == SAO_OK);

    CHECK(sao_plugins_legacy_native_unload_status(handle) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(sao_plugins_native_last_status(handle, &last_status) == SAO_OK);
    CHECK(last_status == SAO_ERR_OS_CALL_FAILED);
    size_t required = 0;
    CHECK(sao_plugins_native_last_error(handle, nullptr, 0, &required) == SAO_ERR_BUFFER_TOO_SMALL);
    REQUIRE(required > 1);
    std::string error(required, '\0');
    REQUIRE(sao_plugins_native_last_error(handle, error.data(), error.size(), &required) == SAO_OK);
    CHECK(std::strstr(error.c_str(), "lifecycle unload failed") != nullptr);

    REQUIRE(set_lifecycle_statuses(SAO_OK, SAO_OK) == SAO_OK);
    REQUIRE(FreeLibrary(control) == TRUE);
    sao_plugins_native_unload(handle);
    CHECK(GetModuleHandleW(native_path.c_str()) == nullptr);
    CHECK(sao_plugins_native_last_status(handle, &last_status) == SAO_ERR_HANDLE_INVALID);
}

TEST_CASE("legacy loader rejects a DLL not owned by its descriptor", "[loader][compat]") {
    TempDirectory temp(L"ownership");
    prepare_native_plugin(temp, "legacy_facade_ownership_case");
    const auto other = temp.path / L"other.dll";
    REQUIRE(CopyFileW(SAO_TEST_NATIVE_PLUGIN_PATH, other.c_str(), FALSE) == TRUE);

    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(other.c_str(), 2, &handle) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
}

TEST_CASE("legacy loader fails closed when no script host provider exists",
          "[loader][compat][provider]") {
    TempDirectory temp(L"provider");
    write_text(temp.path / L"plugin.emma", "entry");
    write_text(
        temp.path / L"plugin.json",
        R"({"id":"legacy_facade_provider_case","name":"Provider","version":"1.0.0","entry":"plugin.emma","language":"emma","abi_version":2})");

    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &handle) == SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE(handle == nullptr);
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &handle) == SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE(handle == nullptr);
}

TEST_CASE("legacy facade retains a failed load when registry cleanup is busy",
          "[loader][compat][failure][ownership][retry]") {
    using namespace sao::plugins::loader;
    TempDirectory temp(L"failed_load_cleanup");
    const auto native_path = prepare_native_plugin(temp, "legacy_facade_failed_cleanup");
    facade_platform_probe probe;
    probe.throw_release = true;
    auto provider = make_facade_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    const HMODULE control =
        LoadLibraryExW(native_path.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    REQUIRE(control != nullptr);
    using set_lifecycle_statuses_fn = int32_t(SAO_PLUGINS_CALL*)(int32_t, int32_t);
    const auto set_lifecycle_statuses = reinterpret_cast<set_lifecycle_statuses_fn>(
        GetProcAddress(control, "sao_test_plugin_set_lifecycle_statuses"));
    REQUIRE(set_lifecycle_statuses != nullptr);
    REQUIRE(set_lifecycle_statuses(SAO_ERR_OS_CALL_FAILED, SAO_OK) == SAO_OK);

    sao_plugins_native_handle_t handle = nullptr;
    CHECK(sao_plugins_native_load(temp.path.c_str(), 2, &handle) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(handle != nullptr);
    CHECK(probe.destroy_calls == 1);
    CHECK(probe.release_calls == 1);
    int32_t last_status = SAO_OK;
    REQUIRE(sao_plugins_native_last_status(handle, &last_status) == SAO_OK);
    CHECK(last_status == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_ctx_unregister_platform_provider() == SAO_PLUGINS_ERR_BUSY);

    probe.throw_release = false;
    REQUIRE(sao_plugins_legacy_native_unload_status(handle) == SAO_OK);
    CHECK(probe.destroy_calls == 1);
    CHECK(probe.release_calls == 2);
    CHECK(sao_plugins_native_last_status(handle, &last_status) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
    REQUIRE(set_lifecycle_statuses(SAO_OK, SAO_OK) == SAO_OK);
    REQUIRE(FreeLibrary(control) == TRUE);
}

TEST_CASE("concurrent facade status unload reports busy instead of success",
          "[loader][compat][unload][concurrency]") {
    using namespace sao::plugins::loader;
    TempDirectory temp(L"concurrent_unload");
    prepare_native_plugin(temp, "legacy_facade_concurrent_unload");
    facade_platform_probe probe;
    auto provider = make_facade_provider(probe);
    REQUIRE(sao_plugins_ctx_register_platform_provider(&provider) == SAO_OK);

    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &handle) == SAO_OK);
    REQUIRE(handle != nullptr);
    {
        std::lock_guard lock(probe.mutex);
        probe.block_quiesce = true;
    }
    int32_t first_status = SAO_ERR_OS_CALL_FAILED;
    std::jthread first([&] { first_status = sao_plugins_legacy_native_unload_status(handle); });
    {
        std::unique_lock lock(probe.mutex);
        REQUIRE(probe.condition.wait_for(lock, std::chrono::seconds(2),
                                         [&probe] { return probe.quiesce_entered; }));
    }

    CHECK(sao_plugins_legacy_native_unload_status(handle) == SAO_PLUGINS_ERR_BUSY);
    {
        std::lock_guard lock(probe.mutex);
        probe.release_quiesce = true;
    }
    probe.condition.notify_all();
    first.join();
    CHECK(first_status == SAO_OK);
    REQUIRE(sao_plugins_ctx_unregister_platform_provider() == SAO_OK);
}
