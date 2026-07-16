// emma_call.h — 从 C++ 调 Emma 函数 (hook 分派) + host 值注入
//
// 平台侧生命周期 (load / enable / disable / unload) 需要调 Emma 里的
// on_load(ctx) / on_enable() / on_disable() / on_unload()。这一层做
// hook 提取和调用。
//
// 对齐 Python 源: emma_runtime.py 的 EmmaRuntime.load_script → module.set_hook
// 那一段, 加 interp.get_function(hook_name)。
//
// Emma 侧 SDK 调用流:
//   1. plugin.emma 顶层 `let click = 0` `fn on_load(ctx) ... end` 定义
//   2. load_script 时 emma_interpreter.execute 跑顶层, on_load 成为全局
//      callable
//   3. call_on_load → interp.call_function(on_load, [ctx_emma_value])
//   4. Emma 函数体里 `ctx.log("hi")`:
//        - `ctx` 从作用域取出 → dict emma_value
//        - `.log` → dict["log"] 取出 → callable (host_impl 非空)
//        - `("hi")` → call_function → host_impl 里 emma_value → C ABI 转换
//        - host_impl 调 sao_plugins_ctx_log(ctx_ptr, "hi")
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::emma_host {

class interpreter;

typedef struct emma_plugin_s* emma_plugin_handle_t;

// 加载 .emma 脚本 (lex + parse + execute 顶层, 拿到 on_* 定义)。
// 内部:
//   1. 读文件到 utf-8 string
//   2. sao_plugins_emma_tokenize
//   3. parser::parse_program
//   4. 创建 interpreter, install_builtins
//   5. sao_plugins_emma_install_stdlib + install_io_stdlib(plugin_dir)
//   6. sao_plugins_binding_emma_register_ctx (由 sdk_binding 提供)
//   7. interpreter::execute (跑顶层, 定义所有 fn)
//   8. get_function("on_load") 存 handle
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_load_script(const wchar_t* plugin_dir,
                             const char* entry_relative,
                             const char* plugin_id_utf8,
                             void* ctx_ptr,        // plugin_context_t*
                             emma_plugin_handle_t* out_plugin);

// 调 on_load(ctx) 传入 ctx emma_value (已 register_ctx)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_load(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_enable(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_disable(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_unload(emma_plugin_handle_t plugin,
                                bool* out_allow_unload);

// 通用 hook: args_json 是 hook 参数的 json 数组; 对 on_load 就是 [ctx_wrapper_id]。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_hook(emma_plugin_handle_t plugin,
                           const char* hook_name,
                           const char* args_json_utf8,
                           char** out_result_json_utf8);

// 查该插件是否有指定 hook。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_emma_has_hook(emma_plugin_handle_t plugin, const char* hook_name);

// 拿底层 interpreter (给 sdk_binding 用)。
extern "C" SAO_PLUGINS_API interpreter* SAO_PLUGINS_CALL
sao_plugins_emma_get_interpreter(emma_plugin_handle_t plugin);

// 卸载 (interpreter 析构, 释放 AST 池)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unload_script(emma_plugin_handle_t plugin);

} // namespace sao::plugins::emma_host
