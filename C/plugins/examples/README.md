# examples/ — 5 种引擎的最小示例插件骨架

**不参与 build** —— 顶层 ``plugins/CMakeLists.txt`` **没有** ``add_subdirectory(examples)``。
这些子目录只作为**运行时被 ``loader/plugin_scanner`` 扫描的骨架**, 让插件作者
对齐目录布局。

## 5 个示例

| 目录 | 语言 | 对应 host | 对应 Python 示例 |
|------|------|-----------|-----------------|
| ``example_python_plugin/`` | Python | python_host | ``python/plugins/example_*_plugin`` (无, 用 hide_seek 类比) |
| ``example_emma_plugin/`` | Emma | emma_host | ``python/plugins/example_emma_plugin`` |
| ``example_angelscript_plugin/`` | AngelScript | angel_host | ``python/plugins/example_angelscript_plugin`` |
| ``example_lua_plugin/`` | Lua | lua_host | ``python/plugins/example_lua_plugin`` |
| ``example_csharp_plugin/`` | C# | csharp_host | ``python/plugins/example_csharp_plugin`` |

每个示例插件目录包含:
- ``plugin.json`` — 1:1 兼容 Python 平台的 manifest 格式
- ``plugin.<ext>`` — 入口脚本, 演示 SDK 调用: register_ui_panel / subscribe /
  register_hotkey / notify / register_timer
- ``README.md`` — 语言特定的写法说明

## 历史手工观察流程（2026-08-23 测试删除前）

旧流程曾把 ``plugins/examples/`` 加进 ``scan_config.builtin_roots``，再观察5个sample plugin
的发现/加载/UI/hotkey行为。该流程仅作历史记录，不是当前测试或产品运行指令。

## 交叉验证

同一份 ``plugin.emma`` (或 ``.lua`` / ``.as``) 应该在 Python 平台和 C++ 平台
跑出等价 UI panel。这是 "旧插件无痛迁移" 的核心承诺。
