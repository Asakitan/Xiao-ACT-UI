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

## vcpkg 依赖

**无**。零依赖是设计目标。

## 历史测试（2026-08-23 已删除）

旧 ``tests/``、Catch2 与 CTest 注册已删除；此前 lex → parse → execute 与
Python selftest 对齐结果只保留为历史证据。
