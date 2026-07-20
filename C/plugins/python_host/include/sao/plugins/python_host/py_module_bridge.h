// py_module_bridge.h — 把 SDK 注册为 Python 内置模块 (不用 pybind11)
//
// 手写 PyModuleDef, 把 sao_plugins_ctx_* API 挂到 ``sao_sdk`` 内置模块下。
// 插件在自己的 plugin.py 里可以:
//     import sao_sdk
//     sao_sdk.log("hello")
// 但通常 on_load(ctx) 接收的 ctx 已经是本 host 用 sao_sdk.wrap_ctx(handle)
// 构造的 PluginContext PyObject, 方法直接: ctx.log("hello")
//
// **旧插件兼容** (对齐 project_ai_editor_plugin_provider_manifest 那种"零改
// 动加载"): 老插件从 act_platform.plugins 里 import PluginContext, 或者
// 靠 on_load(ctx) 拿到平台传入的 ctx。C++ 版本里, 我们:
//   1. 注入 sao_sdk 内置模块 (含 wrap_ctx / free_ctx)
//   2. 注入 act_platform.plugins 兼容 shim (act_platform_shim), 让老 import
//      语句自动 resolve 到本 native 模块 (通过 sys.modules 别名)
//   3. 每次调 on_load 前先 wrap_ctx 生成 PyObject, 传给插件
//
// 因此老插件 (如 star_resonance_plugin) 代码**一个字不用改**就能被本 host 加载。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::python_host {

// ── 内置模块名 ──
//
// "sao_sdk"          — 新代码使用的官方 SDK 模块 (推荐)
// "_sao_plugin_native"  — 内部 native 桥 (给 shim 用)
// "act_platform.plugins" — 老代码 alias (由 shim 提供)
constexpr const char* kSaoSdkModuleName = "sao_sdk";
constexpr const char* kNativeBridgeModuleName = "_sao_plugin_native";
constexpr const char* kActPlatformShimName = "act_platform.plugins";

// 注册 sao_sdk 内置模块 (在 Py_Initialize 之前调 PyImport_AppendInittab)。
// 内部实现: PyImport_AppendInittab("sao_sdk", PyInit_sao_sdk_bridge)。
// PyInit_sao_sdk_bridge 内部创建 PyModuleDef, 用 sdk_binding/binding_python.h
// 里的 sao_plugins_binding_python_method_defs() 拿到所有 PyMethodDef 表。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_register_native_module(void);

// 手写 shim 模块 act_platform.plugins — 让旧插件直接
// ``from act_platform.plugins import PluginContext`` 就拿到一个和 native ctx
// 无缝互操作的 Python 类。
//
// 内部实现: 创建虚 module, 把 PluginContext 类符号指到 sao_sdk 里同名类的别名。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_pyhost_register_shim_module(void);

// 每插件在 sao_sdk 模块层面 wrap 一个 ctx handle 成 PyObject。
// out_pyobject 归属调用方 (用 PyDECREF 释放)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_wrap_ctx(void* ctx_ptr, void** out_pyobject);

// 释放本模块创建的 PluginContext PyObject。
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_free_wrapped_ctx(void* pyobject);

// Releases SDK registrations created through a PluginContext.  The Python
// host calls this before DECREF during plugin unload; callers that create a
// standalone context must call it before dropping their final reference.
extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_teardown_native(void* pyobject);

// Status-returning teardown used by hosts that must preserve the context when
// teardown is re-entered from one of its own callbacks. The legacy void entry
// point remains ABI-compatible and leaves deferred work attached to the
// context when this function reports BUSY.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_try_teardown_native(void* pyobject);

// Binds the loader-owned canonical plugin_context_t to an already-created
// Python PluginContext.  The binding is borrowed and remains valid until the
// loader calls the host adapter's unload callback.
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_bind_loader_context(void* pyobject, void* loader_context);

// 已知的 SDK 方法数 (调 binding_python 侧的 method_defs 拿到)。
extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL sao_plugins_pyhost_sdk_method_count(void);

// 内省 PluginContext 记账 (单测和内部使用)。
// record_kind ∈ {"panels","hotkeys","subscriptions","published","logs",
//                "timers","timer_tokens","callback_refs","notifications","settings",
//                "menus"}。
// 返回借用引用? 不, 返回**新引用** (调用方拿到后 Py_DECREF 释放)。
// 无 Python 时返回 nullptr。声明为 void* 避免 include Python.h。
extern "C" SAO_PLUGINS_API void* SAO_PLUGINS_CALL
sao_plugins_pyhost_ctx_get_records(void* pyobject, const char* record_kind);

} // namespace sao::plugins::python_host
