# example_angelscript_plugin — AngelScript 插件示例骨架

## 需要的宿主

- ``angel_host`` (真 asIScriptEngine 嵌入; ``SAO_PLUGINS_ENABLE_ANGEL=ON``,
  vcpkg unofficial-angelscript)

## AngelScript 特点 vs Python 版本

C++ 平台用真 SDK, 因此你能:
- ``class MyClass { ... }`` 真类定义
- ``dictionary`` / ``array<T>`` 强类型容器
- ``@`` handle 引用计数
- ``PluginContext@ ctx`` 静态类型 ctx

## SDK 命名

当前绑定以 `angel_host/src/as_ctx_surface.cpp` 和 `as_stdlib.cpp` 为准：
- `ctx.get_setting(key, default)` 返回 `json@`，其 `opImplConv` 支持赋给 string 等标量。
- `ctx.ui.panel(title, children)`、`section`、`row`、`kv`、`button` 返回 `dictionary@`。
- UI 子节点使用 `array<dictionary@>`，不是 `UiNode`。
- `ctx.notify(title, message, duration_s)` 返回 bool。
- `on_load(PluginContext@ c)` 保存 ctx；其余生命周期 hook 为无参数自由函数。

## 从 Python 平台迁移

旧脚本须按真实 AngelScript 编译器和当前 ctx 注册声明核对语法、类型及回调签名；
旧解释器接受的写法不代表本宿主全部接受。本例保留面板、问候与计数功能；运行效果由实际宿主验证。
