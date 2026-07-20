// binding_python.h — SDK C ABI → CPython 桥 (手写 PyModuleDef, 不用 pybind11)
//
// 把 sao_plugins_ctx_* C ABI 暴露为一个 Python 内置模块 (手写 PyModuleDef, 不用
// pybind11)。python_host 在 Py_Initialize 之前调 register_sao_module() 把模块
// 挂到 sys.modules['sao_sdk'] 上, 插件里就能 import 它。
//
// 对齐 Python 源: plugin.py 里直接用 ``ctx = PluginContext``, C++ 版本要在
// native 侧模拟同名同签的 API 表面 —— 每个 Python plugin.py 里写
// ``ctx.register_ui_panel(...)`` 或 ``ctx.log(...)`` 都要落到本模块的对应
// PyCFunction 里。
//
// 关键: 手写 PyModuleDef + PyMethodDef 数组, 每个方法用 METH_VARARGS 收
// (*args), 内部 arg parse 完后调 sao_plugins_ctx_* API。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"

// 前置声明避免这里 include Python.h (让不打开 python_host 的构建也能用)
struct _object;
using PyObject = struct _object;
struct PyMethodDef;
struct PyModuleDef;

namespace sao::plugins::sdk_binding {

typedef struct plugin_context_s* plugin_context_ptr; // 来自 loader/plugin_context.h

// ── 模块生命周期 ──────────────────────────────────────

// 供 python_host 调用: 在 Py_Initialize 之前注入内置模块名 "sao_sdk"。
// 使用 PyImport_AppendInittab, 之后 Python 侧 import sao_sdk 就走本模块的
// PyInit_sao_sdk (即 sao_plugins_binding_python_module_init)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_register_module(void);

// PyInit_sao_sdk 内部实现 (由 PyImport_AppendInittab 拿函数指针)。
extern "C" SAO_PLUGINS_API PyObject* SAO_PLUGINS_CALL
sao_plugins_binding_python_module_init(void);

// 每插件在 Py 层创建 ctx PyObject。返回借用引用, 归属由宿主管理。
// 该 PyObject 是一个 "PluginContext" 类实例, __getattr__ 派发到 SDK 方法。
extern "C" SAO_PLUGINS_API PyObject* SAO_PLUGINS_CALL
sao_plugins_binding_python_wrap_ctx(plugin_context_ptr ctx);

// ── 参数 / 返回值转换 ─────────────────────────────────

// 从 Py 参数元组抽出常见类型 → C ABI (供内置模块方法实现用)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_arg_to_utf8(PyObject* arg, char** out_utf8);

// C ABI 结果 → Py 对象 (dict / list / str / int)。
extern "C" SAO_PLUGINS_API PyObject* SAO_PLUGINS_CALL
sao_plugins_binding_python_json_to_pyobject(const char* utf8_json);

// Py 对象 → utf-8 json 字符串 (归属调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_pyobject_to_json(PyObject* obj, char** out_json_utf8);

// ── SDK 方法暴露签名 ── (每一条 PyCFunction 一对一映射 sdk_method_id)
//
// 这些 PyCFunction 由 PyMethodDef 表列出, 手写实现. 每条内部 arg parse 后调
// 对应 sao_plugins_ctx_* API. 签名统一为 METH_VARARGS.
//
// 命名约定: py_ctx_<method_name>, 与 loader/plugin_context.h 的
// sao_plugins_ctx_<method_name> 一一对应.

// 属性访问 (通过 PyGetSetDef, 内部实现)
extern "C" PyObject* py_ctx_get_plugin_id(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_path(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_web_path(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_assets_path(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_should_stop(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_mem(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_ui(PyObject* self, void* closure);
extern "C" PyObject* py_ctx_get_engine(PyObject* self, void* closure);

// 日志
extern "C" PyObject* py_ctx_log(PyObject* self, PyObject* args);

// 事件族
extern "C" PyObject* py_ctx_subscribe(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_subscribe_once(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_unsubscribe(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_emit(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_on_damage(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_on_heal(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_on_skill(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_on_boss(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_on_snapshot(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_on_encounter_finalized(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_get_snapshot(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_snapshot_value(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_recent_events(PyObject* self, PyObject* args);

// 设置族
extern "C" PyObject* py_ctx_get_setting(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_setting(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_defaults(PyObject* self, PyObject* args);

// 扩展注册族
extern "C" PyObject* py_ctx_register_parser_adapter(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_exporter(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_formatter(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_trigger_type(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_report_view(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_timer(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_ui_panel(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_render_hook(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_overlay(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_clear_overlay(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_hotkey(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_engine(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_data_source(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_menu_category(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_menu_surface(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_register_action_handler(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_request_redraw(PyObject* self, PyObject* args);

// 定时器
extern "C" PyObject* py_ctx_set_interval(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_timeout(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_clear_timer(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_run_on_ui(PyObject* self, PyObject* args);

// 通知 / 对话框 / 窗口
extern "C" PyObject* py_ctx_notify(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_dismiss_notify(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_toast(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_open_file(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_open_window(PyObject* self, PyObject* args);

// Compositor Layer
extern "C" PyObject* py_ctx_create_compositor_layer(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_upload_compositor_frame(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_compositor_layer_mmf_source(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_compositor_layer_shared_texture_source(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_compositor_layer_position(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_compositor_layer_visible(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_set_compositor_layer_input(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_destroy_compositor_layer(PyObject* self, PyObject* args);

// 引擎 / 依赖
extern "C" PyObject* py_ctx_get_engine_method(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_require_engine(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_call_engine(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_call_runtime(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_ensure_requirements(PyObject* self, PyObject* args);
extern "C" PyObject* py_ctx_load_local(PyObject* self, PyObject* args);

// PyMethodDef 表 (在 binding_python.cpp 里定义, 供 module init 调 PyModule_AddFunctions)。
extern "C" SAO_PLUGINS_API const struct PyMethodDef* SAO_PLUGINS_CALL
sao_plugins_binding_python_method_defs(size_t* out_count);

// ── 激活 Python 侧 binding (返 opaque plugin binding) ─────────────────
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_activate(plugin_context_ptr plugin_ctx,
                                    void* py_host_state,
                                    plugin_binding_handle_t* out_plugin);
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_binding_python_deactivate(plugin_binding_handle_t plugin);

} // namespace sao::plugins::sdk_binding
