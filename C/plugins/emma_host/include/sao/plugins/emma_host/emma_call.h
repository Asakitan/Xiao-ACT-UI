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

#include <cstddef>
#include <cstdint>

#include "sao/plugins/emma_host/emma_error.h"
#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::emma_host {

class interpreter;
struct ast_pool;

typedef struct emma_plugin_s* emma_plugin_handle_t;
using emma_interpreter_callback_t = int32_t(SAO_PLUGINS_CALL*)(interpreter* interp,
                                                               void* user_data);

// 加载 .emma 脚本 (lex + parse + execute 顶层, 拿到 on_* 定义)。
// 内部:
//   1. 读文件到 utf-8 string
//   2. sao_plugins_emma_tokenize
//   3. parser::parse_program
//   4. 创建 interpreter, install_builtins
//   5. sao_plugins_emma_install_stdlib + install_io_stdlib(plugin_dir)
//   6. 注册 loader canonical plugin_context_t 的 native shim
//   7. interpreter::execute (跑顶层, 定义所有 fn)
//   8. get_function("on_load") 存 handle
// plugin_dir/plugin_id_utf8 仅保留 ABI 兼容；实际身份与根路径来自 ctx_ptr 的
// canonical loader context。若失败回滚被 provider 注销阻塞，out_plugin 返回
// closing handle，调用方必须重试 unload，期间其他操作均返回 BUSY。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_load_script(
    const wchar_t* plugin_dir, const char* entry_relative, const char* plugin_id_utf8,
    void* ctx_ptr, // plugin_context_t*
    emma_plugin_handle_t* out_plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_load_script_ex(
    const wchar_t* plugin_dir, const char* entry_relative, const char* plugin_id_utf8,
    void* ctx_ptr, emma_plugin_handle_t* out_plugin, emma_error* out_error);

// 调 on_load(ctx) 传入 ctx emma_value (已 register_ctx)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_load(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_enable(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_disable(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_on_unload(emma_plugin_handle_t plugin, bool* out_allow_unload);

// 通用 hook: args_json 是 hook 参数的 JSON 数组，结果是 malloc 分配的 JSON。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_call_hook(emma_plugin_handle_t plugin, const char* hook_name,
                           const char* args_json_utf8, char** out_result_json_utf8);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_call_hook_ex(
    emma_plugin_handle_t plugin, const char* hook_name, const char* args_json_utf8,
    char** out_result_json_utf8, emma_error* out_error);

// 查该插件是否有指定 hook。
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_emma_has_hook(emma_plugin_handle_t plugin, const char* hook_name);

// 在 direct operation lease 与调用锁内同步访问底层 interpreter。
// interp 只在 callback 返回前有效；callback 不得保留指针或重入同一 plugin。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_with_interpreter(
    emma_plugin_handle_t plugin, emma_interpreter_callback_t callback, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_get_last_error(emma_plugin_handle_t plugin, emma_error* out_error);

// 进入 closing 并阻止新调用；有活动 operation 时返回 BUSY，重试完成销毁。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unload_script(emma_plugin_handle_t plugin);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_execute_source(interpreter* interp, const char* source_utf8, size_t source_len,
                                ast_pool* out_pool, char** out_error_utf8);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_emma_free_string(char* value);

} // namespace sao::plugins::emma_host
