// as_host.h — AngelScript 真 SDK 嵌入 (asIScriptEngine 生命周期)
//
// **重要升级说明**: Python 侧 angel_runtime.py 是手写子集解释器 (纯 Python
// AST + eval), **没有** angelscript.h 的实际调用。C++ 侧改用真 AngelScript
// SDK, 拿到:
//   - 真 static typing (int, float, string, dictionary, array)
//   - 真 class + inheritance
//   - handle (@) 引用计数
//   - JIT (可选加载 asJITCompiler)
//
// 因此这是 **strict upgrade**, 不是 1:1 移植。
//
// 对齐 Python 源: angel_runtime.py 的 _AngelScriptInterpreter (作为语义参考,
// 但底层实现完全不同)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

// 前置声明避免 angelscript.h 泄漏 (build 无 vcpkg 时也能 include 本 header)
class asIScriptEngine;
class asIScriptContext;
class asIScriptModule;

namespace sao::plugins::angel_host {

typedef struct as_host_s* as_host_handle_t;

struct as_host_config {
    // 是否启用 JIT (asJITCompiler)
    bool enable_jit = false;
    // 消息回调 (编译错误 / warn / info)
    void (*message_callback)(const char* message, int line, int col, int severity, void* ud);
    void* callback_user_data = nullptr;
};

// 创建 asIScriptEngine。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_create(const as_host_config* cfg, as_host_handle_t* out_host);

// 释放 engine (自动清所有 module + context)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_destroy(as_host_handle_t host);

// 拿到底层 asIScriptEngine* (给 sdk_binding/binding_angel 用)。
extern "C" SAO_PLUGINS_API asIScriptEngine* SAO_PLUGINS_CALL
sao_plugins_ashost_engine(as_host_handle_t host);

// 复制最后一批结构化 build/message 诊断；输出由
// sao_plugins_ashost_free_string 释放。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_get_last_error(as_host_handle_t host, char** out_error_utf8);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_execute(as_host_handle_t host, const char* source_utf8, size_t source_len,
                           char** out_result_utf8, char** out_error_utf8);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_call_function_by_name(
    as_host_handle_t host, const char* fn_name, char** out_result_utf8, char** out_error_utf8);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL sao_plugins_ashost_free_string(char* value);

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL sao_plugins_ashost_is_available(void);

// 版本号 (对齐 asGetLibraryVersion)。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL sao_plugins_ashost_version(void);

} // namespace sao::plugins::angel_host
