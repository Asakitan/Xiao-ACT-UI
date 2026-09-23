# example_csharp_plugin — 原生 csmini 最小示例

manifest 固定 `language: "csharp"`、`cs_runtime: "csmini"`、`entry: "plugin.cs"`。
需要构建 `SAO_PLUGINS_ENABLE_CSMINI=ON`；运行此示例无需 .NET、hostfxr、Roslyn 或 dotnet SDK。

## 入口与生命周期

- `Plugin` 类暴露静态 `OnLoad(PluginContext ctx)`、`OnEnable()`、`OnDisable()`、`OnUnload()`。
- `OnLoad` 保存 ctx，读取 `greeting`（缺省 `你好`），写回该值并记录日志。
- `OnUnload` 返回 `true` 允许卸载；返回 `false` 表示否决。
- 本例只演示日志、设置和生命周期，不声明 UI 面板、菜单动作或计时器。

`PluginContext` 和 `dynamic` 由 csmini 子集解释器处理，不代表此源文件可直接作为普通托管项目编译。
API 依据 `csmini/src/csmini_host.cpp` 的 hook 分派与 `csmini/src/csmini_ctx.cpp` 的方法注册；这不是完整 C#/.NET 兼容声明。

## 托管 C# 是独立运行模式

需要 CLR 的插件选择 `cs_runtime: "coreclr"`，提供真实预编译 DLL、runtimeconfig 和 assembly-qualified `managed_type`。
相邻 `hello_csharp` 保留该模式的项目与入口示例；当前通用托管加载器不自动编译 `.cs`。
默认 `enabled: false`；在配置的插件根中发现后，由用户启用。
