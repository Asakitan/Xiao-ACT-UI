# emma_host/ — Emma 自研零依赖解释器

**职责**: 提供 SAO Auto 平台自研的 Emma 脚本语言运行时。零外部依赖, 只用
C++17 stdlib (std::variant / std::unordered_map / std::function)。

## Emma 是什么

见根 ``../README.md`` "Emma 是什么" 一节。简言之: 类 Python 语法, ``end`` 显式
结束块, ``[]`` 数组 + ``{key: value}`` 字典 + ``..`` 字符串拼接, 全套 SDK 都
通过 ``ctx.method(args)`` 调用。

## 对齐的 Python 源

- ``act_platform/scripting/emma_runtime.py``:
  - ``_TOKEN_RE`` + ``_tokenize`` → ``emma_lexer.h``
  - ``_Parser`` + AST 节点类 → ``emma_parser.h``
  - ``_EmmaInterpreter`` + ``_EmmaScope`` + ``_EmmaCallable`` →
    ``emma_interpreter.h``
  - ``_builtins`` + IO builtins → ``emma_stdlib.h``
  - ``EmmaRuntime.load_script`` → ``emma_call.h``
  - SyntaxError / RuntimeError → ``emma_error.h``

## 骨架文件列表

| 文件 | 职责 |
|------|------|
| ``emma_lexer.h`` | tokenizer (手写状态机, 不用 std::regex) |
| ``emma_parser.h`` | 递归下降 parser + AST 池 |
| ``emma_interpreter.h`` | 树遍历执行器 + scope + callable + emma_value variant |
| ``emma_stdlib.h`` | print/str/int/float/len/type + json/time/sleep |
| ``emma_call.h`` | plugin.emma 加载 + hook 分派 |
| ``emma_error.h`` | 词法/语法/运行时错误 → SAO_STATUS |

## 关键设计

**零依赖**: 只用 C++17 stdlib。对齐 Python 版本 emma_runtime.py 的 "零依赖"
承诺 —— 用户不装任何东西就能跑 Emma 插件。

**语义完全对齐**: 一字不改的 ``.emma`` 脚本在 Python 平台和 C++ 平台跑出
一样结果。这一点通过复用 ``examples/example_emma_plugin/plugin.emma``
交叉验证。

**反射引擎面** (``binding_engine.h`` 契约): ``ctx.engine`` 除既有的
``get/require/register/has`` 插件注册表句柄外, 还为 ``sdk_engine_catalog_*``
目录每一条目挂一个命名 callable (``mem.read_u64`` →
``ctx.engine.mem_read_u64(...)``, 位置实参按 ``arg_names`` 映射, 单 dict
且键全落在形参名内时按 kwargs 处理, 尾随多出的一个 string 实参为
envelope 级 ``callback_channel``); ``ctx.engine.list()`` 返回目录数组,
``ctx.engine.on(channel, cb)`` 注册/替换/``nil`` 注销 channel → callable
的通用回调路由; ``ctx.engine_call(name, args)`` 是 raw passthrough 返回
完整 ``{status,result}`` dict。以上全部经宿主自有 ``SaoSdkContext``
(``sao_sdk_context_create`` + ``sao_sdk_context_bind_platform_services``,
``sao::sdk`` 仅 PRIVATE 链接) 走
``sao_plugins_sdk_context_dispatch``; 非零 status 按文件惯例抛
``emma_exception``。emma 的 generic invoke surface 保持 UNSUPPORTED —
这些是新挂的 concrete ctx 成员, 不走 generic invoke。

**性能不追求**: Emma 定位是新手门槛, 不是关键路径。跑 render_panel() 的
速度 emma << lua << angel << C# << python。想要性能选别的引擎。

## 旧插件迁移

Python 平台的 ``.emma`` 脚本, 只要放进 ``plugins/<id>/plugin.emma``,
配好 ``plugin.json`` 里 ``language: emma`` 就跑。没有任何字段需要改。

## 本地 helper 生命周期

`load_local` 导出的 callable 持有 Emma module owner，容器内与返回值内的
callable 递归保留同一 owner；丢弃外层 module 代理不提前卸载仍被 callable
引用的 helper。取得 handle 后的加载失败路径执行卸载，卸载返回非零时仍走
既有 zombie 重试记录；module 成员读取失败直接传播，不伪装成可调用成员。

module 具名调用与导出的独立 callable 均返回原始错误状态，并格式化 helper
文件路径、错误种类、已有行列与调用栈；独立 callable 每次先清空输出，避免
失败时留下前一次结果。成员访问抛出的 Emma/std 异常也保留诊断，而非统一
替换为 `emma module invocation failed`。加载失败沿用终止性状态，附带 helper
路径及词法/语法/执行诊断；入口源码词法/语法错误也沿既有加载失败路径返回，
不表示外部 runtime 缺失。Emma 内置解释器始终可用，不新增外部 runtime
探测或依赖预检图。`missing`/`unsupported` 记录诊断并返回 `nil`。

module 包装仍按字典语义枚举 `get()`，不提前调用取得的函数；不引入惰性代理。
runtime 析构在回调及资源退役后清空解释器全局值，打断全局函数闭包回持作用域
而滞留嵌套 helper owner 的循环，再销毁解释器并释放 context lease；这不是
任意自引用容器或局部闭包的通用循环回收器。

### 原生 SDK fixture

目录：`../../tools/provider_probe/fixture/native_sdk/emma/`，根 `plugin.json`
声明 `id=sdk_emma`、`language=emma`、`enabled=true`。`plugin.emma` 三次加载
`helpers/helper.emma`，helper 再按插件根路径加载 `helpers/leaf.emma`；检查标量
40、嵌套标量及具名 `__call`，丢弃 facade 后重复调用直接成员、容器中的函数及
函数返回的函数，期望分别得到 42/43/44。导出的 `fail_on_call` 与容器内同一函数
只检查 `type(...) == "fn"`，成功加载证明包装过程未提前执行这些失败函数。

| 标记 | 预期触发点 |
| --- | --- |
| `SDK_EMMA_OK` | `on_load` 断言和菜单注册全部成功 |
| `SDK_EMMA_ENABLE_OK` | `on_enable` 再次调用保留的函数成功 |
| `SDK_EMMA_MENU_OK` | 激活 `retained` 菜单项，再次检查保留函数 |
| `SDK_EMMA_UNLOAD_OK` | `on_unload` 到达；本身不证明资源已完全释放 |
| `SDK_EMMA_ASSERT_FAIL <label>` | 任一断言失败，随后调用 `nil` 产生终止性运行错误 |

fixture 使用实际语法 `fn/let/if/end` 和非 callable 调用作为错误断言，不假设
存在 `assert`、`raise` 或异常捕获语法；卸载 hook 刻意保留 `retained`，供父级
探针检查宿主回收、重复加载及最终 shutdown，而非由脚本掩盖所有权问题。

父级失败场景可复用 helper，将独立失败插件入口设为以下源码（成功 fixture
保持不变）：

```emma
fn on_load(ctx)
  let helper = ctx.load_local("helpers/helper.emma")
  let fail = helper.exports.failures[0]
  helper = nil
  fail()
end
```

预期诊断包含 `helpers/helper.emma`（路径分隔符按平台）、`[runtime]`、
`value is not callable at line 11` 和 `fail_on_call` 调用栈；错误状态为
`SAO_ERR_OS_CALL_FAILED`，不是 missing-runtime。改用 `helper.fail_on_call()`
可覆盖具名调用路径。另将 helper 或入口源码设为 `fn broken(`，应得到
`SAO_ERR_INVALID_ARGUMENT` 的语法加载失败，不进入备用 runtime 路线。

session-81 审阅补修后定向 Debug 与 provider888/0 已通过；三种 runtime 配置下此 fixture
原生 load/enable/unload 成功。入口语法错误也经 failed-load 路径附加文件路径，
helper 保留函数的运行错误含原状态、路径和栈。真实菜单回调输出
`SDK_EMMA_MENU_OK`，invoke=0、handled=1、refresh=0、shutdown=0；不声明通用循环
回收或所有并发/失败清理矩阵已验证。

## vcpkg 依赖

**无**。零依赖是设计目标。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除；此前 lex → parse → execute 与
Python selftest 对齐结果只保留为历史证据。
