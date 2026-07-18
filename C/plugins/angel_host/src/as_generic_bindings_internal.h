#pragma once

#include <cstdint>

class asIScriptContext;
class asIScriptEngine;
class asIScriptFunction;

namespace sao::plugins::angel_host {

int32_t register_generic_core_bindings(asIScriptEngine* engine);

int32_t invoke_generic_function(asIScriptContext* context, asIScriptFunction* function,
                                const char* args_json_utf8, char** out_result_json_utf8,
                                void* context_user_data = nullptr);

} // namespace sao::plugins::angel_host
