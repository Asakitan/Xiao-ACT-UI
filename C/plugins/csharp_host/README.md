# csharp_host/ — 按需 .NET hostfxr 与预编译托管组件

**职责**: 用 hostfxr 加载预编译托管 DLL，并连接组件入口与原生 SDK 函数表。
`.cs` 原生子集由同级 `csmini/` 执行；当前没有 Roslyn 源编译实现。

## 升级说明 vs Python 平台

Python 侧走 pythonnet:
```
Python plugin.py → pythonnet → CLR (mono / .NET Framework / .NET Core)
```

依赖 Python 装 pythonnet + .NET runtime。

C++ 侧直接走 hostfxr (.NET 8 官方原生嵌入 API):
```
C++ → 按需 hostfxr → coreclr → 预编译组件入口 + SDK table
```

免 pythonnet 双跳, 免 Python 依赖 (于 C# 插件)。

## 对齐的 Python 源

- ``act_platform/scripting/csharp_runtime.py`` (Roslyn 编译 / assembly 加载
  语义参考; 实现底层不同)

## 接口与实现边界

| 文件 | 职责 |
|------|------|
| ``cs_host.h`` | hostfxr 初始化、句柄与关闭 |
| ``src/cs_component.cpp`` | DLL、runtimeconfig、managed_type 与组件 hook 解析/调用 |
| ``cs_compile.h`` | 保留的源编译 ABI；当前返回未实现 |
| ``cs_call.h`` / ``cs_module_bridge.h`` | 调用与模块桥入口，具体支持面以对应实现为准 |
| ``src/cs_sdk_bridge.cpp`` | 带版本/大小的 SDK table 与托管回调桥 |
| ``cs_sandbox.h`` | 接口保留，不作为任意托管代码的进程隔离保证 |
| ``cs_error.h`` | 错误状态与文本 |

## 插件入口与发现

原生 `.cs` 静态 hooks 示例见 `../examples/example_csharp_plugin/`；托管组件见
`../examples/hello_csharp/`，manifest 指向预编译 `.dll`，并指定 `managed_type`，
runtimeconfig 默认按 entry 同名推导。托管包装层须遵守组件入口/SDK table 契约，
普通 `dynamic ctx` 源码并不自动成为可加载组件。

生产 `csmini` composite 注册只保存配置，实际托管加载才初始化本宿主。
显式 dotnet_root 仅在该根下查找；空根按环境/系统默认路径探测。解析器按数字版本
挑选包含普通 hostfxr.dll 文件的最高版本；布局存在不表示 runtime/framework/架构
匹配。缺运行库的有效托管插件由 launcher 延期，坏源文件/清单仍是插件失败。

session-81 审阅补修后 Debug provider888/0 包含：默认与有效配置下原生源码无 .NET DLL、
无执行路由查询、缺 .NET 的托管插件及依赖者延期、现有 HelloPlugin.dll 实际激活
时才加载 .NET，以及正常卸载/shutdown。host 句柄关闭不声明 CoreCLR DLL 已从
进程物理卸载；未跑全量产品、发布或 CoreCLR-off 配置。Review 在 composite 分类侧
补查 metadata 根/stream 范围，新增四种损坏 DLL 查询已由主会话重建复跑通过；hostfxr 文件解析器不变。

## vendor 依赖

运行预编译托管插件需要兼容的 .NET runtime；原生 csmini 不需要它。
本路径不自动下载 runtime/SDK 或启动 Roslyn，历史材料见 ``vendor_note.md``。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除；此前依赖 .NET runtime 的集成结果仅作历史证据。
