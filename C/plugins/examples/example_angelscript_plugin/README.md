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

因为 AngelScript 静态类型限制, ctx 方法用后缀区分类型:
- ``get_setting_str(key, default)`` — 返回 string
- ``get_setting_int(key, default)`` — 返回 int
- ``get_setting_bool(key, default)`` — 返回 bool
- ``ui_panel(title, array<UiNode@>&)`` — 返回 UiNode@
- ``ui_section(title, ...)``
- ``ui_kv(key, value)``
- ``ui_button(label, action_id, style)``
- ``notify(title, message, duration_s)`` — void

Python 平台的 ``.as`` 子集脚本 (无强类型) 也能加载, 但享受不到静态类型
的编译期错误检查。

## 从 Python 平台迁移

``python/plugins/example_angelscript_plugin/plugin.as`` 可直接搬, 但如果
用了 ``dictionary`` 弱类型 (Python 侧手写解释器允许), 建议改成 typed
版本以享受编译期检查。
