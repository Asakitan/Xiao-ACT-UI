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
- `permissions`: optional declaration for audit/UI review. Use `"engine_access"` when a trusted in-process plugin intentionally touches `ctx.engine`/`ctx.owner`; this is not a sandbox boundary.

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
- `ctx.register_formatter(id, metadata=None, handler=None)`: declare a Mini-Parse/clipboard formatter extension.
- `ctx.register_trigger_type(id, metadata=None, handler=None)`: declare a plugin trigger type.
- `ctx.register_report_view(id, metadata=None, handler=None)`: declare a plugin report/detail view.
- `ctx.register_timer(id, metadata=None, handler=None)`: declare a timer preset/provider extension.
- `ctx.owner`: return the live WebView/Entity owner object when this in-process plugin is attached to a UI runtime.
- `ctx.engine`: high-freedom trusted bridge for direct project engine access.
- `ctx.get_engine(name, default=None)`: read a named engine handle if available.
- `ctx.require_engine(name)`: read a named engine handle or raise a clear error.
- `ctx.call_engine(name, method, *args, **kwargs)`: call a method on a named engine handle.
- `ctx.call_runtime(action, *args, **kwargs)`: call a shared `act_platform.runtime` action against `ctx.owner`.

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

## Trusted engine access

In-process plugins are trusted Python code. They run in the same interpreter as SAO Auto and can import project modules directly, so the SDK exposes an explicit high-freedom bridge instead of forcing plugins to guess private owner attributes.

Use this API for user-installed power plugins, diagnostics, custom report panels, automated testing helpers, or advanced integrations that need real project internals. Keep event callbacks fast: long-running work should move to a worker thread/process or a process-isolated parser adapter.

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

Named handles currently include:

| Handle | Typical owner attribute(s) | Purpose |
| --- | --- | --- |
| `owner` | current owner object | Full UI/runtime owner object. |
| `event_bus` | plugin manager event bus | Publish/subscribe canonical ACT events. |
| `plugin_manager` | current plugin manager | Discovery, status, extension invocation. |
| `settings` | owner settings object | Read/write shared settings. |
| `game_state` | `_game_state`, `game_state` | Live game/session state. |
| `state_manager` | `_state_mgr`, `state_mgr` | GUI/runtime state manager. |
| `dps_tracker` | `_dps_tracker`, `dps_tracker` | Live DPS/encounter tracker. |
| `history_store` | `_dps_history_store`, `dps_history_store` | ACT report history/import/export store. |
| `encounter_manager` | `_encounter_mgr`, `encounter_mgr` | Encounter lifecycle manager. |
| `trigger_engine` | `_act_trigger_engine`, `act_trigger_engine` | ACT trigger/timer engine. |
| `packet_bridge` | `_packet_engine`, `_packet_bridge` | Live packet capture/parser bridge. |
| `memory_bridge` | `_mem_bridge`, `mem_bridge` | Memory/TCP bridge integration. |
| `auto_key_engine` | `_auto_key_engine`, `auto_key_engine` | Auto-key runtime integration. |
| `boss_raid_engine` | `_boss_raid_engine`, `boss_raid_engine` | Boss/raid helper integration. |

`ctx.engine` helpers:

- `ctx.engine.handles()`: return availability/type metadata for all documented handles.
- `ctx.engine.available()`: list currently available handle names.
- `ctx.engine.get(name, default=None)` / `ctx.engine.require(name)`: read a handle.
- `ctx.engine.owner_attr(name, default=None)` / `ctx.engine.set_owner_attr(name, value)`: inspect or edit owner attributes.
- `ctx.engine.call_owner(method, *args, **kwargs)`: call a method on the owner.
- `ctx.engine.call(engine_name, method, *args, **kwargs)`: call a method on a named engine.
- `ctx.engine.runtime(action, *args, **kwargs)`: call shared ACT runtime helpers such as `plugin_status`, `report_status`, `report_export`, `history_status`, `offline_import_file`, `trigger_status`, `timeline_status`, `action_log_status`, `death_recap_status`, `graph_timeseries_status`, `combatant_drilldown_status`, `skill_drilldown_status`, and `data_source_health`.
- `ctx.engine.import_module("engines.combat_analytics")`: import project modules from a plugin.
- `ctx.engine.snapshot()`: read the same owner snapshot as `ctx.get_snapshot()`.

Process-isolated parser adapters do not receive direct engine handles. They communicate by JSON payload only, which keeps untrusted or heavy parsing code away from UI/runtime objects.

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

Plugins can register extension metadata during `on_load(ctx)`. These declarations are exposed through plugin status so WebView and Entity managers can show which plugin contributes parser adapters, exporters, Mini-Parse formatters, trigger types, report views, or timers. Handlers are kept in-process and are intentionally omitted from JSON status.

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

Extension ids must already be safe ids: lowercase letters/numbers plus `_`, `-`, or `.`. Registered metadata is removed automatically when the plugin unloads.

## Mini-Parse formatter handlers

Formatter handlers registered with `ctx.register_formatter(id, metadata, handler=...)` can be invoked by `act_mini_parse_preview(owner, formatter_id="<id>")` and `act_mini_parse_copy(owner, formatter_id="<id>")`.

The handler receives a copied payload:

```python
{
  "encounter_id": "...",
  "preview": {...},  # same top_rows/totals preview used by report export
  "history": [...],
  "report": {...},
  "status": {...},
}
```

Return a string or a dictionary with `text`/`value`. Keep formatter handlers deterministic and fast; built-in formatter ids `summary_table`, `chat_ranking`, and `json` remain available even when no plugin manager is loaded.

## Trigger handlers

Trigger handlers registered with `ctx.register_trigger_type(id, metadata, handler=...)` can be used by ACT trigger rules with `type: "plugin_trigger"` and `plugin_trigger_type: "<id>"`.

The handler receives one copied payload:

```python
{
  "rule": {...},          # normalized trigger rule
  "render_spec": {...},   # current ACT render payload
  "snapshot": {...},      # full ACT snapshot root
}
```

Return `True` to match, `False` to ignore, or a dictionary such as:

```python
{"matched": True, "message": "Burst window", "severity": "warn"}
```

`PluginManager.invoke_extension(...)` records exceptions and elapsed time. If a handler exceeds `time_budget_ms` or `max_runtime_ms`, the call is treated as failed and contributes to the plugin failure counter. In-process Python cannot forcibly stop already-running code, so handlers must stay small and deterministic.

## Parser adapters

The first-party parser adapter contract lives in `act_platform.adapters`. The built-in Star Resonance TCP parser is exposed as `star_resonance_tcp` with:

- `game_id`: `star_resonance`
- `source_kinds`: `["packet"]`
- `supported_locales`: `["zh-CN"]`
- runtime methods: `start()`, `stop()`, `parse_packet(frame)`, `normalize_event(topic, payload)`, and `health()`

Plugin parser adapters declare equivalent metadata through `ctx.register_parser_adapter(...)`. Passing a handler makes the adapter invokable through `act_platform.adapters.PluginParserAdapter`, which routes calls through `PluginManager.invoke_extension("parser_adapters", ...)` with deep-copied payloads, elapsed-time measurement, exception isolation, and the extension time budget by default.

For multi-game parser authoring, start with a synthetic adapter template rather than a guessed real protocol:

```python
def parser_handler(payload):
  op = payload.get("operation")
  if op in {"start", "stop"}:
    return {"ok": True}
  if op == "parse_log_line":
    line = payload.get("line") or ""
    # Parse only documented fixture text here. Do not infer production fields
    # without real samples/schemas from the target game.
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

Real third-party adapters need representative packet/log/report samples or authoritative schemas before they can be marked production-ready.

Parser adapters can opt into subprocess isolation by adding `"isolation": "process"` to their metadata. In that mode `PluginParserAdapter` invokes `python -m act_platform.parser_worker`, reloads the plugin entry in the worker process, calls the registered parser handler through JSON stdin/stdout, and kills the worker if it exceeds the adapter time budget. Process-isolated adapters default to a 1000 ms budget when metadata does not provide `time_budget_ms` or `max_runtime_ms`. `parse_packet` frames cross the process boundary as base64-encoded bytes and are decoded before the handler receives them. This gives stronger containment for untrusted or heavy parser code, with higher per-call startup overhead than the default in-process path.

The handler receives an `operation` field plus operation-specific data:

- `start` / `stop`: lifecycle calls.
- `parse_packet`: `frame` bytes and `frame_len`.
- `parse_log_line`: `line` text; return an event dict, a list of event dicts, or `{ "events": [...] }`.
- `import_file`: `path`; return import metadata/events.
- `normalize_event`: `topic`, `payload`, `source_kind`, `confidence`, and `observed_at`; return a canonical event dict.

Live `PacketBridge` uses the built-in Star Resonance parser by default. To opt into a live plugin packet parser, set `act_live_parser_adapter_id` to an active adapter id whose metadata includes `"packet"` in `source_kinds`. WebView and Entity pass the shared plugin manager/event bus into `PacketBridge`, so selected plugin `parse_packet` results can publish canonical ACT events. Unknown, unloaded, or non-packet adapters automatically fall back to `star_resonance_tcp`; inspect `PacketBridge.health()["parser_adapter_selection"]` or the data-source health panel for the selected id and fallback reason.

Live `parse_packet` handlers should return one of these shapes:

```python
{"events": [{"topic": "damage", "payload": {"damage": 123}}]}
{"event": {"topic": "skill", "payload": {"skill_name": "Demo"}}}
{"topic": "boss", "payload": {"message": "phase"}}
[{"topic": "damage", "payload": {"damage": 456}}]
```

The default plugin runtime is still in-process: calls are deep-copied, timed, and failure-accounted by `PluginManager.invoke_extension(...)`, but Python code cannot be force-killed mid-call. Keep live parser handlers deterministic and short unless they opt into `"isolation": "process"` and accept the extra worker startup cost.

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
