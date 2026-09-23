# examples/ — 五种插件语言与运行模式示例

**不参与 build** —— 顶层 ``plugins/CMakeLists.txt`` **没有** ``add_subdirectory(examples)``。
这些子目录只作为**运行时被 ``loader/plugin_scanner`` 扫描的骨架**, 让插件作者
对齐目录布局。

## 5 个示例

| 目录 | 语言 | 对应 host | 功能或来源 |
|------|------|-----------|-----------------|
| ``example_python_plugin/`` | Python | python_host，显式 `py_runtime: "cpython"` | 面板、设置、快捷键 |
| ``example_emma_plugin/`` | Emma | emma_host | ``python/plugins/example_emma_plugin`` |
| ``example_angelscript_plugin/`` | AngelScript | angel_host | ``python/plugins/example_angelscript_plugin`` |
| ``example_lua_plugin/`` | Lua | lua_host | ``python/plugins/example_lua_plugin`` |
| ``example_csharp_plugin/`` | C# | csmini，显式 `cs_runtime: "csmini"` | 静态生命周期、日志、设置 |

每个示例插件目录包含:
- ``plugin.json`` — manifest、入口及运行模式；五个主要示例均带 `zh-CN` / `en-US` 名称
- ``plugin.<ext>`` — 入口脚本；具体功能以各例源码为准
- ``README.md`` — 语言特定的写法说明

## 历史手工观察流程（2026-08-23 测试删除前）

旧流程曾把 ``plugins/examples/`` 加进 ``scan_config.builtin_roots``，再观察5个sample plugin
的发现/加载/UI/hotkey行为。该流程仅作历史记录，不是当前测试或产品运行指令。

## 语言身份与运行模式

`language` 的五种身份为 `python`、`lua`、`emma`、`angelscript`、`csharp`。
Python 的 `py_runtime` 可选 `cpython` / `pymini` / `auto`；C# 的 `cs_runtime`
可选 `coreclr` / `csmini` / `auto`，这些不是额外语言身份。
pymini/csmini 为原生子集解释器；`auto` 的源码预检不是完整语言或全部 ctx API 的兼容证明。
Lua/Emma/AngelScript 使用各自已构建宿主；运行能力与 manifest 显示名称的本地化分别验证。
`locales` 元数据也不自动翻译脚本内硬编码的面板文字和日志。

## 托管与历史辅助示例

`hello_csharp` 保留为显式 `coreclr` 预编译示例：入口为 `prebuilt/HelloPlugin.dll`，
`managed_type` 为 `SaoAuto.Plugins.HelloCsharp.HelloPlugin, HelloPlugin`，
runtimeconfig 为同目录的 `HelloPlugin.runtimeconfig.json`。
需要真实 DLL、其依赖和匹配 runtime；目录名或 `.cs` 源文件不替代预编译产物。
`HelloPlugin.csproj` 定义作者侧构建，宿主只加载产物，不自动执行 Roslyn 编译。
其 `public static int Hook(IntPtr, int)` ABI 与 csmini 静态脚本 hooks 不同。
`hello_angel` 是自由函数兼容面历史示例；两种 hello 示例都不代表完整 UI 验收。

## 验证边界

本轮仅按当前宿主源码核对 manifest、方法声明和生命周期并修正示例，未运行构建或探针。
集成方应逐个在隔离插件根验证发现、加载、启用、停用、卸载和 shutdown，
再检查面板动作、计时器停用后重启，以及中英文 manifest 名称；托管例需单独验证预编译产物。
