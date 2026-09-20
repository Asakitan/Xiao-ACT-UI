#pragma once

#include "as_host_internal.h"

#include <cstdint>
#include <mutex>

#include <nlohmann/json.hpp>

class asIScriptContext;
class asIScriptEngine;
class asIScriptFunction;

namespace sao::plugins::angel_host {

std::recursive_mutex& engine_execution_mutex() noexcept;

int32_t register_generic_core_bindings(asIScriptEngine* engine);

int32_t invoke_generic_function(asIScriptContext* context, asIScriptFunction* function,
                                const char* args_json_utf8, char** out_result_json_utf8,
                                void* context_user_data = nullptr);

#if defined(SAO_HAS_ANGELSCRIPT)

// Canonical ctx.* surface registration — binds PluginContext properties,
// event/settings/register/timer/hotkey/ui/compositor/engine methods plus the
// "angelscript" .as runtime-bridge provider. Called at the end of the stdlib
// install pass so dictionary/array/json types already exist.
int32_t register_ctx_surface_bindings(asIScriptEngine* engine) noexcept;

// Releases every ctx-surface record owned by `bound_context` (callbacks, menu
// providers, engine refs, the owner dictionary). Called from
// teardown_plugin_locked while the engine and module are still alive.
void ctx_surface_teardown(void* bound_context) noexcept;

// json@ bridging — implemented in as_stdlib.cpp where json_value is visible.
void* json_ref_from_ordered(asIScriptEngine* engine,
                            const nlohmann::ordered_json& value) noexcept;
const nlohmann::ordered_json* json_ref_value(const void* object) noexcept;

#endif

} // namespace sao::plugins::angel_host
