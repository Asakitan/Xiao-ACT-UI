// lua_sandbox.h — Lua 沙箱 (设置 _ENV 白名单)
//
// Lua 5.2+ 用 _ENV 环境表限制脚本可见变量。这一层为脚本设一个安全 env, 只
// 白名单 string/table/math/tostring/tonumber/ipairs/pairs/print 等; 拒
// io/os/package/require (除非 allow_process=true)。
//
// 对齐 Python 源: lua_runtime.py 没做真沙箱, C++ 侧加固为可选。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

struct lua_State;

namespace sao::plugins::lua_host {

struct lua_sandbox_config {
    bool allow_fs = false;
    bool allow_net = false;
    bool allow_process = false;
    bool allow_require = false;   // require() → 拒或允许 (前者更安全)
    bool allow_coroutine = true;
};

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_arm(lua_State* L,
                                const lua_sandbox_config* cfg);

} // namespace sao::plugins::lua_host
