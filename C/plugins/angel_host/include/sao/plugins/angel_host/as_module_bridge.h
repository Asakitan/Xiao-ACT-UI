// as_module_bridge.h — SDK 类型注册到 AS engine
//
// 用 generic calling convention 注册当前宿主实际支持的最小 PluginContext:
// log/log_info、plugin_id、should_stop。注册与 canonical context ownership
// 经过 sdk_binding Angel provider；尚未注册的完整 SDK 方法不在本头契约内。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;

namespace sao::plugins::angel_host {

// 注册最小宿主类型，并确保 sdk_binding Angel provider 已注册。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_register_sdk(asIScriptEngine* engine);

// 将 loader canonical context 绑定到指定 module 的 PluginContext@ ctx。
// 非空绑定同时经过 sdk_binding context_bind；nullptr 仅清空 module slot。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_bind_ctx(asIScriptEngine* engine,
                            void* ctx_handle, // sao::plugins::loader::plugin_context_t*
                            const char* module_name);

} // namespace sao::plugins::angel_host
