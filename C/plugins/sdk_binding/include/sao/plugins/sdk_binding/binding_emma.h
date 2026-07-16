// binding_emma.h — SDK C ABI → Emma 桥
//
// Emma 是 C++ 内实现的解释器 (见 emma_host/), ctx 就是宿主 C++ 对象引用。
// 绑定层负责把 EmmaValue (variant<null, bool, int, double, string, array, dict,
// callable>) 转成 SDK C ABI 用的 utf8 json / raw types。
//
// 对齐 Python 源: emma_runtime.py 里的 _EmmaProxy (每个 ctx 方法都手动转发)。
//
// Emma 侧访问模式 (对齐 example_emma_plugin/plugin.emma):
//     fn on_load(ctx)
//         ctx.log("hello")
//         ctx.register_ui_panel("id", {title: "Demo"}, render, on_action)
//     end
//
// 关键: Emma 是 C++ 侧原生对象, ctx 直接是 C++ shared_ptr<PluginContext>,
// 方法调用不需要跨 ABI, 只需 emma_value ↔ C++ 值转换。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"

namespace sao::plugins::sdk_binding {

typedef struct plugin_context_s* plugin_context_ptr;
typedef struct emma_value_s* emma_value_ptr;   // 来自 emma_host/emma_interpreter.h
typedef struct emma_interpreter_s* emma_interpreter_ptr;

// ── SDK 注入到 Emma 解释器 ──────────────────────────

// 把 sao_plugins_ctx_* API 注入 Emma 解释器 (作为 ctx 全局)。
// 内部: 建一个 dict emma_value 存 ctx 对象, 为每个 sdk_method_id 生成一个
// host_impl callable, 把 emma_value 参数转 C++ 后调 SDK C ABI。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_register_ctx(emma_interpreter_ptr interp,
                                      plugin_context_ptr ctx);

// 装 ctx.ui / ctx.mem / ctx.engine 三个子 dict。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_register_ui(emma_interpreter_ptr interp);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_register_mem(emma_interpreter_ptr interp,
                                      plugin_context_ptr ctx);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_register_engine(emma_interpreter_ptr interp,
                                          plugin_context_ptr ctx);

// ── 值转换 ────────────────────────────────────────────

// EmmaValue → utf-8 json (归属调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_value_to_json(emma_value_ptr value,
                                       char** out_json_utf8);

// utf-8 json → EmmaValue (由 interp 的 pool 分配, 归属 interp)。
extern "C" SAO_PLUGINS_API emma_value_ptr SAO_PLUGINS_CALL
sao_plugins_binding_emma_json_to_value(emma_interpreter_ptr interp,
                                       const char* utf8_json);

// 包装 EmmaCallable 为 SDK 事件回调 (subscribe / on_action / hotkey)。
// 内部把 shared_ptr<callable> 存进 tracked_callbacks, 反调用时通过 interp
// 的 call_function 执行。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_wrap_callback(emma_interpreter_ptr interp,
                                       emma_value_ptr callable,
                                       void** out_sdk_callback_ptr,
                                       void** out_user_data);

// 释放 wrap_callback 分配的 user_data (归还 shared_ptr 引用)。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_binding_emma_release_callback(void* user_data);

// ── SDK 方法在 Emma 侧的 C++ 实现 ── (由 emma_interpreter 通过 host_impl 调用)
//
// 每条 emma_ctx_<method> 拿 vector<emma_value> args 参数, 返回 emma_value。
// 内部把 emma_value 转 C ABI, 调 sao_plugins_ctx_*, 结果转回 emma_value。
//
// 完整暴露 (对齐 sdk_method_id 每一项). C++ 命名空间不用 extern "C", 只在
// emma_interpreter 里用 std::function 挂载, 不跨 ABI。

// 声明所有 SDK 方法的 host_impl 函数指针类型。签名统一为:
//   emma_value(vector<emma_value> args, plugin_context_ptr ctx)
using emma_ctx_method_fn = int32_t (*)(void* interp_impl,
                                       plugin_context_ptr ctx,
                                       const void* args_vector,
                                       void* out_return_value);

// 表: 每一项一个 sdk_method_id + emma_ctx_method_fn。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_get_method_table(sdk_method_id* out_ids,
                                          emma_ctx_method_fn* out_fns,
                                          size_t* inout_count);

// ── Wave 4 新增: 激活 Emma 侧 binding ────────────────────────
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_activate(plugin_context_ptr plugin_ctx,
                                  emma_interpreter_ptr interp,
                                  plugin_binding_handle_t* out_plugin);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_emma_deactivate(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
