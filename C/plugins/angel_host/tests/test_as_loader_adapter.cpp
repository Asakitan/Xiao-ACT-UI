#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/angel_host/as_call.h"
#include "sao/plugins/angel_host/as_gpu_hunt_bind.h"
#include "sao/plugins/angel_host/as_plugin_lifecycle.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/sdk/sao_sdk.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace sao::plugins::angel_host;
using namespace sao::plugins::loader;
namespace fs = std::filesystem;

extern "C" {
SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void);
SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ashost_free_string(char* value);
SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_live_context_count(void);
}

namespace {

struct TempTree {
    fs::path root;

    explicit TempTree(const wchar_t* suffix) {
        root = fs::temp_directory_path() /
               (L"sao_ashost_adapter_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + suffix);
        std::error_code error;
        fs::remove_all(root, error);
        REQUIRE(fs::create_directories(root));
    }

    ~TempTree() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

std::string path_utf8(const fs::path& path) {
    const std::wstring wide = path.native();
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    REQUIRE(needed > 0);
    std::string result(static_cast<size_t>(needed), '\0');
    REQUIRE(WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                static_cast<int>(wide.size()), result.data(), needed, nullptr,
                nullptr) == needed);
    return result;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

plugin_manifest make_manifest(const TempTree& tree, const char* plugin_id) {
    plugin_manifest manifest;
    manifest.plugin_id = plugin_id;
    manifest.name = plugin_id;
    manifest.version = "1.0.0";
    manifest.entry = "plugin.as";
    manifest.language = engine_kind::angelscript;
    manifest.enabled = false;
    manifest.source_path = path_utf8(tree.root);
    return manifest;
}

plugin_handle_t add_plugin(const plugin_manifest& manifest) {
    plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(),
                                            &manifest, &plugin) == SAO_OK);
    REQUIRE(plugin != nullptr);
    return plugin;
}

int hook_int(as_plugin_handle_t plugin, const char* name) {
    char* result = nullptr;
    const int32_t status =
        sao_plugins_ashost_call_hook(plugin, name, nullptr, &result);
    INFO("hook " << name << " status=" << status << " result="
                 << (result == nullptr ? "(null)" : result));
    REQUIRE(status == SAO_OK);
    REQUIRE(result != nullptr);
    const int value = std::stoi(result);
    sao_plugins_ashost_free_string(result);
    return value;
}

std::string hook_json(as_plugin_handle_t plugin, const char* name,
                      const char* arguments) {
    char* result = nullptr;
    const int32_t status =
        sao_plugins_ashost_call_hook(plugin, name, arguments, &result);
    INFO("hook " << name << " status=" << status << " result="
                 << (result == nullptr ? "(null)" : result));
    REQUIRE(status == SAO_OK);
    REQUIRE(result != nullptr);
    const std::string value(result);
    sao_plugins_ashost_free_string(result);
    return value;
}

} // namespace

TEST_CASE("Angel generic loader adapter owns true lifecycle and isolates plugins",
          "[plugins][angel][loader_adapter]") {
    if (!sao_plugins_ashost_is_available()) {
        SKIP("AngelScript SDK unavailable");
    }

    const size_t contexts_before = sao_sdk_test_live_context_count();
    as_loader_adapter_owner_t owner = nullptr;
    REQUIRE(sao_plugins_ashost_register_loader_adapter(nullptr, &owner) == SAO_OK);
    REQUIRE(owner != nullptr);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 0);

    as_loader_adapter_owner_t duplicate = nullptr;
    CHECK(sao_plugins_ashost_register_loader_adapter(nullptr, &duplicate) ==
          SAO_PLUGINS_ERR_ALREADY_EXISTS);
    CHECK(duplicate == nullptr);

    TempTree first_tree(L"first");
    write_text(first_tree.root / L"plugin.as", R"AS(
int loads = 0;
int enables = 0;
int disables = 0;
int unloads = 0;
int generic_calls = 0;

void on_load(PluginContext@ ctx) {
    loads += 1;
    ctx.log("first loaded");
}
void on_enable() { enables += 1; }
void on_disable() { disables += 1; }
bool on_unload() {
    unloads += 1;
    return unloads > 1;
}
int get_loads() { return loads; }
int get_enables() { return enables; }
int get_disables() { return disables; }
int get_unloads() { return unloads; }
int generic_hook() {
    generic_calls += 1;
    return generic_calls;
}
int add(int left, int right) { return left + right; }
string echo(string value) { return value + "!"; }
int explode() {
    int zero = 0;
    return 7 / zero;
}
)AS");
    const auto first_manifest = make_manifest(first_tree, "angel_adapter_first");
    plugin_handle_t first = add_plugin(first_manifest);

    REQUIRE(sao_plugins_lifecycle_load(first) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(first) == lifecycle_state::loaded_disabled);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 1);

    plugin_context_t* first_loader_context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(first, &first_loader_context) ==
            SAO_OK);
    as_loader_adapter_plugin_lease_t first_lease = nullptr;
    REQUIRE(sao_plugins_ashost_loader_adapter_acquire_plugin(
                owner, first, &first_lease) == SAO_OK);
    REQUIRE(first_lease != nullptr);
    as_plugin_handle_t first_script =
        sao_plugins_ashost_loader_adapter_lease_script(first_lease);
    REQUIRE(first_script != nullptr);
    CHECK(sao_plugins_ashost_get_bound_context(first_script) ==
          first_loader_context);
        CHECK(sao_plugins_ashost_has_hook(first_script, "on_load"));
        CHECK_FALSE(sao_plugins_ashost_has_hook(first_script, "missing_hook"));
    CHECK(hook_int(first_script, "get_loads") == 1);
    CHECK(hook_int(first_script, "generic_hook") == 1);
        CHECK(hook_json(first_script, "add", "[-2,5]") == "3");
        CHECK(hook_json(first_script, "echo", R"(["hello"])") ==
            R"("hello!")");

        char* invalid_result = nullptr;
        CHECK(sao_plugins_ashost_call_hook(first_script, "add",
                               "[2147483648,0]", &invalid_result) ==
            SAO_ERR_INVALID_ARGUMENT);
        CHECK(invalid_result == nullptr);

    char* ignored = nullptr;
    CHECK(sao_plugins_ashost_call_hook(first_script, "explode", nullptr,
                                        &ignored) == SAO_ERR_OS_CALL_FAILED);
    CHECK(ignored == nullptr);
    CHECK(hook_int(first_script, "get_loads") == 1);

    REQUIRE(sao_plugins_lifecycle_enable(first) == SAO_OK);
    CHECK(hook_int(first_script, "get_enables") == 1);
    REQUIRE(sao_plugins_lifecycle_disable(first) == SAO_OK);
    CHECK(hook_int(first_script, "get_disables") == 1);

    TempTree second_tree(L"second");
    write_text(second_tree.root / L"plugin.as", R"AS(
int loads = 0;
void on_load(PluginContext@ ctx) {
    loads += 1;
    ctx.log("second loaded");
}
int get_loads() { return loads; }
)AS");
    const auto second_manifest = make_manifest(second_tree, "angel_adapter_second");
    plugin_handle_t second = add_plugin(second_manifest);
    REQUIRE(sao_plugins_lifecycle_load(second) == SAO_OK);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 2);

    plugin_context_t* second_loader_context = nullptr;
    REQUIRE(sao_plugins_lifecycle_get_context(second, &second_loader_context) ==
            SAO_OK);
    as_loader_adapter_plugin_lease_t second_lease = nullptr;
    REQUIRE(sao_plugins_ashost_loader_adapter_acquire_plugin(
                owner, second, &second_lease) == SAO_OK);
    REQUIRE(second_lease != nullptr);
    as_plugin_handle_t second_script =
        sao_plugins_ashost_loader_adapter_lease_script(second_lease);
    REQUIRE(second_script != nullptr);
    CHECK(first_loader_context != second_loader_context);
    CHECK(sao_plugins_ashost_get_bound_context(second_script) ==
          second_loader_context);
    CHECK(hook_int(second_script, "get_loads") == 1);

    SaoSdkContext* first_sdk =
        sao_plugins_ashost_loader_adapter_lease_sdk_context(first_lease);
    SaoSdkContext* second_sdk =
        sao_plugins_ashost_loader_adapter_lease_sdk_context(second_lease);
    REQUIRE(first_sdk != nullptr);
    REQUIRE(second_sdk != nullptr);
    CHECK(first_sdk != second_sdk);
    CHECK(first_sdk->ctx_impl != second_sdk->ctx_impl);
    CHECK(std::string(first_sdk->plugin_id_utf8) == "angel_adapter_first");
    CHECK(std::string(second_sdk->plugin_id_utf8) == "angel_adapter_second");

    asIScriptEngine* first_engine = sao_plugins_ashost_get_engine(first_script);
    asIScriptEngine* second_engine = sao_plugins_ashost_get_engine(second_script);
    REQUIRE(first_engine != nullptr);
    REQUIRE(second_engine != nullptr);
    CHECK(first_engine != second_engine);
    CHECK(sao_plugins_ashost_get_module(first_script) !=
          sao_plugins_ashost_get_module(second_script));
    CHECK(gpu_hunt_binding_context(first_engine) == first_sdk);
    CHECK(gpu_hunt_binding_context(second_engine) == second_sdk);
    CHECK(sao_sdk_test_live_context_count() == contexts_before + 2);

    CHECK(sao_plugins_ashost_unregister_loader_adapter(owner) ==
          SAO_PLUGINS_ERR_BUSY);
        CHECK(sao_plugins_lifecycle_unload(second) == SAO_PLUGINS_ERR_BUSY);
        REQUIRE(sao_plugins_ashost_loader_adapter_release_plugin(second_lease) ==
            SAO_OK);
        second_lease = nullptr;
    REQUIRE(sao_plugins_lifecycle_unload(second) == SAO_OK);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 1);

    CHECK(sao_plugins_lifecycle_unload(first) == SAO_PLUGINS_ERR_BUSY);
    REQUIRE(sao_plugins_lifecycle_state(first) == lifecycle_state::loaded_disabled);
    CHECK(hook_int(first_script, "get_unloads") == 1);
        REQUIRE(sao_plugins_ashost_loader_adapter_release_plugin(first_lease) ==
            SAO_OK);
        first_lease = nullptr;
    REQUIRE(sao_plugins_lifecycle_unload(first) == SAO_OK);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 0);
    CHECK(sao_sdk_test_live_context_count() == contexts_before);

    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), second) ==
            SAO_OK);
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), first) ==
            SAO_OK);

    TempTree failed_tree(L"failed");
    write_text(failed_tree.root / L"plugin.as", R"AS(
void on_load(PluginContext@ ctx) {
    int zero = 0;
    int value = 1 / zero;
}
void on_unload() {}
)AS");
    const auto failed_manifest = make_manifest(failed_tree, "angel_adapter_failed");
    plugin_handle_t failed = add_plugin(failed_manifest);
    CHECK(sao_plugins_lifecycle_load(failed) == SAO_ERR_OS_CALL_FAILED);
    CHECK(sao_plugins_lifecycle_state(failed) == lifecycle_state::failed);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 0);
    CHECK(sao_sdk_test_live_context_count() == contexts_before);
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), failed) ==
            SAO_OK);

    TempTree invalid_signature_tree(L"invalid_signature");
    write_text(invalid_signature_tree.root / L"plugin.as", R"AS(
void on_load(string value) {}
)AS");
    const auto invalid_signature_manifest =
        make_manifest(invalid_signature_tree, "angel_adapter_invalid_signature");
    plugin_handle_t invalid_signature = add_plugin(invalid_signature_manifest);
    CHECK(sao_plugins_lifecycle_load(invalid_signature) ==
          SAO_ERR_INVALID_ARGUMENT);
    CHECK(sao_plugins_lifecycle_state(invalid_signature) ==
          lifecycle_state::failed);
    CHECK(sao_plugins_ashost_loader_adapter_plugin_count(owner) == 0);
    CHECK(sao_sdk_test_live_context_count() == contexts_before);
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(),
                                        invalid_signature) == SAO_OK);

    REQUIRE(sao_plugins_ashost_unregister_loader_adapter(owner) == SAO_OK);

    as_loader_adapter_owner_t replacement = nullptr;
    REQUIRE(sao_plugins_ashost_register_loader_adapter(nullptr, &replacement) ==
            SAO_OK);
    REQUIRE(sao_plugins_ashost_unregister_loader_adapter(replacement) == SAO_OK);
    CHECK(sao_plugins_ashost_unregister_loader_adapter(replacement) ==
          SAO_ERR_HANDLE_INVALID);

    TempTree legacy_tree(L"legacy");
    write_text(legacy_tree.root / L"plugin.json",
               R"({"id":"angel_legacy_compat","entry":"plugin.as"})");
    write_text(legacy_tree.root / L"plugin.as", R"AS(
void on_load() {}
void on_unload() {}
string echo(string value) { return value + "!"; }
)AS");
    as_host_handle_t legacy_host = nullptr;
    REQUIRE(sao_plugins_ashost_create(nullptr, &legacy_host) == SAO_OK);
    REQUIRE(legacy_host != nullptr);
    as_plugin_handle_t legacy_script = nullptr;
    char* legacy_error = nullptr;
    const std::string legacy_manifest =
        path_utf8(legacy_tree.root / L"plugin.json");
    const int32_t legacy_load_status = sao_plugins_ashost_load_plugin(
        legacy_host, legacy_manifest.c_str(), &legacy_script, &legacy_error);
    const std::string legacy_error_text =
        legacy_error == nullptr ? std::string{} : legacy_error;
    sao_plugins_ashost_free_string(legacy_error);
    INFO("legacy load status=" << legacy_load_status
                                << " error=" << legacy_error_text);
    REQUIRE(legacy_load_status == SAO_OK);
    REQUIRE(legacy_script != nullptr);
    CHECK(hook_json(legacy_script, "echo", R"(["legacy"])") ==
          R"("legacy!")");
    REQUIRE(sao_plugins_ashost_unload_plugin(legacy_script, nullptr) == SAO_OK);
    REQUIRE(sao_plugins_ashost_destroy(legacy_host) == SAO_OK);
}
