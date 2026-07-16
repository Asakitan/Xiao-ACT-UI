# example_python_plugin — Python 插件示例骨架

## 用法

放进 ``plugins/`` 或 ``user_plugins/`` 目录, 平台启动时自动扫描发现。默认
``enabled: false``, 用户在 UI 里点开启用。

## 需要的宿主

- ``python_host`` (CPython 3.11+ 嵌入; ``SAO_PLUGINS_ENABLE_PYTHON=ON``)

## SDK 调用一览

| ctx 方法 | 语义 |
|----------|------|
| ``ctx.log(msg)`` | 打日志 |
| ``ctx.set_defaults({...})`` | 设置默认值 (仅当 setting 未设时) |
| ``ctx.get_setting(key, default)`` | 读设置 |
| ``ctx.set_setting(key, value)`` | 写设置 |
| ``ctx.register_ui_panel(id, meta, render, on_action)`` | 注册 UI 面板 |
| ``ctx.register_hotkey(id, cb, key, label)`` | 注册全局热键 |
| ``ctx.request_redraw(surface)`` | 请求重绘 |
| ``ctx.notify(title, msg, duration_s, kind)`` | 通知气泡 |
| ``ctx.subscribe(topic, cb)`` | 事件订阅 |
| ``ctx.emit(topic, payload)`` | 事件发射 |
| ``ctx.ui.panel(title, children)`` | UI 面板构造 |
| ``ctx.ui.section(title, children)`` | UI section |
| ``ctx.ui.kv(k, v)`` | key-value 行 |
| ``ctx.ui.row([...])`` | 横向排 |
| ``ctx.ui.button(label, action_id, style)`` | 按钮 |

## 从 Python 平台迁移

直接把老 ``python/plugins/<plugin>/`` 目录复制过来即可, ``plugin.json`` +
``plugin.py`` 都 1:1 兼容。
