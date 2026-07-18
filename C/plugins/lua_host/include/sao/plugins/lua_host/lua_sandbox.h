// lua_sandbox.h — Lua 沙箱 (Wave 18 / Agent b 真装)
//
// Wave 18 / Agent b 将 wave3 时期的 stub 升级成真沙盒:
//   * 用 restricted _ENV table 收窄可见全局
//   * 通过 lua_setallocf 拦截 alloc 累计跟踪 & 上限拦截 (returns NULL → Lua raises
//     "not enough memory")
//   * 通过 lua_sethook(LUA_MASKCOUNT) 定期强制中断防死循环
//   * 支持通过 config 追加拒绝的具名全局
//
// arm(L, cfg) 会把 (L → snapshot) 记入进程级 map, 允许后续 disarm(L) 恢复原有
// _G / alloc / hook 状态.  同一个 L 重复 arm 视为 EINVAL (先 disarm 再 arm).
//
// 对齐 Python 源: lua_runtime.py 没做真沙箱, C++ 侧加固为可选.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host {

// ── 老字段 (向后兼容 wave3 stub 阶段) ─────────────────────────────────
//
// 这些 bool 老字段控制"是否让脚本触到 fs / net / process / require /
// coroutine".  Wave 18/b 新装法下若这些为 false, 相应全局符号会被从
// restricted _ENV 里移除.
struct lua_sandbox_config {
    bool allow_fs = false;
    bool allow_net = false;
    bool allow_process = false;
    bool allow_require = false;
    bool allow_coroutine = true;

    // ── Wave 18/b 新增字段 ───────────────────────────────────────────
    //
    // max_instructions_per_run:
    //   通过 lua_sethook(L, hook, LUA_MASKCOUNT, N) 每 N 条指令强制 hook 抬
    //   error, 用于终结 `while true do end` 死循环. 0 = 不启 hook (谨慎).
    uint32_t max_instructions_per_run = 1000000u;

    // max_memory_bytes:
    //   通过 lua_setallocf 拦截, 每次分配前累计跟踪, 超过则 alloc 返 NULL
    //   → Lua 5.4 raises "not enough memory" error 让脚本自然崩掉. 0 = 无限.
    uint64_t max_memory_bytes = 0;

    // 白名单模块开关 (在 restricted _ENV 里是否放入对应 table).
    //   allow_string_lib : string.* (safe subset, string.dump 无论如何拒)
    //   allow_math_lib   : math.*
    //   allow_table_lib  : table.*
    bool allow_string_lib = true;
    bool allow_math_lib   = true;
    bool allow_table_lib  = true;

    // 追加黑名单. deny_specific_globals 指向数组, 每个元素是一个 UTF-8 C 字
    // 符串 (对应全局名).  arm 时会把这些名字从 restricted _ENV 里移除 (无论
    // 该名字之前是否列入白名单).  由调用方保证生命周期.
    const char* const* deny_specific_globals = nullptr;
    size_t              deny_specific_globals_count = 0;
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_arm(lua_State* L,
                                const lua_sandbox_config* cfg);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_disarm(lua_State* L);

// 查询当前 tick 累计的 alloc bytes (调试用).  未 arm 的 L 返 0.
extern "C" SAO_PLUGINS_API uint64_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_bytes_used(lua_State* L);

// 判断沙盒是否 armed. 单纯用于 test / debug.
extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_is_armed(lua_State* L);

} // namespace sao::plugins::lua_host
