#pragma once

#include "sao/plugins/angel_host/as_plugin_lifecycle.h"
#if defined(SAO_HAS_ANGELSCRIPT)
#include "as_host_internal.h"
#endif
#include "sao/plugins/sdk_binding/binding_common.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class asIScriptContext;
class asIScriptEngine;
class asIScriptModule;

namespace sao::plugins::angel_host {

inline constexpr unsigned long long kPluginContextUserDataSlot = 0x5A05DB01ULL;

using as_string_value = std::string;

enum class plugin_runtime_state {
    ready,
    unloading,
    cleanup_pending,
    dead,
};

struct retained_script_error {
    int32_t status = SAO_OK;
    std::string phase;
    std::string message;
    std::string function;
    std::string section;
    int line = 0;
    int column = 0;
};

struct as_plugin_s {
#if defined(SAO_HAS_ANGELSCRIPT)
    shared_host_state host_state;
    host_instance_lease host_instance;
#endif
    asIScriptEngine* engine = nullptr;
    asIScriptModule* module = nullptr;
    asIScriptContext* context = nullptr;
    void* bound_context = nullptr;
    sdk_binding::plugin_binding_handle_t binding = nullptr;
    std::string plugin_id;
    std::string module_name;
    plugin_runtime_state lifecycle = plugin_runtime_state::ready;
    std::thread::id unload_owner;
    bool unload_hook_completed = false;
    std::unordered_map<std::string, retained_script_error> retained_errors;
    as_sdk_counters counters{};
    std::mutex call_mutex;
};

using shared_plugin_state = std::shared_ptr<as_plugin_s>;

int32_t register_plugin_state(const shared_plugin_state& plugin);
shared_plugin_state acquire_plugin_state(as_plugin_handle_t plugin);
shared_plugin_state retire_plugin_state(as_plugin_handle_t plugin);
int32_t restore_plugin_state(const shared_plugin_state& plugin);
void retain_plugin_error(as_plugin_s& plugin, const char* phase, int32_t status,
                         const char* message, asIScriptContext* context = nullptr);
void clear_plugin_error(as_plugin_s& plugin, const char* phase);
std::string plugin_error_text(as_plugin_handle_t plugin, const char* phase, const char* fallback);

} // namespace sao::plugins::angel_host
