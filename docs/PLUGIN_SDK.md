# ACT Python Plugin SDK

SAO Auto exposes a small in-process Python plugin layer for ACT-style extensions. Plugins are loaded from `plugins/<plugin_id>/` and `user_plugins/<plugin_id>/`.

## Directory layout

```text
plugins/
  hello_act_plugin/
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

- `ctx.log(message, level="info")`: write to the plugin status log.
- `ctx.subscribe(topic, callback)`: subscribe to ACT events.
- `ctx.emit(topic, payload=None, source=None)`: publish a canonical ACT event.
- `ctx.get_snapshot()`: read a shallow owner snapshot when available.
- `ctx.get_setting(key, default=None)`: read an owner setting.
- `ctx.set_setting(key, value)`: write an owner setting.

Callbacks receive one argument: a canonical ACT event envelope.

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

## Example plugin

See `plugins/hello_act_plugin/` for a runnable example. It subscribes to `act_snapshot` and `encounter_finalized`, then logs compact status lines in both WebView and Entity plugin manager panels.

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
