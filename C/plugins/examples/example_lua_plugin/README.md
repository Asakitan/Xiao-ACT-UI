# example_lua_plugin — Lua 插件示例骨架

## 需要的宿主

- ``lua_host`` (真 Lua 5.4 C API; ``SAO_PLUGINS_ENABLE_LUA=ON``, vcpkg lua)

## Lua 姿势

ctx userdata 方法使用冒号：`ctx:log("hi")`（自动传 self）；`ctx.ui` 构造器使用点号。
本例在停用和直接卸载时都清理计时器并重置本地状态。

## SDK 命名

Lua 侧 ctx 方法完全同名, 都走 self 参数:

```lua
ctx:log(msg)
ctx:register_ui_panel(id, meta, render_fn, action_fn)
ctx:register_hotkey(id, cb, "F6", "label")
ctx:notify(title, msg, duration, kind)
ctx:subscribe(topic, cb)
ctx:emit(topic, payload)
ctx:set_interval(cb, seconds)
ctx:set_timeout(cb, seconds)
ctx:clear_timer(token)
ctx:get_setting(key, default)
ctx:set_setting(key, value)
ctx:request_redraw(surface)
```

``ctx.ui`` 是子表, 用点号:

```lua
ctx.ui.panel(title, children)
ctx.ui.section(title, children)
ctx.ui.kv(k, v)
ctx.ui.row({...})
ctx.ui.button(label, action_id, style)
```

## 从 Python 平台迁移

迁移时核对冒号调用、回调参数及宿主提供的模块；旧 lupa 插件不视为自动兼容。
