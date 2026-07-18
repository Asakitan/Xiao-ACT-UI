// lua_error.h — Lua 错误 → SAO_STATUS
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host {

// 从栈顶弹出错误并复制 UTF-8 文本。成功输出统一由
// sao_plugins_luahost_free_string 释放。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_take_error(lua_State* L, char** out_utf8);

} // namespace sao::plugins::lua_host
