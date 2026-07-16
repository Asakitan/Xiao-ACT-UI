// as_module_bridge.h — SDK 类型注册到 AS engine
//
// 用 asIScriptEngine::RegisterObjectType + RegisterObjectMethod 把 PluginContext
// 暴露成 AS class:
//   class PluginContext {
//       void log(string message);
//       int subscribe(string topic, callback @cb);
//       ...
//   }
//
// 这一层调 sdk_binding/binding_angel 做真的类型注册。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;

namespace sao::plugins::angel_host {

// 注册 SDK 类型 (在 create engine 后立刻调, engine 生命周期内一次)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_register_sdk(asIScriptEngine* engine);

// 每插件绑定一个 ctx 到 AS 全局 (作为 PluginContext@ ctx)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_bind_ctx(asIScriptEngine* engine,
                            void* ctx_handle,   // sao::plugins::loader::plugin_context_t*
                            const char* module_name);

} // namespace sao::plugins::angel_host
