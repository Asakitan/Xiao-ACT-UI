// as_sandbox.h — AngelScript 沙箱 (Wave 18 / Agent b 真装)
//
// AngelScript 的沙箱定位:
//   1. 只注册**白名单** global function / global type — 危险 API 不 register
//      即不可调 (script 无 addon 加载权).
//   2. 强制 JIT 关 → 走解释器, 生成机器码无从逃逸.
//   3. 通过 SetContextLineCallback 定期回调 → 超 max_context_execution_ms
//      Abort 掉 script.
//   4. arm 时把 config 快照存 (engine → snapshot) map, 未来 disarm 支持撤销
//      (仅撤销 line callback / JIT 属性; 已注册的 global 无法反注册).
//
// arm(engine, cfg) 会:
//   - engine->SetJITCompiler(nullptr)
//   - 从 engine 里读一次原 max exec ms (0)
//   - 逐条 RegisterGlobalFunction 白名单 API — math (sqrt/cos/sin/floor/ceil
//     /abs/pow/log/exp) + log_info(string).  这些函数由本 lib 自 己实装.
//   - 如果 allow_string_type=true, 注册一个 lite SaoString 值类型 (as "string"
//     命名)  — 简化版, 不用 scriptstdstring addon.
//   - 若 allow_array_type=true, gate 一句 stub — 目前 lite 实装不注册数组
//     (预留位, 未来接 scriptarray).
//   - 存 (engine → snapshot) 到进程级 map.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

class asIScriptEngine;

namespace sao::plugins::angel_host {

struct as_sandbox_config {
    // 老字段 — 兼容 wave3 stub 阶段.
    bool allow_datetime_addon = false;
    bool allow_file_addon = false;
    bool allow_math_addon = true;
    bool allow_dictionary_addon = false;
    bool allow_array_addon = false;
    bool allow_string_addon = true;

    // 新字段 (wave18b 真装)
    //
    // allow_string_type:
    //   注册 SaoString 值类型 (命名为 "string") — script 里可写
    //   `string s = "hi";` 若 false, "string" 类型缺失, 编译 fail.
    bool allow_string_type = true;

    // allow_array_type: 保留位 (当前实装总是拒).
    bool allow_array_type = false;

    // allow_dictionary_type: 保留位 (当前实装总是拒).
    bool allow_dictionary_type = false;

    // max_context_execution_ms: 每次 asIScriptContext::Execute 的最长毫秒.
    // 通过 SetContextLineCallback 定期抢占, 超时 Context->Abort().  0 = 不限.
    uint32_t max_context_execution_ms = 0;

    // deny_globals: 追加拒名列表 (无法反注册已注册的, 但 arm 会绕过 register).
    const char* const* deny_globals = nullptr;
    size_t              deny_globals_count = 0;
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_arm(asIScriptEngine* engine,
                               const as_sandbox_config* cfg);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_disarm(asIScriptEngine* engine);

// 已 armed 查询.
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_is_armed(asIScriptEngine* engine);

// 查上次 arm 记录的 max_context_execution_ms (test 用).
extern "C" SAO_PLUGINS_API uint32_t SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_max_context_ms(asIScriptEngine* engine);

// 查 JIT 是否被强制关 (test 用). 返 true 表 arm 后 SetJITCompiler(nullptr) 已挂.
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_ashost_sandbox_jit_off(asIScriptEngine* engine);

} // namespace sao::plugins::angel_host
