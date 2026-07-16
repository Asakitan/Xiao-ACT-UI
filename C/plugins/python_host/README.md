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

- ``Python3::Python`` (vcpkg python3, embedded 版本; 或直接用 Windows 官方
  embeddable package)
- 见 ``vendor_note.md`` —— 嵌入式 CPython 分发从哪拿

## 测试

见 ``tests/``。集成测试要求真实 CPython 存在, 否则跳过。
