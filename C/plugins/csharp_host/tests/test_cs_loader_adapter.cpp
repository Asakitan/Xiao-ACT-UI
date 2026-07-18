#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/csharp_host/cs_host.h"
#include "sao/plugins/csharp_host/cs_loader_adapter.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_registry.h"

#include <filesystem>
#include <string>

using namespace sao::plugins::csharp_host;
using namespace sao::plugins::loader;
namespace fs = std::filesystem;

#ifndef SAO_HELLO_CSHARP_DIR
#define SAO_HELLO_CSHARP_DIR "plugins/examples/hello_csharp"
#endif

namespace {

std::string path_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

plugin_manifest hello_manifest(std::string id) {
    plugin_manifest manifest;
    manifest.plugin_id = std::move(id);
    manifest.name = "Generic C# fixture";
    manifest.version = "1.0.0";
    manifest.entry = "prebuilt/HelloPlugin.dll";
    manifest.managed_type = "SaoAuto.Plugins.HelloCsharp.HelloPlugin, HelloPlugin";
    manifest.runtimeconfig = "prebuilt/HelloPlugin.runtimeconfig.json";
    manifest.language = engine_kind::csharp;
    manifest.source_path = path_utf8(fs::path(SAO_HELLO_CSHARP_DIR));
    return manifest;
}

plugin_handle_t add_plugin(const plugin_manifest& manifest) {
    plugin_handle_t plugin = nullptr;
    REQUIRE(sao_plugins_registry_add_plugin(sao_plugins_registry_instance(), &manifest, &plugin) ==
            SAO_OK);
    REQUIRE(plugin != nullptr);
    return plugin;
}

void remove_plugin(plugin_handle_t plugin) {
    REQUIRE(sao_plugins_registry_remove(sao_plugins_registry_instance(), plugin) == SAO_OK);
}

bool generic_fixture_available(std::string& reason) {
    bool hostfxr = false;
    REQUIRE(sao_plugins_cshost_is_available(&hostfxr) == SAO_OK);
    if (!hostfxr) {
        reason = "hostfxr runtime capability missing";
        return false;
    }
    const fs::path root(SAO_HELLO_CSHARP_DIR);
    if (!fs::is_regular_file(root / L"prebuilt" / L"HelloPlugin.dll")) {
        reason = "precompiled managed fixture missing";
        return false;
    }
    if (!fs::is_regular_file(root / L"prebuilt" / L"HelloPlugin.runtimeconfig.json")) {
        reason = "managed fixture runtimeconfig missing";
        return false;
    }
    return true;
}

std::string adapter_error(cs_loader_adapter_owner_t owner, plugin_handle_t plugin) {
    char* raw = nullptr;
    REQUIRE(sao_plugins_cshost_loader_adapter_get_last_error(owner, plugin, &raw) == SAO_OK);
    const std::string result = raw == nullptr ? std::string{} : raw;
    sao_plugins_cshost_free_string(raw);
    return result;
}

} // namespace

TEST_CASE("csharp generic manifest fields are public and source entries fail closed",
          "[plugins][csharp][generic][contract]") {
    const std::string text = R"({
        "id":"generic_manifest","entry":"plugin.dll","language":"csharp",
        "managed_type":"Example.Plugin, Example",
        "runtimeconfig":"runtime/plugin.runtimeconfig.json"
    })";
    plugin_manifest manifest;
    REQUIRE(sao_plugins_manifest_parse(text.data(), text.size(), &manifest) == SAO_OK);
    REQUIRE(manifest.managed_type == "Example.Plugin, Example");
    REQUIRE(manifest.runtimeconfig == "runtime/plugin.runtimeconfig.json");
    REQUIRE(validate_manifest(manifest) == SAO_OK);

    manifest.runtimeconfig = "../outside.runtimeconfig.json";
    REQUIRE(validate_manifest(manifest) == SAO_ERR_INVALID_ARGUMENT);
}

TEST_CASE("csharp generic loader runs real component load and separates hooks",
          "[plugins][csharp][generic][lifecycle]") {
    std::string capability_reason;
    if (!generic_fixture_available(capability_reason)) {
        WARN("CAPABILITY_SKIP: " << capability_reason);
        SUCCEED("generic lifecycle capability unavailable");
        return;
    }

    cs_loader_adapter_owner_t owner = nullptr;
    const int32_t register_status = sao_plugins_cshost_register_loader_adapter(nullptr, &owner);
    INFO("register_status=" << register_status);
    REQUIRE(register_status == SAO_OK);
    REQUIRE(owner != nullptr);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);

    auto missing_type = hello_manifest("csharp_generic_missing_type");
    missing_type.managed_type.clear();
    auto missing_type_plugin = add_plugin(missing_type);
    REQUIRE(sao_plugins_lifecycle_load(missing_type_plugin) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(adapter_error(owner, missing_type_plugin).find("managed_type") != std::string::npos);
    REQUIRE(adapter_error(owner, missing_type_plugin).empty());
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(missing_type_plugin);

    auto missing_assembly = hello_manifest("csharp_generic_missing_assembly");
    missing_assembly.entry = "prebuilt/MissingComponent.dll";
    auto missing_assembly_plugin = add_plugin(missing_assembly);
    REQUIRE(sao_plugins_lifecycle_load(missing_assembly_plugin) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(adapter_error(owner, missing_assembly_plugin).find("managed file") !=
            std::string::npos);
    remove_plugin(missing_assembly_plugin);

    auto missing_runtime = hello_manifest("csharp_generic_missing_runtime");
    missing_runtime.runtimeconfig = "prebuilt/MissingComponent.runtimeconfig.json";
    auto missing_runtime_plugin = add_plugin(missing_runtime);
    REQUIRE(sao_plugins_lifecycle_load(missing_runtime_plugin) == SAO_ERR_HANDLE_INVALID);
    REQUIRE(adapter_error(owner, missing_runtime_plugin).find("runtimeconfig unavailable") !=
            std::string::npos);
    remove_plugin(missing_runtime_plugin);

    auto source_entry = hello_manifest("csharp_generic_source_rejected");
    source_entry.entry = "HelloPlugin.cs";
    auto source_entry_plugin = add_plugin(source_entry);
    REQUIRE(sao_plugins_lifecycle_load(source_entry_plugin) == SAO_PLUGINS_ERR_UNSUPPORTED);
    REQUIRE(adapter_error(owner, source_entry_plugin).find("precompiled .dll entries only") !=
            std::string::npos);
    remove_plugin(source_entry_plugin);

    auto manifest = hello_manifest("csharp_generic_real");
    manifest.runtimeconfig.clear();
    auto plugin = add_plugin(manifest);
    REQUIRE(sao_plugins_lifecycle_load(plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(plugin) == lifecycle_state::loaded_disabled);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 1);

    plugin_context_t* adapter_context = nullptr;
    plugin_context_t* loader_context = nullptr;
    REQUIRE(sao_plugins_cshost_loader_adapter_get_context(owner, plugin, &adapter_context) ==
            SAO_OK);
    REQUIRE(sao_plugins_lifecycle_get_context(plugin, &loader_context) == SAO_OK);
    REQUIRE(adapter_context != nullptr);
    REQUIRE(adapter_context == loader_context);
    REQUIRE(std::string(sao_plugins_ctx_plugin_id(adapter_context)) == "csharp_generic_real");

    REQUIRE(sao_plugins_cshost_unregister_loader_adapter(owner) == SAO_PLUGINS_ERR_BUSY);

    const int32_t enable_status = sao_plugins_lifecycle_enable(plugin);
    INFO("enable_status=" << enable_status << " adapter_error=" << adapter_error(owner, plugin));
    REQUIRE(enable_status == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(plugin) == lifecycle_state::loaded_active);

    REQUIRE(sao_plugins_lifecycle_unload(plugin) == SAO_OK);
    REQUIRE(sao_plugins_lifecycle_state(plugin) == lifecycle_state::unloaded);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(plugin);

    auto reload = hello_manifest("csharp_generic_reload_rejected");
    auto reload_plugin = add_plugin(reload);
    REQUIRE(sao_plugins_lifecycle_load(reload_plugin) == SAO_PLUGINS_ERR_ALREADY_EXISTS);
    REQUIRE(adapter_error(owner, reload_plugin).find("process restart") != std::string::npos);
    REQUIRE(sao_plugins_cshost_loader_adapter_plugin_count(owner) == 0);
    remove_plugin(reload_plugin);

    REQUIRE(sao_plugins_cshost_unregister_loader_adapter(owner) == SAO_OK);
}
