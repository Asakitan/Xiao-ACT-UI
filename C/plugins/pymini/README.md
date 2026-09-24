# pymini/ — 纯 C++ Python 子集解释器 (legacy `plugin.py` 免 CPython)

**职责**: 让 legacy Python 插件 (如 `plugin.py` + `ctx.*`) 在**不依赖
CPython/Python SDK/python3.dll/pip** 的前提下运行在 C++ 插件系统上。
我们自己解析其插件结构与支持的 Python 子集语法, 映射到当前 C++ SDK /
Plugin API, 由 C++ 原生执行。

```
legacy plugin.py → pymini lexer/parser → AST (tree-walk interpreter)
                 → ctx 对象绑 loader::plugin_context_t (C ABI)
```

## 基础语言兼容面

原生路径现覆盖常用插件语法中的 `NAME := value`、`async def` / `await`、
`async for` / `async with` / async comprehension，以及 `yield` / `yield from`。
异步语法按立即值与同步协议执行；generator 在函数调用时有界收集（最多 65,536 项）
并返回单次迭代器，不实现挂起恢复、`send`、`throw` 或 `close`。连续省略号形式的
相对导入按三个层级解析。

stdlib-lite 新增 `uuid`、`secrets`、`statistics` 与 `importlib.import_module`；
UUID v4、token 和随机选择使用系统 CSPRNG。它们与既有本地 package/import 图、
`ctx`、常用容器、异常、装饰器、comprehension 和文件沙箱组合运行，不加载
CPython。动态代码生成、任意二进制扩展、真正的事件循环与平台专用第三方包仍由
显式 `cpython` 路径承担；原生代码开始执行后仍不切换解释器重跑。

## 与 cpython 委托的复合路由

`pymini` 是 `engine_kind::python` 的默认生产适配器 (composite):

| manifest `py_runtime` | 路由 |
|---|---|
| `"pymini"` | 本解释器原生执行 |
| `"cpython"` | 委托 `sao_plugins_pyhost_*` (仅当 cpython runtime 可用) |
| 缺失 / `auto` | 源码预检: 子集内 → pymini; 子集外 → pyhost 委托; 无 pyhost → `SAO_PLUGINS_ERR_UNSUPPORTED` + 特性名 |

注册只保存 `pymini_adapter_config::python_home` 和模块目录，不初始化 CPython。
只有实际加载已选定的 CPython 路由时才初始化可选宿主；为空时该插件返回缺少
CPython 的诊断。原生插件不依赖客户机 Python，也不因配置了 Python 而加载其 DLL。
执行开始后的异常不会切换解释器重跑，避免重复插件副作用。

反射 `ctx.engine_call`/`ctx.engine.list` 使用共享 8 MiB 结果预算一次性派发，严格
校验非空、NUL 终止及 `status`/`result` 包壳；非零或越界状态抛出错误，不会因
结果扩容重试可能带副作用的 invoker。高容量缓冲为单次调用局部所有，回调重入不会
覆盖外层调用结果。

launcher 在缺少可用 Python 时，通过 `sao_plugins_pymini_adapter_requires_python`
逐插件预检并延期完整 Python 插件及其依赖者；独立原生插件继续启动。
语法错误、缺失本地文件、强制 `pymini` 的子集外代码仍是插件错误，不变成安装提示。
CPython 首次初始化线程继续拥有冷启动宿主；跨线程生命周期调用明确返回 BUSY。
显式 CPython 路由同样检查入口路径与可读性；加载失败但清理尚未完成时保留宿主
所有权，交由生命周期回滚/卸载重试，不丢弃仍待释放的实例。

### 导入预检契约

`pymini_preflight_plugin(plugin_root, entry_rel, extra_dirs, out_reason)`
在执行入口、包初始化或插件回调之前调用；只注册原生工厂指针，读取和解析
入口与可达本地模块，不调用工厂、不执行源码、不启动 Python。
`true` 表示静态检查通过；`false` 仅表示解析器明确报告的 `UnsupportedSyntax`
或搜索路径中不存在的已知 CPython 依赖根，原因包含文件与行号。
语法、读取、路径、未知模块缺失和预算错误抛出 `py_error`，不变成安装 Python 的理由。

`pymini_preflight_subset(source, reason)` 保留为无目录上下文的词法/语法树检查：
注释和字符串里的 `await`、`yield`、`import ctypes` 不触发误报；
分号语句、多项 import、裸 `yield` 和解析器子集外诊断均可识别。
旧入口没有目录上下文，绝对导入的已知依赖根按外部依赖保守判断；
插件分流使用新入口，不能以旧入口代替模块图检查。

- 本地 `ctypes` 等同名包/模块先解析，再判断是否需要 CPython；原生工厂、
  `builtins` 和 `__future__.annotations` 不属于外部依赖。
- 缺失导入直接受 `except ImportError`、`except ModuleNotFoundError`
  （含异常元组）或裸 except 保护时，不要求 CPython；已经找到的本地模块仍完整扫描，
  其内部依赖、语法和预算错误不由外层 try 豁免。函数体不继承定义位置的异常保护，
  except/else/finally 也不继承同一 try 的保护。
- 仅跳过模块顶层先执行的 `from typing import TYPE_CHECKING`（可带别名）
  对应的直接 `if NAME:` 主体，前提是整棵语法树中该名字只有这一次绑定、
  没有星号导入；else 仍扫描。嵌套/条件导入、`typing.TYPE_CHECKING`、重绑定、
  复合条件和函数体内的类型检查分支保守扫描，仍可能要求 CPython。
  此优化不模拟反射、动态命名空间修改或猴子补丁，所有源码仍先经过解析器。
- 每个源码最多 1 MiB，总源码最多 8 MiB，最多 256 个源码身份、1024 个模块名；
  导入/语法树深度上限 64，额外目录最多 128 个；单源码最多 131072 个 token，
  单逻辑语句最多 512 个 token，括号与缩进组合深度最多 64。超限抛错，不归类为 CPython 需求。
- 静态图扫描函数与一般条件分支，不执行条件或推导动态 import、`__all__`、
  修改后的 `sys.path`/`__path__`；`from pkg import child` 对存在的同名子模块保守扫描，
  即使包运行时可能提供同名普通属性。预检不是完整 CPython 语法验证器，首个
  `UnsupportedSyntax` 之后的同文件语法尚未由解析器验证；磁盘变化也不由此 API 锁定。

### 原生模块与包

图预检和运行时共用逐级解析器，运行时先复用缓存，再查原生模块/宿主桥；
文件搜索依次使用插件根、`vendor`、`vendors`、`libs`、`lib`、`site-packages`、
`python` 和额外目录。同目录先选择 `pkg/__init__.py`，再选择 `pkg.py`；
只有没有具体包/模块时才合并命名空间包目录，后续具体包可以覆盖前面收集的命名空间部分。
普通 `.py` 模块没有文件系统子模块。

包在执行前进入缓存并建立 `__path__`，父包、循环导入与原生 stdlib 模块复用同一对象。
`from . import helper` / `from pkg import child` 先查属性再查包子模块；
子模块执行中的异常原样传播，不改写成“找不到导入名”。入口去掉 `.py` 后作为逻辑模块名，
嵌套路径转换为点号，`__init__.py` 使用包名。普通顶层模块 `__package__` 为空，
子模块指向父包，包指向自身；越过顶层或没有父包的相对导入产生 `ImportError`。
相对导入同时接受分开的点 token 与连续省略号 token；每个 `...` 计为三级，仍对越过顶层的路径返回 `ImportError`。

入口在执行前注入 `ctx`/`act_ctx` 并注册，顶层日志、菜单注册和循环回读可使用它。
加载失败先排空 loader 注册，再释放 pymini 回调；排空失败保留宿主状态并在卸载时重试。
`tools/pymini_probe/fixture/native_packages/` 覆盖嵌套相对导入、循环身份、包优先、命名空间包、
本地 `ctypes`、字符串误报、可选导入、`TYPE_CHECKING`、原生模块身份和顶层 ctx 日志/菜单；
Debug 定向构建与该 fixture 的加载/启用/卸载已通过；Python 未配置和有效 home
两种情况下，注册/预检/执行/卸载的进程模块 census 均无 Python DLL。
session-80 最终 provider 探针为 `334 checks, 0 failures`，覆盖逐插件及传递依赖延期、
实际加载时才初始化 CPython、释放以及跨线程 BUSY/owner 线程清理；独立审阅
`passed-after-fix`。该 session-80 基线仅构建定向 Debug 目标；session-82 已在扩展
fixture 后完成 pymini `native_literals` 与 provider888/0；最终 Review 只改 AI Editor
execution runtime，故这两项 mini 证据保持。session-82 的全量 Debug 与 Hardened
22PE/77文件发布验收已按最终 C++ 补修重跑通过。分别见
[session-80](../../docs/session-log/session-80-pymini-native-runtime.md) 和
[session-82](../../docs/session-log/session-82-mini-runtimes-vscode-execution.md)。

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
  中传入可选 `cfg.python_home`；`python_runtime_status=READY` 仅表示布局可用，
  不表示已加载 Python DLL。
- `sao_plugins_pymini_adapter_requires_python(owner, manifest, &required, &reason)` —
  无脚本执行、无 CPython 初始化的路由查询；`reason` 由 `..._free_string` 释放。
- `sao_plugins_pymini_unregister_loader_adapter(owner)` — 先卸载全部
  lifecycle 插件再调用。
- `sao_plugins_pymini_register_script_engine()` /
  `sao_plugins_pymini_unregister_script_engine()` — 注册 `.py` 的 composite
  script_ctx providers：pymini `priority=10`，可选 CPython helper facade
  `priority=100`；只有 pymini 无执行预检明确不支持时才进入后者，独立于 plugin adapter。
- `..._adapter_plugin_count` / `..._adapter_get_last_error` /
  `..._free_string` — 内省。

## 卸载顺序

lifecycle 移除全部注册项 → `pymini_drop_callbacks`（先析构 ctx_builder
——其引擎 `SaoSdkContext` 销毁时按 provider cleanup 排空在途 engine
回调，再释放 cb_box 列表）→ interpreter 析构;
helper/`load_local` 解释器由 loader 的 per-context attachment 强持有，与调用方语言
无关，不再使用常驻 helper map。module/function proxy 仅保留弱身份与值键；
callback 实参、容器内和返回的 callable 使用同一调用 lease，退休后返回
`HANDLE_INVALID`，不访问旧解释器或 ctx，成员枚举返回空列表。
在途调用或重入卸载返回 `BUSY`，保留资源供外层排空后重试；注册清理完成后才调用
`pymini_drop_callbacks`，最终析构不持 loader/helper/ctx mutex 或解释器 guard。
entry 与 helper 的 SDK engine callback 均通过 ctx 创建时记录的弱世代身份取得
lease，过期身份不重新按 ctx 地址绑定，也不形成 attachment→interpreter→owner
强引用环；`interpreter::call` 固定 callable 强引用，嵌套回调替换原绑定时正在执行的函数仍存活。
CPython helper 同样由 context-generation attachment 拥有，按 canonical path 缓存，
以 generation/path hash 组成唯一物理 module ID 防 `sys.modules` 冲突；facade 只传递
有界 JSON-compatible 值与 callable member。只要 helper handle 或 zombie state 仍在，
最终 CPython shutdown 就返回 busy 并保留资源供后续重试。
本 helper 生命周期修改经静态调用链、磁盘读回与独立审阅；同路径同线程递归加载返回
`BUSY`，其他线程等待首个加载结果，context lease 阻止 active call 期间 retirement。
session-83 post-Review 的 helper fixture、三真实 action、正常 unload/finalization、定向/完整
Debug 与 Hardened acceptance 均通过；未执行对抗式调度、finalization 故障注入或长期并发压力。
`unique_ptr<impl>` 成员 (PyFileObj/PyThreadObj) 的 ctor/dtor 定义在
`pymini_stdlib2.cpp`。

## 构建

`SAO_PLUGINS_ENABLE_PYMINI` (默认 ON, 在 `plugins/CMakeLists.txt`) —
必须在 `python_host` 之后 `add_subdirectory`：可选 `sao_plugins_python_host`
委托链接只在 `SAO_PLUGINS_ENABLE_PYTHON AND TARGET sao_plugins_python_host`
时启用。launcher 侧 `SAO_LAUNCHER_PROVIDER_HAS_PYMINI=1` 切换
`plugins_provider.cpp` 的注册路径。

**语法子集边界**: 覆盖基础插件常用的 ctx、builtins、容器、导入、同步
async-lite、eager generator 与 stdlib-lite；不等同完整 Python。子集外特性由
`py_error.features` 命名并走委托/UNSUPPORTED 路由。包导入、递归预检与惰性
CPython 的基线证据记录于 [`session-80`](../../docs/session-log/session-80-pymini-native-runtime.md)，
本轮扩展语法/stdlib 与全量构建、发布验收记录于
[`session-82`](../../docs/session-log/session-82-mini-runtimes-vscode-execution.md)；跨语言
CPython helper 与真实 AS/Emma/Lua 插件证据见
[`session-83`](../../docs/session-log/session-83-cross-language-support-runtime.md)。
