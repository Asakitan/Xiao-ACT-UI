#include <catch2/catch_test_macros.hpp>

#include "sao_plugins/sao_plugins.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

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
               (std::wstring(L"sao_legacy_loader_") + label + L"_" +
                std::to_wstring(stamp));
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
    write_text(
        temp.path / L"plugin.json",
        std::string{"{\"id\":\""} + std::string(plugin_id) +
            "\",\"name\":\"Legacy facade\",\"version\":\"1.0.0\","
            "\"entry\":\"plugin.emma\",\"language\":\"emma\","
            "\"enabled\":true,\"abi_version\":2,"
            "\"native_entry\":\"native_fixture.dll\","
            "\"native_abi\":\"sao_plugin_v2\","
            "\"capabilities\":[\"native_test\"]}");
    return native_path;
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
    REQUIRE(sao_plugins_native_load(L"missing-plugin.dll", 2, &handle) ==
        SAO_ERR_HANDLE_INVALID);

    TempDirectory temp(L"abi");
    prepare_native_plugin(temp, "legacy_facade_abi_case");
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 1, &handle) ==
        SAO_ERR_INVALID_ARGUMENT);
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
    REQUIRE(sao_plugins_native_load(
        (temp.path / L"plugin.json").c_str(), 0x00020000U,
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

TEST_CASE("legacy loader rejects a DLL not owned by its descriptor", "[loader][compat]") {
    TempDirectory temp(L"ownership");
    prepare_native_plugin(temp, "legacy_facade_ownership_case");
    const auto other = temp.path / L"other.dll";
    REQUIRE(CopyFileW(SAO_TEST_NATIVE_PLUGIN_PATH, other.c_str(), FALSE) == TRUE);

    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(other.c_str(), 2, &handle) ==
        SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);
}

TEST_CASE("legacy loader fails closed when no script host provider exists",
      "[loader][compat][provider]") {
    TempDirectory temp(L"provider");
    write_text(temp.path / L"plugin.emma", "entry");
    write_text(temp.path / L"plugin.json",
           R"({"id":"legacy_facade_provider_case","name":"Provider","version":"1.0.0","entry":"plugin.emma","language":"emma","abi_version":2})");

    sao_plugins_native_handle_t handle = nullptr;
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &handle) ==
        SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE(handle == nullptr);
    REQUIRE(sao_plugins_native_load(temp.path.c_str(), 2, &handle) ==
        SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE(handle == nullptr);
}
