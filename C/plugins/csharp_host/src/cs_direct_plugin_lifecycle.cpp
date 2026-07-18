#include "sao/plugins/csharp_host/cs_plugin_lifecycle.h"

#include "cs_component_internal.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sao::plugins::csharp_host {
namespace {

bool utf8_to_wide(const char* value, std::wstring& output) {
    output.clear();
    if (value == nullptr || value[0] == '\0')
        return false;
#if !defined(_WIN32)
    while (*value != '\0')
        output.push_back(*value++);
    return true;
#else
    const size_t length = std::strlen(value);
    if (length > static_cast<size_t>(INT32_MAX))
        return false;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                                             static_cast<int>(length), nullptr, 0);
    if (required <= 0)
        return false;
    output.resize(static_cast<size_t>(required));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length),
                               output.data(), required) == required;
#endif
}

void set_error(char** output, const std::string& error) noexcept {
    if (output == nullptr)
        return;
    (void)cshost_copy_string(error, output);
}

std::string pascal_identifier(const std::string& value) {
    std::string output;
    bool uppercase_next = true;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) == 0) {
            uppercase_next = true;
            continue;
        }
        output.push_back(uppercase_next ? static_cast<char>(std::toupper(ch))
                                        : static_cast<char>(ch));
        uppercase_next = false;
    }
    return output;
}

void ensure_direct_legacy_contract(sao::plugins::loader::plugin_manifest& manifest) {
    if (!manifest.managed_type.empty())
        return;
    const std::string assembly = std::filesystem::u8path(manifest.entry).stem().string();
    const std::string plugin_namespace = pascal_identifier(manifest.plugin_id);
    if (assembly.empty() || plugin_namespace.empty())
        return;
    const std::string managed_type =
        "SaoAuto.Plugins." + plugin_namespace + "." + assembly + ", " + assembly;
    manifest.managed_type = managed_type;
}

} // namespace

struct cs_plugin_s {
    cs_host_handle_t host = nullptr;
    managed_component_s* component = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    std::string plugin_id;
    bool unload_hook_completed = false;
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_load_plugin(cs_host_handle_t host, const char* plugin_json_path_utf8,
                               cs_plugin_handle_t* out_plugin, char** out_error_utf8) {
    if (out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (host == nullptr || plugin_json_path_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::wstring manifest_path;
        if (!utf8_to_wide(plugin_json_path_utf8, manifest_path)) {
            set_error(out_error_utf8, "plugin.json path is not valid UTF-8");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        sao::plugins::loader::plugin_manifest manifest;
        int32_t status = sao::plugins::loader::sao_plugins_manifest_load_from_file(
            manifest_path.c_str(), &manifest);
        if (status != SAO_OK) {
            set_error(out_error_utf8, "cannot parse plugin.json");
            return status;
        }
        ensure_direct_legacy_contract(manifest);

        managed_component_s* component = nullptr;
        std::string error;
        status = cshost_component_load(host, manifest, &component, error);
        if (status != SAO_OK) {
            set_error(out_error_utf8, error);
            return status;
        }
        auto component_guard = std::unique_ptr<managed_component_s, void (*)(managed_component_s*)>(
            component, cshost_component_abandon);

        SaoSdkContext* sdk_context = nullptr;
        status = sao_sdk_context_create(manifest.source_path.c_str(), manifest.plugin_id.c_str(),
                                        &sdk_context);
        if (status == SAO_OK)
            status = sao_sdk_context_bind_platform_services(sdk_context);
        if (status != SAO_OK) {
            if (sdk_context != nullptr)
                sao_sdk_context_destroy(sdk_context);
            set_error(out_error_utf8, "C# SDK context initialization failed");
            return status;
        }
        auto sdk_guard = std::unique_ptr<SaoSdkContext, void (*)(SaoSdkContext*)>(
            sdk_context, sao_sdk_context_destroy);

        cshost_reset_sdk_counters();
        status = cshost_component_initialize(component, sdk_context, nullptr, error);
        if (status != SAO_OK) {
            set_error(out_error_utf8, error);
            return status;
        }
        int32_t managed_result = 0;
        status = cshost_component_on_load(component, &managed_result, error);
        if (status != SAO_OK || managed_result != 0) {
            if (cshost_component_has_hook(component, managed_hook::on_unload)) {
                int32_t ignored_result = 0;
                std::string ignored_error;
                (void)cshost_component_invoke(component, managed_hook::on_unload, nullptr, 0,
                                              &ignored_result, ignored_error);
            }
            if (error.empty()) {
                error = "OnLoad returned " + std::to_string(managed_result);
            }
            set_error(out_error_utf8, error);
            return status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status;
        }

        auto plugin = std::make_unique<cs_plugin_s>();
        plugin->host = host;
        plugin->component = component_guard.release();
        plugin->sdk_context = sdk_guard.release();
        plugin->plugin_id = manifest.plugin_id;
        *out_plugin = plugin.release();
        return SAO_OK;
    } catch (...) {
        set_error(out_error_utf8, "direct C# load crossed the native exception boundary");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_tick_plugin(cs_plugin_handle_t plugin, char** out_error_utf8) {
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr || plugin->component == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        int32_t managed_result = 0;
        std::string error;
        const int32_t status = cshost_component_invoke(plugin->component, managed_hook::on_tick,
                                                       nullptr, 0, &managed_result, error);
        if (status != SAO_OK || managed_result != 0) {
            if (error.empty()) {
                error = "OnTick returned " + std::to_string(managed_result);
            }
            set_error(out_error_utf8, error);
            return status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : status;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unload_plugin(cs_plugin_handle_t plugin, char** out_error_utf8) {
    if (out_error_utf8 != nullptr)
        *out_error_utf8 = nullptr;
    if (plugin == nullptr || plugin->component == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        int32_t managed_result = 0;
        std::string error;
        if (!plugin->unload_hook_completed &&
            cshost_component_has_hook(plugin->component, managed_hook::on_unload)) {
            const int32_t status = cshost_component_invoke(
                plugin->component, managed_hook::on_unload, nullptr, 0, &managed_result, error);
            if (status != SAO_OK) {
                set_error(out_error_utf8, error);
                return status;
            }
            if (managed_result == 1) {
                set_error(out_error_utf8, "OnUnload vetoed unload");
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            if (managed_result != 0) {
                set_error(out_error_utf8,
                          "OnUnload returned invalid result " + std::to_string(managed_result));
                return SAO_ERR_OS_CALL_FAILED;
            }
            plugin->unload_hook_completed = true;
        }
        const int32_t close_status = cshost_component_close(plugin->component, error);
        if (close_status != SAO_OK) {
            set_error(out_error_utf8, error);
            return close_status;
        }
        plugin->component = nullptr;
        sao_sdk_context_destroy(plugin->sdk_context);
        plugin->sdk_context = nullptr;
        delete plugin;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_get_sdk_counters(cs_plugin_handle_t plugin, cs_sdk_counters* out_counters) {
    if (plugin == nullptr || out_counters == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        return cshost_get_sdk_counters(out_counters);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_read_tick_count(cs_plugin_handle_t plugin, int32_t* out_value) {
    if (plugin == nullptr || plugin->component == nullptr || out_value == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::string error;
        return cshost_component_invoke(plugin->component, managed_hook::get_tick_count, nullptr, 0,
                                       out_value, error);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::csharp_host
