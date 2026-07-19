#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sao::plugins::loader;
namespace fs = std::filesystem;

namespace {

struct dependency_probe;

struct dependency_provider_session {
    dependency_probe* owner = nullptr;
    deps_session_t loader_session = nullptr;
    std::string plugin_id;
};

struct dependency_probe {
    int retain_calls = 0;
    int release_calls = 0;
    int create_calls = 0;
    int attach_calls = 0;
    int restore_calls = 0;
    int close_calls = 0;
    int fail_attach_call = 0;
    int32_t create_status = SAO_OK;
    int32_t restore_status = SAO_OK;
    int32_t close_status = SAO_OK;
    bool throw_attach = false;
    bool throw_release = false;
    bool reenter_restore = false;
    int32_t nested_restore_status = SAO_ERR_NOT_INITIALIZED;
    dependency_provider_session* latest_session = nullptr;
    std::vector<std::string> calls;
};

std::string path_name(const wchar_t* path) {
    return fs::path(path).filename().string();
}

void SAO_PLUGINS_CALL provider_retain(void* user_data) {
    ++static_cast<dependency_probe*>(user_data)->retain_calls;
}

void SAO_PLUGINS_CALL provider_release(void* user_data) {
    auto* probe = static_cast<dependency_probe*>(user_data);
    ++probe->release_calls;
    if (probe->throw_release)
        throw std::runtime_error("fixture release failure");
}

int32_t SAO_PLUGINS_CALL provider_create_session(void* user_data, const deps_session_spec* spec,
                                                 void** out_provider_session) {
    if (user_data == nullptr || spec == nullptr || out_provider_session == nullptr ||
        spec->struct_size < sizeof(deps_session_spec) || spec->plugin_id_utf8 == nullptr ||
        spec->plugin_dir == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    auto* probe = static_cast<dependency_probe*>(user_data);
    ++probe->create_calls;
    probe->calls.push_back("deps.create:" + std::string(spec->plugin_id_utf8));
    if (probe->create_status != SAO_OK)
        return probe->create_status;
    auto session = std::make_unique<dependency_provider_session>();
    session->owner = probe;
    session->plugin_id = spec->plugin_id_utf8;
    probe->latest_session = session.get();
    *out_provider_session = session.release();
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_attach_path(void*, void* provider_session,
                                              const wchar_t* absolute_dir) {
    if (provider_session == nullptr || absolute_dir == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* session = static_cast<dependency_provider_session*>(provider_session);
    auto* probe = session->owner;
    ++probe->attach_calls;
    probe->calls.push_back("deps.attach:" + path_name(absolute_dir));
    if (probe->throw_attach)
        throw std::runtime_error("fixture attach failure");
    if (probe->fail_attach_call == probe->attach_calls)
        return SAO_ERR_OS_CALL_FAILED;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL provider_restore_path(void*, void* provider_session,
                                               const wchar_t* absolute_dir) {
    if (provider_session == nullptr || absolute_dir == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* session = static_cast<dependency_provider_session*>(provider_session);
    auto* probe = session->owner;
    ++probe->restore_calls;
    probe->calls.push_back("deps.restore:" + path_name(absolute_dir));
    if (probe->reenter_restore) {
        probe->reenter_restore = false;
        probe->nested_restore_status = sao_plugins_deps_session_restore(session->loader_session);
    }
    return probe->restore_status;
}

int32_t SAO_PLUGINS_CALL provider_close_session(void*, void* provider_session) {
    if (provider_session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    auto* session = static_cast<dependency_provider_session*>(provider_session);
    auto* probe = session->owner;
    ++probe->close_calls;
    probe->calls.push_back("deps.close:" + session->plugin_id);
    if (probe->close_status != SAO_OK)
        return probe->close_status;
    probe->latest_session = nullptr;
    delete session;
    return SAO_OK;
}

deps_provider make_provider(dependency_probe& probe) {
    deps_provider provider{};
    provider.abi_version = SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &probe;
    provider.retain = provider_retain;
    provider.release = provider_release;
    provider.create_session = provider_create_session;
    provider.attach_path = provider_attach_path;
    provider.restore_path = provider_restore_path;
    provider.close_session = provider_close_session;
    return provider;
}

struct TempDirectory {
    fs::path path;

    explicit TempDirectory(const wchar_t* label) {
        wchar_t base[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, base) > 0);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::path(base) /
               (std::wstring(L"sao_loader_deps_") + label + L"_" + std::to_wstring(stamp));
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

std::string path_utf8(const fs::path& path) {
    const auto wide = path.native();
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

plugin_handle_t add_lifecycle_plugin(const TempDirectory& temp, const char* plugin_id) {
    plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.cs";
    manifest.language = engine_kind::csharp;
    manifest.source_path = path_utf8(temp.path);
    manifest.abi_version = 2;
    plugin_handle_t handle = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &handle) ==
            SAO_OK);
    return handle;
}

int32_t SAO_PLUGINS_CALL adapter_load(plugin_handle_t, const plugin_manifest*, void* user_data) {
    static_cast<dependency_probe*>(user_data)->calls.push_back("host.load");
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL adapter_simple(plugin_handle_t, void*) {
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(plugin_handle_t, bool* allow, void*) {
    *allow = true;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL adapter_unload(plugin_handle_t, void* user_data) {
    static_cast<dependency_probe*>(user_data)->calls.push_back("host.unload");
    return SAO_OK;
}

host_adapter_vtable make_adapter(dependency_probe& probe) {
    host_adapter_vtable adapter{};
    adapter.load_plugin = adapter_load;
    adapter.call_on_load = adapter_simple;
    adapter.call_on_enable = adapter_simple;
    adapter.call_on_disable = adapter_simple;
    adapter.call_on_unload = adapter_on_unload;
    adapter.unload_plugin = adapter_unload;
    adapter.host_user_data = &probe;
    return adapter;
}

size_t call_index(const dependency_probe& probe, const std::string& call) {
    const auto found = std::find(probe.calls.begin(), probe.calls.end(), call);
    REQUIRE(found != probe.calls.end());
    return static_cast<size_t>(std::distance(probe.calls.begin(), found));
}

} // namespace

TEST_CASE("dependency attach fails closed without a host owner",
          "[plugins][loader][deps][provider]") {
    deps_bootstrap_record record;
    record.added_paths = {L"C:\\fixture\\libs"};
    deps_session_t session = reinterpret_cast<deps_session_t>(1);
    CHECK(sao_plugins_deps_attach("missing_owner", L"C:\\fixture", &record, &session) ==
          SAO_PLUGINS_ERR_UNSUPPORTED);
    CHECK(session == nullptr);
}

TEST_CASE("dependency attach rolls back paths in reverse order and blocks unregister",
          "[plugins][loader][deps][provider][rollback]") {
    dependency_probe probe;
    probe.fail_attach_call = 3;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_deps_register_provider(&provider) == SAO_OK);

    deps_bootstrap_record record;
    record.added_paths = {L"C:\\fixture\\libs", L"C:\\fixture\\vendor", L"C:\\fixture\\engine"};
    deps_session_t session = nullptr;
    CHECK(sao_plugins_deps_attach("rollback", L"C:\\fixture", &record, &session) ==
          SAO_ERR_OS_CALL_FAILED);
    CHECK(session == nullptr);
    CHECK(probe.calls == std::vector<std::string>{"deps.create:rollback", "deps.attach:libs",
                                                  "deps.attach:vendor", "deps.attach:engine",
                                                  "deps.restore:engine", "deps.restore:vendor",
                                                  "deps.restore:libs", "deps.close:rollback"});
    CHECK(probe.retain_calls == 1);
    CHECK(probe.release_calls == 1);
    REQUIRE(sao_plugins_deps_unregister_provider() == SAO_OK);
}

TEST_CASE("dependency session retains failed restore and close work for retry and reentry",
          "[plugins][loader][deps][provider][retry][reentry]") {
    dependency_probe probe;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_deps_register_provider(&provider) == SAO_OK);

    deps_bootstrap_record record;
    record.added_paths = {L"C:\\fixture\\libs", L"C:\\fixture\\vendor"};
    deps_session_t session = nullptr;
    REQUIRE(sao_plugins_deps_attach("retry", L"C:\\fixture", &record, &session) == SAO_OK);
    REQUIRE(session != nullptr);
    REQUIRE(probe.latest_session != nullptr);
    probe.latest_session->loader_session = session;
    CHECK(sao_plugins_deps_unregister_provider() == SAO_PLUGINS_ERR_BUSY);

    probe.restore_status = SAO_ERR_OS_CALL_FAILED;
    CHECK(sao_plugins_deps_session_restore(session) == SAO_ERR_OS_CALL_FAILED);
    probe.restore_status = SAO_OK;
    probe.reenter_restore = true;
    probe.close_status = SAO_ERR_OS_CALL_FAILED;
    CHECK(sao_plugins_deps_session_close(session) == SAO_ERR_OS_CALL_FAILED);
    CHECK(probe.nested_restore_status == SAO_PLUGINS_ERR_BUSY);
    CHECK(sao_plugins_deps_unregister_provider() == SAO_PLUGINS_ERR_BUSY);
    probe.close_status = SAO_OK;
    REQUIRE(sao_plugins_deps_session_close(session) == SAO_OK);
    CHECK(sao_plugins_deps_session_close(session) == SAO_ERR_HANDLE_INVALID);
    CHECK(probe.retain_calls == 1);
    CHECK(probe.release_calls == 1);
    REQUIRE(sao_plugins_deps_unregister_provider() == SAO_OK);
}

TEST_CASE("dependency provider exceptions are contained by transactional attach",
          "[plugins][loader][deps][provider][throw-barrier]") {
    dependency_probe probe;
    probe.throw_attach = true;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_deps_register_provider(&provider) == SAO_OK);

    deps_bootstrap_record record;
    record.added_paths = {L"C:\\fixture\\libs"};
    deps_session_t session = nullptr;
    CHECK(sao_plugins_deps_attach("throw", L"C:\\fixture", &record, &session) ==
          SAO_ERR_OS_CALL_FAILED);
    CHECK(session == nullptr);
    CHECK(probe.calls == std::vector<std::string>{"deps.create:throw", "deps.attach:libs",
                                                  "deps.restore:libs", "deps.close:throw"});
    CHECK(probe.close_calls == 1);
    CHECK(probe.release_calls == 1);
    REQUIRE(sao_plugins_deps_unregister_provider() == SAO_OK);
}

TEST_CASE("dependency create rollback retains ownership when provider release throws",
          "[plugins][loader][deps][provider][retry][throw-barrier]") {
    dependency_probe probe;
    probe.create_status = SAO_ERR_OS_CALL_FAILED;
    probe.throw_release = true;
    auto provider = make_provider(probe);
    REQUIRE(sao_plugins_deps_register_provider(&provider) == SAO_OK);

    deps_bootstrap_record record;
    record.added_paths = {L"C:\\fixture\\libs"};
    deps_session_t session = nullptr;
    CHECK(sao_plugins_deps_attach("release_throw", L"C:\\fixture", &record, &session) ==
          SAO_ERR_OS_CALL_FAILED);
    REQUIRE(session != nullptr);
    CHECK(sao_plugins_deps_unregister_provider() == SAO_PLUGINS_ERR_BUSY);
    probe.throw_release = false;
    REQUIRE(sao_plugins_deps_session_close(session) == SAO_OK);
    CHECK(probe.close_calls == 0);
    CHECK(probe.release_calls == 2);
    REQUIRE(sao_plugins_deps_unregister_provider() == SAO_OK);
}

TEST_CASE("loader lifecycle attaches dependencies before host load and closes them last",
          "[plugins][loader][deps][provider][lifecycle]") {
    TempDirectory temp(L"lifecycle");
    fs::create_directories(temp.path / L"libs");
    write_text(temp.path / L"plugin.emma", "entry");

    dependency_probe probe;
    auto provider = make_provider(probe);
    auto adapter = make_adapter(probe);
    REQUIRE(sao_plugins_deps_register_provider(&provider) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_register_host_adapter(engine_kind::csharp, &adapter) == SAO_OK);
    auto handle = add_lifecycle_plugin(temp, "deps_lifecycle");

    REQUIRE(sao_plugins_lifecycle_load(handle) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_unload(handle) == SAO_OK);
    CHECK(call_index(probe, "deps.attach:libs") < call_index(probe, "host.load"));
    CHECK(call_index(probe, "host.unload") < call_index(probe, "deps.restore:libs"));
    CHECK(call_index(probe, "deps.restore:libs") < call_index(probe, "deps.close:deps_lifecycle"));

    REQUIRE(sao_plugins_lifecycle_unregister_host_adapter(engine_kind::csharp) == SAO_OK);
    REQUIRE(sao_plugins_deps_unregister_provider() == SAO_OK);
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), handle) == SAO_OK);
}
