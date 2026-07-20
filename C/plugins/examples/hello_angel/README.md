# hello_angel — AngelScript 插件宿主 smoke test

这个插件的唯一职责是**在 `angel_host` 里真的跑起来**,
让 `plugins/angel_host/tests/test_hello_angel.cpp` 观察到:

- 引擎初始化后 SDK 3 条 API (`log_info` / `register_ui_panel` / `register_hotkey`) 都被注册进 `asIScriptEngine`
- 加载 `main.as` 编译不报错
- `on_load()` 被调, 里面调 SDK 三条真的被记账
- `on_tick()` 被调 3 次, `tick_count` 从 0 涨到 3
- `on_unload()` 被调, 之后引擎 shutdown 无泄漏

`plugin.json` 用最小字段 (id / name / version / entry / language / description),
故意**不引** `PluginContext@` 或 `dictionary` 这些复杂类型 — 让本插件跑通不依赖
`sdk_binding/binding_angel` 的全量 SDK 注册, 只需要 `angel_host` 自己暴露的 3 条 C 函数即可.

SDK binding 扩展到全类型强绑 (对齐 `plugins/examples/example_angelscript_plugin/`)
后, 这个插件仍然可跑, 作为向后兼容的最小基线.
