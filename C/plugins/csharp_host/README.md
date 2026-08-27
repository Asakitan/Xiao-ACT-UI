# csharp_host/ — .NET hostfxr + Roslyn

**职责**: 用 hostfxr 嵌入 .NET runtime, Roslyn 编译 .cs 源, 提供 C# 插件运行时。

## 升级说明 vs Python 平台

Python 侧走 pythonnet:
```
Python plugin.py → pythonnet → CLR (mono / .NET Framework / .NET Core)
```

依赖 Python 装 pythonnet + .NET runtime。

C++ 侧直接走 hostfxr (.NET 8 官方原生嵌入 API):
```
C++ → hostfxr → coreclr → 每插件独立 AssemblyLoadContext
```

免 pythonnet 双跳, 免 Python 依赖 (于 C# 插件)。

## 对齐的 Python 源

- ``act_platform/scripting/csharp_runtime.py`` (Roslyn 编译 / assembly 加载
  语义参考; 实现底层不同)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``cs_host.h`` | hostfxr + coreclr 生命周期; 每插件 AssemblyLoadContext |
| ``cs_compile.h`` | Roslyn 编译 .cs → .dll |
| ``cs_call.h`` | 反射 hook 调用 |
| ``cs_module_bridge.h`` | NativeInterop 注入 ctx 到托管 |
| ``cs_sandbox.h`` | ALC 隔离 + Reflection.Emit 白名单 |
| ``cs_error.h`` | CLR exception + Roslyn diagnostics → SAO_STATUS |

## 插件作者的 C# 姿势

```csharp
using SAO;   // 平台提供的托管侧 SDK wrapper

public class Plugin
{
    public void OnLoad(dynamic ctx)
    {
        ctx.log("Hello from C#!");
        ctx.register_ui_panel("cs_panel",
            new Dictionary<string, object> { { "title", "C# Panel" } },
            new Func<object, object>(Render),
            new Func<string, object, object>(OnAction));
    }

    public object Render(object payload)
    {
        return ((dynamic)_ctx).ui.panel("C# Plugin", new object[] {
            ((dynamic)_ctx).ui.text("Hello from C#!"),
        });
    }
}
```

`dynamic ctx` 让 C# 插件的姿势和 Python 一致 (免大量 wrapper 类)。这一
点通过 SDK 侧的 DynamicObject 提供。

## vendor 依赖

见 ``vendor_note.md``。hostfxr + Roslyn 从 .NET 官方拉, 不进 vcpkg。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除；此前依赖 .NET runtime 的集成结果仅作历史证据。
