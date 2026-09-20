# python_host/ — 嵌入式 CPython 3.11+

**职责**: 让平台把 Python 插件也当成一种脚本语言引擎处理, 而不是特殊化对待。

## 对齐的 Python 源

- ``act_platform/plugins.py`` (Python 分支: ``record.language == "python"``)
- ``act_platform/scripting/base.py`` (ScriptRuntime 抽象 —— 这里 Python 也算作一种)

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``py_host.h`` | Py_Initialize / Py_Finalize 生命周期 |
| ``py_module_bridge.h`` | 手写 PyModuleDef 注册 ``_sao_plugin_native`` |
| ``py_call.h`` | plugin.py 加载 + hook 调用 |
| ``py_sandbox.h`` | 沙箱 (import 白名单等) |
| ``py_error.h`` | PyErr → SAO_STATUS 转换 |

## 关键设计

**不用 pybind11**: SDK 只暴露 ~20 个 C ABI 方法, 用 pybind11 拉几十 MB 的依赖不划
算。手写 PyModuleDef + PyMethodDef 更清晰, 也让插件作者不需要装 pybind11
才能引用 native 类型。

**每插件独占 PyThreadState**: 避免 GIL 争抢, 允许 N 个 Python 插件并发跑
(即使 GIL 只有一个, 至少调度粒度小一点)。

**旧插件无痛跑**: py_module_bridge.h 里的 ``sao_plugins_pyhost_register_shim_module``
装了个 ``act_platform.plugins`` 假模块, 老插件的 ``from act_platform.plugins import
PluginContext`` 直接可用 → 无需改一个字迁移。

## vcpkg / vendor 依赖

- 构建时需要 CPython 3.11+ `Development.Embed` headers；MSVC 下只读取
  `Python3::Python` 的 include 目录，不把 `python3xx.lib` 依赖传播到 launcher。
- `py_import_stubs.cpp` 在配置的 `python_home` 内加载 `python3xx.dll`，解析完整
  required-export 表后才允许初始化；缺任一导出即 fail closed。`SaoAuto` payload
  因此不静态导入 `python311.dll`，没有安装 CPython 的机器仍可启动。
- CMake 找不到 headers 时模块仍可构建为 unavailable host，运行时返回明确状态，
  不下载 runtime。
- 见 ``vendor_note.md`` —— 嵌入式 CPython 分发从哪拿

## Production runtime acceptance

- `python_home` 为空时状态为 `UNCONFIGURED`；路径存在但 runtime probe失败为
  `UNAVAILABLE`；host未构建为 `HOST_UNAVAILABLE`；只有DLL加载、required exports与
  host初始化条件全部满足才发布 `READY`。
- `py_runtime: "cpython"` 的插件只在 `READY` 时进入CPython adapter；没有READY
  runtime时保持deferred/fail-closed。`auto`或`pymini`插件继续走本机C++ pymini，
  子集外能力返回明确`UNSUPPORTED`，不静默下载或替换runtime。
- Launcher把上述Python状态显示为built-in Plugins状态行；Python状态本身不改变overall
  plugin runtime operational status。只有整体plugin runtime不是`READY`时，Entity
  publication才使用空loader catalog并清除旧revision/content token/published catalog，
  防止上一次generation的插件action继续可调用。

当前live provider probe曾完成CPython 3.11.0 init→version→shutdown；这是configured
runtime生命周期证据，不代表任意CPython版本或第三方package都已验收。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除；此前依赖真实 CPython 的集成结果仅作历史证据。
