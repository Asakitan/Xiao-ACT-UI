# csharp_host — .NET vendor 依赖

## hostfxr

Windows .NET 8+ runtime 自带 hostfxr.dll。位置:
```
C:\Program Files\dotnet\host\fxr\<version>\hostfxr.dll
```

**离线随包**方案 (推荐给冻结态):
```
runtime/dotnet-embed/
    hostfxr.dll
    hostpolicy.dll
    coreclr.dll
    System.*.dll
    runtimeconfig.json
```

从 .NET SDK 安装目录复制过来即可。**注意版本必须 >= 8.0**。

## Roslyn 编译器

用作插件源 → dll 编译。走 NuGet 包 ``Microsoft.CodeAnalysis.CSharp``:
```
Microsoft.CodeAnalysis.dll
Microsoft.CodeAnalysis.CSharp.dll
System.Collections.Immutable.dll
System.Reflection.Metadata.dll
System.Text.Encoding.CodePages.dll
```

随包放在 ``runtime/roslyn-embed/`` (对齐 python fetch_roslyn 的
``scripting/roslyn/`` 目录布局)。

## 参考程序集 (reference assemblies)

Roslyn 编译要引 System.dll / netstandard.dll 的 reference assemblies:
```
runtime/dotnet-embed/ref/
    System.Runtime.dll
    System.Collections.dll
    ...
```

从 .NET SDK 的 ``packs/Microsoft.NETCore.App.Ref/<ver>/ref/net<ver>/``
复制。

## 跳过策略

如果冻结态发布不带 .NET runtime, csharp_host 检测到 hostfxr 缺失时:
```
sao_plugins_cshost_init → SAO_ERR_NOT_INITIALIZED
```

上层 loader 记录到 last_error 里, 该插件保持 disabled 状态, 不影响其他
语言插件。UI 上给出 "该插件需要 .NET 8 runtime, 请安装" 提示。
