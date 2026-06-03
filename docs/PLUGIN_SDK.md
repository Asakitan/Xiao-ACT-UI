# ACT Python Plugin SDK

SAO Auto exposes a small in-process Python plugin layer for ACT-style extensions. Plugins are loaded from `plugins/<plugin_id>/` and `user_plugins/<plugin_id>/`.

## Directory layout

```text
plugins/
  hello_act_plugin/
    plugin.json
    plugin.py
  star_basic_report_plugin/
    plugin.json
    plugin.py
```

`user_plugins/` has the same layout and is intended for user-installed plugins.

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

Fields:

- `id`: stable plugin id. It must match the folder name for predictable UI display.
- `name`: display name.
- `version`: plugin version.
- `entry`: Python file inside the plugin folder. Defaults to `plugin.py`.
- `enabled`: whether the plugin should load automatically.
- `subscriptions`: optional documentation for topics the plugin listens to.
- `capabilities`: optional ACT capability IDs or metadata objects exposed by this plugin.
- `permissions`: reserved for future permission gates.

The loader rejects entry paths that escape the plugin directory.

## Lifecycle hooks

All hooks are optional:

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

`on_load(ctx)` receives a `PluginContext`. Register event subscriptions from this hook.

## `PluginContext`

`ctx` provides:

- `ctx.log(message)`: write to the plugin status log.
- `ctx.subscribe(topic, callback)`: subscribe to ACT events and return an unsubscribe token.
- `ctx.on(topic, callback=None)`: subscribe directly or use decorator form: `@ctx.on("damage")`.
- `ctx.subscribe_once(topic, callback)`: handle the next event for a topic, then auto-unsubscribe.
- `ctx.unsubscribe(token)`: remove a previous subscription token.
- `ctx.on_damage(callback)`, `ctx.on_heal(callback)`, `ctx.on_skill(callback)`, `ctx.on_boss(callback)`, `ctx.on_snapshot(callback)`, `ctx.on_encounter_finalized(callback)`: common topic shortcuts.
- `ctx.emit(topic, payload=None)`: publish a canonical ACT event from the plugin.
- `ctx.get_snapshot()`: read a shallow owner snapshot when available.
- `ctx.snapshot_value("a.b.c", default=None)`: read a dotted path from the snapshot.
- `ctx.recent_events(limit=20, topic="")`: inspect recent ACT events, optionally filtered by topic.
- `ctx.get_setting(key, default=None)`: read an owner setting.
- `ctx.setting(key, default=None)`: shorthand for `get_setting`.
- `ctx.set_setting(key, value)`: write an owner setting.
- `ctx.set_defaults(mapping)`: initialize missing plugin settings without overwriting user values.
- `ctx.register_parser_adapter(id, metadata=None, handler=None)`: declare a parser/game adapter extension.
- `ctx.register_exporter(id, metadata=None, handler=None)`: declare an exporter extension.
- `ctx.register_trigger_type(id, metadata=None, handler=None)`: declare a plugin trigger type.
- `ctx.register_report_view(id, metadata=None, handler=None)`: declare a plugin report/detail view.
- `ctx.register_timer(id, metadata=None, handler=None)`: declare a timer preset/provider extension.

Callbacks receive one argument: a canonical ACT event envelope.

Decorator style keeps external plugins compact:

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

## Event envelope

Every event is a dict with this shape:

```json
{
  "schema_version": 1,
  "topic": "act_snapshot",
  "source": "replay",
  "ts": 1710000000.0,
  "payload": {}
}
```

Known topics include:

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

Unknown topics are allowed, but public plugins should prefer the known list for UI parity and future compatibility.

## Capability metadata

Plugins may declare ACT capabilities in `plugin.json` so the manager UI and future parity routers can show what the plugin contributes:

```json
{
  "capabilities": [
    "plugin_manager",
    {"id": "triggers_timers", "title": "Trigger authoring"},
    {"capability_id": "export", "actions": ["save"]}
  ]
}
```

Supported object keys are `id`/`capability_id`, `title`, `description`, `route`, `render_hint`, `actions`, and `payload_fields`. Unknown or invalid capability entries are ignored.

## Extension registry

Plugins can register extension metadata during `on_load(ctx)`. These declarations are exposed through plugin status so WebView and Entity managers can show which plugin contributes parser adapters, exporters, trigger types, report views, or timers. Handlers are kept in-process for future runtime wiring and are intentionally omitted from JSON status.

```python
def on_load(ctx):
  ctx.register_parser_adapter("star_fixture", {
    "title": "Star fixture parser",
    "game_ids": ["star_resonance"],
    "source_kinds": ["fixture", "packet"],
    "priority": 5,
  })

  ctx.register_exporter("summary_json", {
    "title": "Summary JSON",
    "formats": ["json"],
    "payload_fields": ["total_damage"],
  })

  ctx.register_trigger_type("field_match", {
    "label": "Field match",
    "schema": {"field": "string", "match": "string"},
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

Extension ids must already be safe ids: lowercase letters/numbers plus `_`, `-`, or `.`. Registered metadata is removed automatically when the plugin unloads.

## Example plugin

See `plugins/hello_act_plugin/` for a tiny lifecycle example. It demonstrates `ctx.set_defaults()`, decorator-style `ctx.on("act_snapshot")`, and `ctx.on_encounter_finalized()`, then logs compact status lines in both WebView and Entity plugin manager panels.

See `plugins/star_basic_report_plugin/` for a report-style example. It demonstrates `ctx.subscribe_once()`, explicit `ctx.unsubscribe(token)`, threshold settings, `ctx.snapshot_value()`, extension registration, and plugin-originated `plugin_report_summary` events.

## UI management

Both UIs expose the same plugin actions:

- list plugins
- show status
- enable plugin
- disable plugin
- reload one plugin or all plugins

WebView: `SAO Menu > ACT 插件管理 Plugin Manager`.

Entity: `SAO 菜单 > 面板 > ACT插件管理面板`.

## Safety notes

Plugins run in-process, so keep callbacks fast and defensive. Callback exceptions are isolated by the event bus and recorded as plugin/event failures, but a plugin can still consume CPU if it performs long blocking work. For heavy work, start a background thread and keep event callbacks minimal.
