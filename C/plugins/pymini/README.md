# pymini/ — 纯 C++ Python 子集解释器 (legacy `plugin.py` 免 CPython)

**职责**: 让 legacy Python 插件 (如 `plugin.py` + `ctx.*`) 在**不依赖
CPython/Python SDK/python3.dll/pip** 的前提下运行在 C++ 插件系统上。
我们自己解析其插件结构与支持的 Python 子集语法, 映射到当前 C++ SDK /
Plugin API, 由 C++ 原生执行。

```
legacy plugin.py → pymini lexer/parser → AST (tree-walk interpreter)
                 → ctx 对象绑 loader::plugin_context_t (C ABI)
```

## 与 cpython 委托的复合路由

`pymini` 是 `engine_kind::python` 的默认生产适配器 (composite):

| manifest `py_runtime` | 路由 |
|---|---|
| `"pymini"` | 本解释器原生执行 |
| `"cpython"` | 委托 `sao_plugins_pyhost_*` (仅当 cpython runtime 可用) |
| 缺失 / `auto` | 源码预检: 子集内 → pymini; 子集外 → pyhost 委托; 无 pyhost → `SAO_PLUGINS_ERR_UNSUPPORTED` + 特性名 |

`pymini_adapter_config::python_home` 非空才启用 cpython 委托;
为空时 cpython/子集外路由直接 fail-closed。

## 管线

| 阶段 | 文件 |
|------|------|
| 词法 | `pymini_lexer.{h,cpp}` — INDENT/DEDENT 缩进块 tokenizer |
| 语法 | `pymini_parser.{h,cpp}` — 递归下降 → AST; `src_pos` 追踪诊断 |
| 值系统 | `pymini_value.{h,cpp}` — `PyObj`/`PyRef`, dict/list/tuple/class/module/exception/file/thread |
| 求值 | `pymini_interp.{h,cpp}` + `pymini_ops`/`pymini_calls`/`pymini_ast.h` |
| 内建 | `pymini_builtins.cpp`, `pymini_stdlib{,2}.cpp` — builtins + `os`/`os.path`/`sys`/`json`/`math`/`time`/`re`/`base64`/`hashlib`/`random`/`shutil`/`io`/`threading`/`typing` 等子集 |
| ctx 桥 | `pymini_ctx.cpp` — `ctx.*` 对象 → `sao_plugins_ctx_*` C ABI；`ctx.engine`/​`ctx.engine_call` 走反射引擎面：惰性绑定 per-plugin `SaoSdkContext`（`sao_sdk_context_create` + `bind_platform_services`）后经 `sao_plugins_sdk_context_dispatch`（`method_engine_call`/`method_engine_list`）派发，`engine.on(channel, cb)` 注册 `engine_callback` 通用通道 |
| 宿主编排 | `pymini_host.cpp`, `pymini_bridge.cpp`, `pymini_loader_adapter.cpp` |
| 导入 | `pymini_imports.cpp` — `import`/`from x import y`, `sys.path` 根 (`plugin_root` + `module_dirs`) |
| 公共 | `pymini_common.h` — `py_error{code,kind,message,pos,file,features}`、`src_pos` |

## 范围模型 / 控制流

- LEGB：每个函数体运行在新建 locals dict; 闭包捕获外层 locals dict 链
  (`captured`)。`global` 写 module dict，`nonlocal` 搜索捕获链；
  class 体在临时 dict 运行后成为类属性。
- 控制流靠 C++ exception 信号：`sig_raise{exc}`/`sig_return{value}`/
  `sig_break`/`sig_continue`；脚本可见异常是 `PyExcObj` 值。
- `py_error.features` 记录触到的子集外特性名 → 供路由层决定
  fail-closed 还是委托 cpython。

## 插件接入入口 (public header)

`include/sao/plugins/pymini/pymini_host.h`:
- `sao_plugins_pymini_register_loader_adapter(cfg, &owner)` — 注册
  `engine_kind::python` 生产 adapter。launcher 在 `plugins_provider.cpp`
  中按 `python_runtime_status` 设 `cfg.python_home`。
- `sao_plugins_pymini_unregister_loader_adapter(owner)` — 先卸载全部
  lifecycle 插件再调用。
- `sao_plugins_pymini_register_script_engine()` /
  `sao_plugins_pymini_unregister_script_engine()` — `.py` 的 script_ctx
  provider (`runtime_bridge` priority=10)，独立于 adapter。
- `..._adapter_plugin_count` / `..._adapter_get_last_error` /
  `..._free_string` — 内省。

## 卸载顺序

lifecycle 移除全部注册项 → `pymini_drop_callbacks`（先析构 ctx_builder
——其引擎 `SaoSdkContext` 销毁时按 provider cleanup 排空在途 engine
回调，再释放 cb_box 列表）→ interpreter 析构;
helper/`load_local` 解释器归属调用方插件生命周期。`unique_ptr<impl>`
成员 (PyFileObj/PyThreadObj) 的 ctor/dtor 定义在 `pymini_stdlib2.cpp`。

## 构建

`SAO_PLUGINS_ENABLE_PYMINI` (默认 ON, 在 `plugins/CMakeLists.txt`) —
必须在 `python_host` 之后 `add_subdirectory`：可选 `sao_plugins_python_host`
委托链接只在 `SAO_PLUGINS_ENABLE_PYTHON AND TARGET sao_plugins_python_host`
时启用。launcher 侧 `SAO_LAUNCHER_PROVIDER_HAS_PYMINI=1` 切换
`plugins_provider.cpp` 的注册路径。

**语法子集边界**: 仅覆盖真实 legacy 插件用到的最小 Python 子集 —
ctx 对象 + 常用 builtins/`os`/`json`/`time`/`random`/`re` 等;
子集外特性由 `py_error.features` 命名并走委托/UNSUPPORTED 路由。
