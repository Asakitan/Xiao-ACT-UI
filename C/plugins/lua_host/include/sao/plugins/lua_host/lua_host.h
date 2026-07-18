// lua_host.h — Lua 5.4 真 C API 嵌入
//
// **重要升级说明**: Python 侧走 lupa (Python ↔ Lua 双跳), 而且因 lupa 顶层
// 默认绑定 Lua 5.5 beta 有属性缓存 bug 要手动锁到 lua54 子模块 (见项目记忆
// project_script_plugin_ui_pitfalls)。C++ 侧直接用真 Lua 5.4 C API, 免双跳
// 免属性缓存 bug。
//
// 对齐 Python 源: lua_runtime.py (_LuaProxy 类似 stack-based ctx 转发)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"
#include "sao_plugins/sao_status.h"
#include "sao/plugins/lua_host/lua_error.h"

// Lua 前置声明
struct lua_State;

namespace sao::plugins::lua_host {

typedef struct lua_host_s* lua_host_handle_t;
typedef struct lua_loader_adapter_owner_s* lua_loader_adapter_owner_t;

struct lua_host_config {
    // 是否装 Lua 官方标准库 (base / string / table / math / io / os / package)
    // 默认 false, 手动按 permissions 装
    bool install_stdlib = false;
    // 消息 (print / error) 回调
    void (*message_callback)(const char* utf8, int is_error, void* ud) = nullptr;
    void* callback_user_data = nullptr;
    // adapter 为每插件 state 应用的默认资源限制。0 表示关闭对应限制。
    uint32_t max_instructions_per_run = 1000000u;
    uint64_t max_memory_bytes = 0;
};

// 创建 lua_State；adapter 随后按 manifest permissions 安装 stdlib 和 sandbox。
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

// 执行 UTF-8 Lua 源码并复制首个返回值。所有 char* 输出统一由
// sao_plugins_luahost_free_string 释放。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_execute(lua_host_handle_t host,
                            const char* source_utf8,
                            size_t source_len,
                            char** out_result_utf8,
                            char** out_error_utf8);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function(lua_host_handle_t host,
                                  const char* fn_name,
                                  char** out_result_utf8,
                                  char** out_error_utf8);

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_is_available(void);

extern "C" SAO_PLUGINS_API void SAO_PLUGINS_CALL
sao_plugins_luahost_free_string(char* value);

// 注册 engine_kind::lua 的 production loader adapter。每个插件独占一个
// lua_State；注销前必须先经 loader 卸载全部插件。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_register_loader_adapter(
    const lua_host_config* cfg,
    lua_loader_adapter_owner_t* out_owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_unregister_loader_adapter(
    lua_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_luahost_loader_adapter_plugin_count(
    lua_loader_adapter_owner_t owner);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_loader_adapter_get_last_error(
    lua_loader_adapter_owner_t owner,
    void* loader_plugin_handle,
    char** out_utf8);

// 对 adapter 管理的插件调用 generic hook；该入口持有 active-call lease。
extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_loader_adapter_call_hook(
    lua_loader_adapter_owner_t owner,
    void* loader_plugin_handle,
    const char* hook_name,
    const char* args_json_utf8,
    char** out_result_json_utf8);

} // namespace sao::plugins::lua_host
