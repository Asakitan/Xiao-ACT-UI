#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_isolation.h"
#include "sao/plugins/loader/plugin_lifecycle.h"

namespace sao::plugins::loader {

struct plugin_context_s;

using native_on_load_fn = int32_t(SAO_PLUGINS_CALL*)(plugin_context_s*);
using native_simple_fn = int32_t(SAO_PLUGINS_CALL*)();

struct plugin_handle_s {
    mutable std::mutex mutex;
    plugin_manifest manifest;
    lifecycle_state state = lifecycle_state::discovered;
    bool user_owned = false;
    std::atomic_bool stop_requested{false};
    HMODULE native_module = nullptr;
    native_on_load_fn native_on_load = nullptr;
    native_simple_fn native_on_enable = nullptr;
    native_simple_fn native_on_disable = nullptr;
    native_simple_fn native_on_unload = nullptr;
    plugin_context_s* context = nullptr;
    deps_session_t dependency_session = nullptr;
    bool unload_hook_completed = false;
    bool host_adapter_unloaded = false;
    uint32_t failure_count = 0;
    std::string last_error;
};

struct plugin_registry_s {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<plugin_handle_s>> plugins;
    std::vector<extension_record> extensions;
};

plugin_registry_s& registry_storage() noexcept;
std::shared_ptr<plugin_handle_s> retain_plugin(plugin_handle_t plugin) noexcept;
bool registry_contains(plugin_handle_t plugin) noexcept;
plugin_manifest manifest_snapshot(plugin_handle_t plugin);
bool plugin_is_user_owned(plugin_handle_t plugin) noexcept;
void plugin_remove_extensions(plugin_handle_t plugin) noexcept;
void plugin_context_request_stop(plugin_context_t* ctx) noexcept;
void plugin_context_clear_stop(plugin_context_t* ctx) noexcept;
int32_t plugin_context_register_entity_providers(plugin_context_t* ctx,
                                                 const native_entity_provider_descriptor* providers,
                                                 size_t count) noexcept;
bool plugin_context_entity_provider_is_current_thread(plugin_context_t* ctx) noexcept;
bool plugin_context_platform_is_current_thread(plugin_context_t* ctx) noexcept;
int32_t plugin_context_quiesce_entity_providers(plugin_context_t* ctx) noexcept;
int32_t plugin_context_quiesce_platform(plugin_context_t* ctx) noexcept;
int32_t plugin_context_resume_entity_providers(plugin_context_t* ctx) noexcept;
int32_t plugin_context_release_resources(plugin_context_t* ctx) noexcept;
int32_t plugin_context_destroy(plugin_context_t* ctx) noexcept;

} // namespace sao::plugins::loader