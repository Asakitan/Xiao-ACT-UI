# lua_host/ — Lua 5.4 真 C API 嵌入

**职责**: 用真 Lua 5.4 C API 提供 Lua 插件运行时。免 Python 侧 lupa 双跳,
免 Lua 5.5 beta 属性缓存 bug。

## 升级说明 vs Python 平台

Python 侧 ``lua_runtime.py`` 走:
```
Python plugin.py → PluginContext (Python obj) → lupa Lua proxy → LuaJIT / Lua 5.x
```

而且 lupa 顶层默认绑定最新 Lua (当前 5.5 beta), 其 Python 对象属性缓存 bug
逼我们锁到 lua54 子模块 (见项目记忆 project_script_plugin_ui_pitfalls)。

C++ 侧:
```
C++ ctx → lua_State (真 5.4) → 插件 lua script
```

省一跳, 直接用官方稳定 5.4。

## 对齐的 Python 源

- ``act_platform/scripting/lua_runtime.py`` (_LuaProxy: 每个 ctx 方法手动
  包 Lua callback → C++ 侧改成用 lua_ref + closure 存 Lua fn ref)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``lua_host.h`` | lua_State 生命周期 (luaL_newstate + custom allocator) |
| ``lua_module_bridge.h`` | luaL_register 注册 ctx 全局表 |
| ``lua_call.h`` | plugin.lua 加载 + hook 分派 |
| ``lua_sandbox.h`` | _ENV 白名单沙箱 |
| ``lua_stdlib.h`` | 按沙箱选装 io/os/package |
| ``lua_error.h`` | 栈顶错误串 + debug.traceback → SAO_STATUS |

## 关键设计

**每插件独立 lua_State**: 一个插件的 syntax error / stack corruption 不影响
别的 plugin。

**冒号 vs 点号方法**: C++ 侧插件 ``.lua`` 里应该用 ``ctx:log("hi")`` (方法调
用, 隐式传 self); Python 侧 lupa 因为把 ctx 当 Python object 用双 syntax。
新平台推荐**统一冒号**, 老 plugin.lua 里的 ``ctx.log(...)`` 通过 metatable
的 ``__index`` 转发也能跑 (compat 层)。

## vcpkg 依赖

- ``lua`` (5.4)

## 测试

见 ``tests/``。
