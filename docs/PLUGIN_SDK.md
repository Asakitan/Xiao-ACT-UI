# ACT Python 插件 SDK

> 当前版本：`4.6.74`（review-chain Batch 202）。本文与 `docs/ACT_PLATFORM.md`、`docs/ACT_UI_PARITY_SDK.md` 配套阅读。

SAO Auto 提供一个进程内的 Python 插件层，用于 ACT 风格扩展。插件从 `plugins/<plugin_id>/` 与 `user_plugins/<plugin_id>/` 加载。

## 目录结构

```text
plugins/
  hello_act_plugin/
    plugin.json
    plugin.py
  star_basic_report_plugin/
    plugin.json
    plugin.py
```

`user_plugins/` 与 `plugins/` 结构相同，用于用户安装的插件。

## `plugin.json`

```json
{
  "id": "hello_act_plugin",
  "name": "Hello ACT Plugin",
  "version": "0.1.0",
  "entry": "plugin.py",
  "description": "Logs ACT snapshots and encounter summaries.",
  "enabled": true,
  "subscriptions": ["act_snapshot", "encounter_finalized"],
  "capabilities": ["plugin_manager"],
  "permissions": []
}
```

字段说明：

- `id`：稳定的插件 id。建议与文件夹同名，方便 UI 显示。
- `name`：显示名。
- `version`：插件版本。
- `entry`：插件文件夹内的 Python 入口文件，默认 `plugin.py`。
- `enabled`：是否在启动时自动加载。
- `subscriptions`：可选，文档化插件订阅的主题。
- `capabilities`：可选，插件暴露的 ACT capability id 或元数据对象。
- `permissions`：可选，用于审计/UI 评审的声明。受信任的进程内插件如果有意访问 `ctx.engine`/`ctx.owner`，建议声明 `"engine_access"`；这不是沙箱边界。

加载器会拒绝逃逸出插件目录的入口路径。

## 生命周期钩子

所有钩子均为可选：

```python
_ctx = None


def on_load(ctx):
    global _ctx
    _ctx = ctx
    ctx.log("loaded")


def on_enable():
    if _ctx:
        _ctx.log("enabled")


def on_disable():
    if _ctx:
        _ctx.log("disabled")


def on_unload():
    if _ctx:
        _ctx.log("unloaded")
```

`on_load(ctx)` 收到一个 `PluginContext`。请在该钩子中注册事件订阅。

## `PluginContext`

`ctx` 提供：

- `ctx.log(message)`：写入插件状态日志。
- `ctx.subscribe(topic, callback)`：订阅 ACT 事件，返回取消订阅的 token。
- `ctx.on(topic, callback=None)`：直接订阅或装饰器形式：`@ctx.on("damage")`。
- `ctx.subscribe_once(topic, callback)`：处理某主题的下一次事件后自动取消订阅。
- `ctx.unsubscribe(token)`：移除先前的订阅 token。
- `ctx.on_damage(callback)`、`ctx.on_heal(callback)`、`ctx.on_skill(callback)`、`ctx.on_boss(callback)`、`ctx.on_snapshot(callback)`、`ctx.on_encounter_finalized(callback)`：常用主题的 shortcut。
- `ctx.emit(topic, payload=None)`：从插件发布一条规范化 ACT 事件。
- `ctx.get_snapshot()`：在可用时读取浅层 owner snapshot。
- `ctx.snapshot_value("a.b.c", default=None)`：从 snapshot 读取点路径。
- `ctx.recent_events(limit=20, topic="")`：检视最近 ACT 事件，可选按 topic 过滤。
- `ctx.get_setting(key, default=None)`：读取 owner 设置。
- `ctx.setting(key, default=None)`：`get_setting` 的简写。
- `ctx.set_setting(key, value)`：写入 owner 设置。
- `ctx.set_defaults(mapping)`：在不覆盖用户值的前提下初始化缺失的插件设置。
- `ctx.register_parser_adapter(id, metadata=None, handler=None)`：声明解析器/游戏适配器扩展。
- `ctx.register_exporter(id, metadata=None, handler=None)`：声明导出器扩展。
- `ctx.register_formatter(id, metadata=None, handler=None)`：声明 Mini-Parse/剪贴板 formatter 扩展。
- `ctx.register_trigger_type(id, metadata=None, handler=None)`：声明插件触发器类型。
- `ctx.register_report_view(id, metadata=None, handler=None)`：声明插件报告/明细视图。
- `ctx.register_timer(id, metadata=None, handler=None)`：声明计时器预设/provider 扩展。
- `ctx.owner`：当此进程内插件挂在 UI runtime 上时，返回活跃的 WebView/Entity owner。
- `ctx.engine`：受信任的高自由度桥，用于直接访问项目引擎。
- `ctx.get_engine(name, default=None)`：按名读取引擎句柄。
- `ctx.require_engine(name)`：按名读取引擎句柄，若不可用则抛出明确错误。
- `ctx.call_engine(name, method, *args, **kwargs)`：调用具名引擎句柄上的方法。
- `ctx.call_runtime(action, *args, **kwargs)`：针对 `ctx.owner` 调用共享 `act_platform.runtime` 动作。

回调接收一个参数：规范化的 ACT 事件信封。

实时 Star Resonance 包事件 `skill`、`dungeon`、`monster`、`boss` 在内置解析器能够派生时，会带有稳定的战斗语义 payload，挂在 `event["payload"]["combat_fact"]`。常用键包括 `skill_id`、`skill_name`、`skill_role`、`dungeon_id`、`dungeon_name`、`monster_id`、`mechanics`、`boss_mechanic_key`、`boss_mechanic_label`、`trigger_family`。ACT snapshot 还会在 `render_spec.context` 暴露 `last_skill_id`、`last_skill_name`、`last_skill_role`、`last_boss_mechanic_key`、`last_boss_mechanic_label`、`last_boss_trigger_family` 等 shortcut。

AutoKey profile 条件可以使用相同的语义事实，条件类型包括 `dungeon_is`、`last_skill_is`、`boss_mechanic_is`、`boss_mechanic_family_is`。值可能是 id、名字或键，取决于具体条件，例如 `boss_mechanic_is=shield_broken` 或 `boss_mechanic_family_is=shield`。

装饰器风格让外部插件保持紧凑：

```python
def on_load(ctx):
    ctx.set_defaults({"damage_threshold": 100000})

    @ctx.on("damage")
    def log_big_hit(event):
        damage = int((event.get("payload") or {}).get("damage") or 0)
        if damage >= ctx.setting("damage_threshold", 100000):
            ctx.log(f"big hit: {damage}")

    ctx.on_encounter_finalized(lambda event: ctx.log("encounter finalized"))
```

## 受信任的引擎访问

进程内插件是受信任的 Python 代码，与 SAO Auto 跑在同一个解释器中，可以直接 import 项目模块。SDK 因此显式提供高自由度桥，避免插件去猜测私有的 owner 属性。

请将这个 API 用于用户安装的高权限插件、诊断、自定义报告面板、自动化测试辅助或需要真实项目内部状态的高级集成。事件回调要保持轻量；耗时工作应迁移到 worker 线程/进程或进程隔离的解析器适配器。

```python
def on_load(ctx):
    ctx.log("available engines: " + ",".join(ctx.engine.available()))

    tracker = ctx.require_engine("dps_tracker")
    history = ctx.get_engine("history_store")
    report_status = ctx.call_runtime("report_status")

    ctx.register_report_view("engine_diagnostics", {
        "title": "Engine diagnostics",
        "route": "plugin://my_plugin/engine_diagnostics",
        "payload_fields": ["handles", "report_status"],
    }, handler=lambda payload: {
        "ok": True,
        "handles": ctx.engine.handles(),
        "report_status": report_status,
        "tracker_type": type(tracker).__name__,
        "history_type": type(history).__name__ if history else "missing",
    })
```

当前提供的具名句柄：

| 句柄 | 典型 owner 属性 | 用途 |
| --- | --- | --- |
| `owner` | 当前 owner 对象 | 完整 UI/runtime owner。 |
| `event_bus` | 插件管理器事件总线 | 发布/订阅规范化 ACT 事件。 |
| `plugin_manager` | 当前插件管理器 | 发现、状态、扩展调用。 |
| `settings` | owner 设置对象 | 读写共享设置。 |
| `game_state` | `_game_state`、`game_state` | 实时游戏/会话状态。 |
| `state_manager` | `_state_mgr`、`state_mgr` | GUI/runtime 状态管理器。 |
| `dps_tracker` | `_dps_tracker`、`dps_tracker` | 实时 DPS/encounter tracker。 |
| `history_store` | `_dps_history_store`、`dps_history_store` | ACT 报告历史/导入/导出存储。 |
| `encounter_manager` | `_encounter_mgr`、`encounter_mgr` | 战斗生命周期管理。 |
| `trigger_engine` | `_act_trigger_engine`、`act_trigger_engine` | ACT 触发器/计时器引擎。 |
| `packet_bridge` | `_packet_engine`、`_packet_bridge` | 实时抓包/解析器桥。 |
| `memory_bridge` | `_mem_bridge`、`mem_bridge` | 内存/TCP 桥集成。 |
| `auto_key_engine` | `_auto_key_engine`、`auto_key_engine` | Auto-key runtime 集成。 |
| `boss_raid_engine` | `_boss_raid_engine`、`boss_raid_engine` | Boss/raid helper 集成。 |

`ctx.engine` 帮助函数：

- `ctx.engine.handles()`：返回所有文档化句柄的可用性/类型元数据。
- `ctx.engine.available()`：列出当前可用的句柄名。
- `ctx.engine.get(name, default=None)` / `ctx.engine.require(name)`：读取句柄。
- `ctx.engine.owner_attr(name, default=None)` / `ctx.engine.set_owner_attr(name, value)`：检视或编辑 owner 属性。
- `ctx.engine.call_owner(method, *args, **kwargs)`：在 owner 上调用方法。
- `ctx.engine.call(engine_name, method, *args, **kwargs)`：在具名引擎上调用方法。
- `ctx.engine.runtime(action, *args, **kwargs)`：调用共享 ACT runtime helper，例如 `plugin_status`、`report_status`、`report_export`、`history_status`、`offline_import_file`、`trigger_status`、`timeline_status`、`action_log_status`、`death_recap_status`、`graph_timeseries_status`、`combatant_drilldown_status`、`skill_drilldown_status`、`data_source_health`。
- `ctx.engine.import_module("engines.combat_analytics")`：从插件导入项目模块。
- `ctx.engine.snapshot()`：读取与 `ctx.get_snapshot()` 相同的 owner snapshot。

进程隔离的解析器适配器不会获得直接的引擎句柄。它们只通过 JSON payload 通信，把不可信或重型解析代码与 UI/runtime 对象隔离。

## 事件信封

每个事件都是这种结构的 dict：

```json
{
  "schema_version": 1,
  "topic": "act_snapshot",
  "source": "replay",
  "ts": 1710000000.0,
  "payload": {}
}
```

已知主题包括：

- `act_snapshot`
- `combat_event`
- `skill_event`
- `buff_event`
- `boss_event`
- `party_event`
- `encounter_started`
- `encounter_finalized`
- `replay_started`
- `replay_finished`
- `ui_action`

允许未知主题，但公共插件应优先使用已知列表，以便保证 UI 对等与未来兼容性。

## Capability 元数据

插件可以在 `plugin.json` 中声明 ACT 能力，让管理器 UI 与未来的对等路由能展示插件贡献了什么：

```json
{
  "capabilities": [
    "plugin_manager",
    {"id": "triggers_timers", "title": "Trigger authoring"},
    {"capability_id": "export", "actions": ["save"]}
  ]
}
```

支持的对象键：`id`/`capability_id`、`title`、`description`、`route`、`render_hint`、`actions`、`payload_fields`。未知或无效 capability 条目将被忽略。

## 扩展注册表

插件可以在 `on_load(ctx)` 中注册扩展元数据。这些声明会通过插件状态暴露给 WebView 与 Entity 管理器，展示哪个插件贡献了 parser adapter、exporter、Mini-Parse formatter、trigger type、report view 或 timer。处理函数保持进程内运行，按设计不出现在 JSON 状态中。

```python
def on_load(ctx):
    ctx.register_parser_adapter("star_fixture", {
        "title": "Star fixture parser",
        "display_name": "Star fixture parser",
        "game_id": "star_resonance",
        "game_ids": ["star_resonance"],
        "supported_locales": ["zh-CN"],
        "source_kinds": ["fixture", "packet"],
        "priority": 5,
    })

    ctx.register_exporter("summary_json", {
        "title": "Summary JSON",
        "formats": ["json"],
        "payload_fields": ["total_damage"],
    })

    ctx.register_formatter("chat_summary", {
        "title": "Chat summary",
        "formatter": "text",
        "payload_fields": ["preview", "history"],
    }, handler=lambda payload: {
        "text": " | ".join(
            f"#{idx + 1} {row.get('name')} {row.get('damage', 0)}"
            for idx, row in enumerate((payload.get("preview") or {}).get("top_rows") or [])
        ) or "No combatants"
    })

    ctx.register_trigger_type("burst_gate", {
        "label": "Burst gate",
        "schema": {"field": "string", "match": "string"},
        "time_budget_ms": 25,
    }, handler=lambda payload: {
        "matched": (payload.get("render_spec", {}).get("totals", {}).get("damage", 0) >= payload.get("rule", {}).get("threshold", 0)),
        "message": "Burst gate matched",
        "severity": "warn",
    })

    ctx.register_report_view("compact_report", {
        "route": "plugin://my_plugin/compact",
        "actions": ["open", "copy"],
    })

    ctx.register_timer("burst_window", {
        "duration_s": 12,
        "scope": "encounter",
    })
```

扩展 id 必须是安全 id：小写字母/数字加 `_`、`-` 或 `.`。插件卸载时已注册的元数据会自动移除。

## Mini-Parse formatter handler

通过 `ctx.register_formatter(id, metadata, handler=...)` 注册的 formatter handler 可被 `act_mini_parse_preview(owner, formatter_id="<id>")` 与 `act_mini_parse_copy(owner, formatter_id="<id>")` 调用。

handler 接收复制后的 payload：

```python
{
    "encounter_id": "...",
    "preview": {...},  # 与报告导出相同的 top_rows/totals 预览
    "history": [...],
    "report": {...},
    "status": {...},
}
```

返回字符串或带 `text`/`value` 的 dict。formatter handler 应保持确定性与轻量；即使没有插件管理器加载，内置 formatter id `summary_table`、`chat_ranking`、`json` 仍然可用。

## Trigger handler

通过 `ctx.register_trigger_type(id, metadata, handler=...)` 注册的触发器 handler 可被 ACT 触发器规则使用，规则类型为 `type: "plugin_trigger"`，并指定 `plugin_trigger_type: "<id>"`。

handler 接收一份复制后的 payload：

```python
{
    "rule": {...},          # 归一化触发器规则
    "render_spec": {...},   # 当前 ACT 渲染 payload
    "snapshot": {...},      # 完整 ACT snapshot 根
}
```

返回 `True` 表示匹配，`False` 表示忽略，或返回 dict，例如：

```python
{"matched": True, "message": "Burst window", "severity": "warn"}
```

`PluginManager.invoke_extension(...)` 会记录异常与耗时。如果 handler 超过 `time_budget_ms` 或 `max_runtime_ms`，调用会被视作失败并计入插件失败计数。进程内 Python 不能强制终止已运行的代码，所以 handler 必须小且确定。

## 解析器适配器

一方解析器适配器契约位于 `act_platform.adapters`。内置 Star Resonance TCP 解析器以 `star_resonance_tcp` 暴露：

- `game_id`：`star_resonance`
- `source_kinds`：`["packet"]`
- `supported_locales`：`["zh-CN"]`
- 运行时方法：`start()`、`stop()`、`parse_packet(frame)`、`normalize_event(topic, payload)`、`health()`

插件解析器适配器通过 `ctx.register_parser_adapter(...)` 声明等价的元数据。传入 handler 后该适配器可由 `act_platform.adapters.PluginParserAdapter` 调用，由 `PluginManager.invoke_extension("parser_adapters", ...)` 路由，附带深拷贝 payload、耗时测量、异常隔离，默认使用扩展时间预算。

多游戏解析器作者建议先用 fixture 模板，而不是猜测真实协议：

```python
def parser_handler(payload):
    op = payload.get("operation")
    if op in {"start", "stop"}:
        return {"ok": True}
    if op == "parse_log_line":
        line = payload.get("line") or ""
        # 这里只解析文档化的 fixture 文本。
        # 在没有目标游戏的真实样本/schema 之前，不要推断生产字段。
        if "DAMAGE" in line:
            return {"events": [{"topic": "damage", "payload": {"damage": 1, "actor": "fixture"}}]}
    return {"events": []}


def on_load(ctx):
    ctx.register_parser_adapter("fixture_game_log", {
        "title": "Fixture game log parser",
        "game_id": "fixture_game",
        "game_ids": ["fixture_game"],
        "supported_locales": ["en-US"],
        "source_kinds": ["log", "fixture"],
        "isolation": "process",
        "time_budget_ms": 1000,
    }, handler=parser_handler)
```

真实的第三方适配器需要代表性的包/日志/报告样本或权威 schema 才能视为生产可用。

解析器适配器可在元数据中加上 `"isolation": "process"` 启用子进程隔离。该模式下 `PluginParserAdapter` 调用 `python -m act_platform.parser_worker`，在 worker 进程中重新加载插件入口，通过 JSON stdin/stdout 调用注册的 parser handler；超出适配器时间预算时 worker 会被强杀。process-isolated 适配器在元数据未提供 `time_budget_ms` 或 `max_runtime_ms` 时默认使用 1000 ms 预算。`parse_packet` 帧以 base64 字节形式跨越进程边界，并在 handler 收到前解码。该模式对不可信或重型解析代码提供更强的隔离，代价是更高的单次启动开销。

handler 收到一个 `operation` 字段加上对应数据：

- `start` / `stop`：生命周期调用。
- `parse_packet`：`frame` 字节与 `frame_len`。
- `parse_log_line`：`line` 文本；返回事件 dict、事件 dict 列表或 `{ "events": [...] }`。
- `import_file`：`path`；返回导入元数据/事件。
- `normalize_event`：`topic`、`payload`、`source_kind`、`confidence`、`observed_at`；返回规范化事件 dict。

实时 `PacketBridge` 默认使用内置 Star Resonance 解析器。要切换到实时插件包解析器，请把 `act_live_parser_adapter_id` 设为已激活、元数据 `source_kinds` 包含 `"packet"` 的适配器 id。WebView 与 Entity 把共享插件管理器/事件总线传入 `PacketBridge`，因此被选中的插件 `parse_packet` 结果可以发布规范化 ACT 事件。未知、未加载或非 packet 适配器会自动回退到 `star_resonance_tcp`；可在 `PacketBridge.health()["parser_adapter_selection"]` 或数据源健康面板查看选中的 id 与回退原因。

实时 `parse_packet` handler 应返回以下结构之一：

```python
{"events": [{"topic": "damage", "payload": {"damage": 123}}]}
{"event": {"topic": "skill", "payload": {"skill_name": "Demo"}}}
{"topic": "boss", "payload": {"message": "phase"}}
[{"topic": "damage", "payload": {"damage": 456}}]
```

默认插件 runtime 仍是进程内：调用经过深拷贝、耗时测量和失败计数（由 `PluginManager.invoke_extension(...)` 完成），但 Python 代码无法强制中止。除非显式选择 `"isolation": "process"` 并接受额外的 worker 启动成本，否则实时 parser handler 必须保持确定性与短小。

## 示例插件

参见 `plugins/hello_act_plugin/`，一个最小的生命周期示例，演示 `ctx.set_defaults()`、装饰器风格 `ctx.on("act_snapshot")`、`ctx.on_encounter_finalized()`，并在 WebView 与 Entity 的插件管理器面板中输出紧凑状态行。

参见 `plugins/star_basic_report_plugin/`，一个报告型示例，演示 `ctx.subscribe_once()`、显式 `ctx.unsubscribe(token)`、阈值设置、`ctx.snapshot_value()`、扩展注册以及插件发起的 `plugin_report_summary` 事件。

## UI 管理

两套 UI 暴露相同的插件操作：

- 列出插件
- 查看状态
- 启用插件
- 禁用插件
- 重新加载某个插件或所有插件

WebView：`SAO Menu > ACT 插件管理 Plugin Manager`。

Entity：`SAO 菜单 > 面板 > ACT插件管理面板`。

## 打包提示

插件零件（如独立 MIDI 插件、独立 Hide & Seek 插件等）已并入正式打包流程。如果插件需要进入 onedir 发行，请同步 `XiaoACTUI.spec` 的 hidden imports 与资源声明，避免运行时找不到入口或资源。

## 安全注意

插件运行在主进程内，回调要快、要防御。事件总线会把回调异常隔离并记入插件/事件失败统计，但插件仍可能在长阻塞工作中占用 CPU。重型工作请开后台线程，让事件回调保持精简。
