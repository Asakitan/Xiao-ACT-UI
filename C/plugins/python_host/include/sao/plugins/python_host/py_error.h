// py_error.h — Python 异常 → SAO_STATUS 转换
//
// Python 里 raise 出来的异常在 C 侧要:
//   1. PyErr_Fetch 取出 (type, value, traceback)
//   2. PyErr_NormalizeException 规范化
//   3. traceback.format_exception 或 手写 fallback 转字符串
//   4. 映射到 SAO_STATUS + utf8 消息
//   5. PyErr_Clear 清干净, 避免污染下次调用
//
// 对齐 Python 源: PluginManager._record_failure 里的 exc → last_error 转换。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

namespace sao::plugins::python_host {

// 从当前线程的 PyErr 状态取错误并清空。
// 返回值: SAO_STATUS_*, 出参 out_utf8 是分配的 utf-8 消息 (调用方 free)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_take_error(char** out_utf8);

// 把 SAO_STATUS 映射回 Python exception 类型名 (供打日志时可读)。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_pyhost_status_to_python_exc(int32_t status);

} // namespace sao::plugins::python_host
