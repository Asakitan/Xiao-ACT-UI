// py_call.h — 从 C++ 调 Python 函数 (hook 分派)
//
// 每个插件的 plugin.py 定义 on_load(ctx) / on_enable() / on_disable() /
// on_unload() 等 hook。加载后 py_host 抽出这些 PyObject 存起来,
// 生命周期机需要时通过这里调用。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct _object;
using PyObject = struct _object;

namespace sao::plugins::python_host {

typedef struct py_plugin_s* py_plugin_handle_t; // 每插件一个

// 加载 plugin.py 到独立 module space, 提取 hook 函数指针。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_load_script(const wchar_t* plugin_dir,
                               const char* entry_relative,
                               const char* plugin_id_utf8,
                               py_plugin_handle_t* out_plugin);

// 调 hook(*args), 结果转 json 返回。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_call_hook(py_plugin_handle_t plugin,
                             const char* hook_name,
                             const char* args_json_utf8,
                             char** out_result_json_utf8);

// 卸载 (从 sys.modules 删 plugin 的模块, DECREF hook 对象)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_unload_script(py_plugin_handle_t plugin);

} // namespace sao::plugins::python_host
