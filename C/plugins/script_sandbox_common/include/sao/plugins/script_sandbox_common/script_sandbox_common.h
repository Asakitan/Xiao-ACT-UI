// script_sandbox_common.h — 脚本沙箱共享支持层
//
// 独立 STATIC lib, 为 Lua sandbox / AS sandbox test binary 提供共享 utility:
//   * denied_globals_contains(names, count, needle) — 无 std::span/std::view
//     依赖, C 字符串数组通用 lookup, sandbox test 里也复用.
//   * kScriptSandboxCommonTag — 供 support lib 消费方确认已真链上的 tag 值.
//
// 命名空间 sao::plugins::script_sandbox_common.
//
// **不含** Lua 或 AS SDK 头 — 让此 lib 在 SAO_HAS_LUA / SAO_HAS_ANGELSCRIPT
// 都关掉时仍可编.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sao::plugins::script_sandbox_common {

// 查询 (const char* const* names, count) 里是否含 needle (strcmp 语义).
// 返 true 表 found; 空/nullptr 都返 false.
bool denied_globals_contains(const char* const* names, size_t count,
                             const char* needle) noexcept;

// 一个简易 tag 值, 供 support lib 消费方确认 support lib 真链上了.
extern const uint32_t kScriptSandboxCommonTag;

} // namespace sao::plugins::script_sandbox_common
