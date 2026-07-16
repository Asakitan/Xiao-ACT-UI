# example_csharp_plugin — C# 插件示例骨架

## 需要的宿主

- ``csharp_host`` (hostfxr + Roslyn; ``SAO_PLUGINS_ENABLE_CSHARP=ON``,
  运行时 .NET 8+ runtime)

## 两种入口模式

### 1. 源代码模式 (推荐, 用户零编译)

```
plugin.cs        <- Roslyn 内存编译
plugin.json      <- language: "csharp", entry: "plugin.cs"
```

Roslyn 编译器 DLL 随包 vendor (见 ``../../csharp_host/vendor_note.md``),
用户不需要装 .NET SDK。

### 2. 预编译 DLL 模式 (作者高级用法)

```
plugin.dll       <- 作者本地已编译
plugin.json      <- language: "csharp", entry: "plugin.dll"
[references/]    <- 额外引用的第三方 DLL
```

宿主直接 ``LoadAssembly``, 免 Roslyn 步骤。适合需要第三方 NuGet 包的
复杂插件。

## dynamic ctx 姿势

C# 的 dynamic 让 ctx 用起来和 Python 一样:

```csharp
_ctx.log("hi");
_ctx.register_ui_panel("id", meta, renderFn, actionFn);
_ctx.notify("title", "msg", 3.0);
```

免大量 wrapper 类 (类型安全的 typed SDK 也提供, 见 ``SAO.Plugin`` 静态类)。

## 从 Python 平台迁移

``python/plugins/example_csharp_plugin/plugin.cs`` 直接搬。pythonnet 侧
用的 dynamic 语义在 hostfxr 侧完全一致。

## 依赖

- ``.NET 8+ runtime`` (随包或用户系统装; 见 vendor_note.md)
- Roslyn 编译 (源代码模式) 或 已编译 .dll (预编译模式)
