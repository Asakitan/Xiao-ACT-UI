#pragma once

#include "sao/plugins/angel_host/as_plugin_lifecycle.h"

#include <mutex>
#include <string>

class asIScriptContext;
class asIScriptEngine;
class asIScriptModule;

namespace sao::plugins::angel_host {

inline constexpr unsigned long long kPluginContextUserDataSlot = 0x5A05DB01ULL;

struct as_string_value {
    std::string data;
};

struct as_plugin_s {
    asIScriptEngine* engine = nullptr;
    asIScriptModule* module = nullptr;
    asIScriptContext* context = nullptr;
    void* bound_context = nullptr;
    std::string plugin_id;
    std::string module_name;
    as_sdk_counters counters{};
    std::mutex call_mutex;
};

} // namespace sao::plugins::angel_host
