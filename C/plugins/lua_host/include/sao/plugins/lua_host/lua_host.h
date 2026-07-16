// lua_host.h — Lua 5.4 真 C API 嵌入
//
// **重要升级说明**: Python 侧走 lupa (Python ↔ Lua 双跳), 而且因 lupa 顶层
// 默认绑定 Lua 5.5 beta 有属性缓存 bug 要手动锁到 lua54 子模块 (见项目记忆
// project_script_plugin_ui_pitfalls)。C++ 侧直接用真 Lua 5.4 C API, 免双跳
// 免属性缓存 bug。
//
// 对齐 Python 源: lua_runtime.py (_LuaProxy 类似 stack-based ctx 转发)。
#pragma once

#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"

// Lua 前置声明
struct lua_State;

namespace sao::plugins::lua_host {

typedef struct lua_host_s* lua_host_handle_t;

struct lua_host_config {
    // 是否装 Lua 官方标准库 (base / string / table / math / io / os / package)
    // 默认 false, 手动按 permissions 装
    bool install_stdlib = false;
    // 消息 (print / error) 回调
    void (*message_callback)(const char* utf8, int is_error, void* ud);
    void* callback_user_data = nullptr;
};

// 创建 lua_State (luaL_newstate + custom allocator, 便于统计 mem 分配)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_create(const lua_host_config* cfg, lua_host_handle_t* out_host);

// 释放 (lua_close)。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_destroy(lua_host_handle_t host);

// 拿底层 lua_State* (给 sdk_binding/binding_lua 用)。
extern "C" SAO_PLUGINS_API lua_State* SAO_PLUGINS_CALL
sao_plugins_luahost_state(lua_host_handle_t host);

// 版本 (LUA_RELEASE)。
extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_luahost_version(void);

} // namespace sao::plugins::lua_host
