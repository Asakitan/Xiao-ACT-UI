# ACT Platform Architecture

SAO Auto's ACT platform is a shared runtime for live combat telemetry, replay validation, plugins, reports, triggers, and the two first-party UI surfaces: WebView and Entity/Tk.

No ACT feature is complete until WebView and Entity can reach the same shared backend capability.

## Runtime Goals

- Keep TCP packet parsing as the live combat ground truth.
- Publish stable ACT event envelopes for plugins, replay, history, triggers, and UI tools.
- Keep WebView and Entity 1:1 through a capability parity contract.
- Let plugins declare extensions without mutating engine internals directly.
- Keep memory probing conservative, opt-in, read-only, and observable.
- Validate every ACT change with targeted selftests instead of broad repo-wide compile runs.

## Core Modules

| Area | Module | Responsibility |
| --- | --- | --- |
| Event envelopes | `act_platform/events.py` | Builds canonical ACT event dictionaries with `topic`, `source`, `game_id`, `parser_id`, and copied payloads. |
| Event bus | `act_platform/event_bus.py` | Synchronous publish/subscribe with bounded recent-event history and callback isolation. |
| Runtime helpers | `act_platform/runtime.py` | Shared WebView/Entity command helpers for plugins, reports, history, timelines, graphs, drilldowns, data-source health, and triggers. |
| Plugin SDK | `act_platform/plugins.py` | Discovers plugin folders, loads `plugin.json`, owns `PluginContext`, and exposes plugin status. |
| Parser adapters | `act_platform/adapters.py` | First-party parser adapter contract and built-in `star_resonance_tcp` wrapper. |
| Replay | `act_replay/` | Offline event fixtures, replay harness, and platform regression selftest. |
| Combat analytics | `engines/combat_analytics.py` | Builds ACT snapshots and render specs from DPS/game state. |
| Trigger engine | `engines/act_trigger_engine.py` | Shared trigger/timer rule evaluator. |
| History/export | `engines/dps_history.py` | Rolling encounter persistence plus JSON/CSV export. |
| TCP bridge | `net/packet_bridge.py` | Owns capture, parser adapter creation, packet callbacks, and data-source health. |
| Hybrid memory source | `mem_probe/unified_source.py` | Read-only memory self-state bridge used by memory/hybrid/auto modes. |

## Data Flow

Live TCP path:

```text
PacketCapture
  -> PacketBridge
  -> StarResonanceParserAdapter
  -> packet_parser.PacketParser
  -> DpsTracker / EncounterManager / GameStateManager
  -> combat_analytics.build_act_snapshot(...)
  -> act_platform.runtime shared helpers
  -> WebView and Entity surfaces
```

ACT event path:

```text
parser/replay/runtime callback
  -> act_platform.events.make_event(...)
  -> EventBus.publish(...)
  -> plugins, action log, graph/timeseries, timeline/VCR, trigger consumers
```

Replay path:

```text
act_replay fixtures
  -> ActReplayHarness
  -> same tracker/encounter/event-bus contracts
  -> act_replay.selftest and targeted tool selftests
```

## Event Envelopes

ACT event envelopes are plain dictionaries so UI code, replay, and plugins can consume them without a UI-specific dependency.

Important fields:

- `schema_version`: canonical event schema version.
- `topic`: event topic such as `damage`, `skill`, `boss`, `act_snapshot`, or `trigger_fired`.
- `observed_at`: event timestamp.
- `source.name`: publisher name.
- `source.kind`: source kind such as `packet`, `replay`, `runtime`, or `plugin`.
- `source.game_id`: game id, currently `star_resonance`.
- `source.parser_id`: parser adapter id, currently `star_resonance_tcp` for live TCP parsing.
- `payload`: copied event payload.

Use `make_event(...)` from `act_platform.events` when adding new producers.

## Parser Adapters

The parser adapter contract lives in `act_platform.adapters`.

The built-in adapter is:

```text
adapter_id: star_resonance_tcp
game_id: star_resonance
source_kinds: packet
supported_locales: zh-CN
```

`PacketBridge` now creates the live `packet_parser.PacketParser` through `StarResonanceParserAdapter`, while preserving `self._parser` for existing compatibility methods such as player cache, monster cache, scene reset, and profession skill cache restore.

Plugin parser adapter declarations are currently metadata-only through `ctx.register_parser_adapter(...)`. Runtime execution of plugin parser handlers is reserved for a later slice and must include time-budget and fault-isolation rules.

## Plugin Extensions

Plugins can declare these extension kinds during `on_load(ctx)`:

- `parser_adapters`
- `exporters`
- `trigger_types`
- `report_views`
- `timers`

Plugin status exposes registered extension metadata and per-plugin extension counts. Extension handlers stay in-process and are intentionally omitted from JSON status.

Load/unload behavior:

- Duplicate extension ids across plugins are rejected.
- Plugin unload removes extension metadata.
- Failed plugin load removes partial extension metadata and event subscriptions.

## UI Parity

The parity contract lives in `tools.act_ui_parity` and is documented in `docs/ACT_UI_PARITY_SDK.md`.

Every ACT capability has:

- a stable capability id;
- shared backend provider;
- required actions;
- required payload fields;
- WebView binding;
- Entity binding;
- parity validation.

Current capability ids include live overview, history, export, triggers/timers, plugin manager, data-source health, action log, graph/timeseries, timeline/VCR, combatant drilldown, skill drilldown, and offline import.

## Trigger And Timer Rules

`ActTriggerEngine` evaluates shared rules against ACT snapshots/render specs.

Supported rule types:

- `damage_total`
- `heal_total`
- `elapsed_s`
- `boss_hp_pct_below`
- `skill_kind`
- `boss_event_type`
- `encounter_start`
- `field_match`
- `timer_preset`

`field_match` supports dot-path lookups and operators `exists`, `eq`, `ne`, `contains`, `regex`, `gt`, `gte`, `lt`, and `lte`.

Preset helpers:

- `act_trigger_export_presets(owner, rule_ids=None)`
- `act_trigger_import_presets(owner, payload, replace=False)`

## Reports, History, And Replay

The current history store is a rolling encounter store exposed through shared runtime helpers. JSON and CSV export remain backed by `DpsHistoryStore`.

Replay is the contract gate for ACT behavior. When adding a feature that can be tested without the live game, prefer adding a fixture/selftest path first.

## Validation

Recommended targeted validation from `sao_auto`:

```powershell
e:\Py\python.exe -m py_compile act_platform\events.py act_platform\event_bus.py act_platform\runtime.py act_platform\plugins.py act_platform\adapters.py engines\act_trigger_engine.py net\packet_bridge.py
e:\Py\python.exe -m act_platform.selftest
e:\Py\python.exe -m act_replay.selftest
e:\Py\python.exe -m unittest tools.act_ui_parity_selftest tools.act_trigger_runtime_selftest tools.act_parser_adapter_selftest tools.act_plugin_extensions_selftest
git diff --check
```

Do not run release packaging unless explicitly requested. For packaging changes, keep `XiaoACTUI.spec` hidden imports aligned with new ACT packages and Cython scanner modules.

## Open Edges

- Plugin parser and trigger handlers are declared but not invoked by runtime services yet.
- Offline import is still replay-fixture focused; broader pcap/log/XML import remains future work.
- History is not yet a searchable SQLite-style database.
- Manual WebView/Entity smoke still depends on a UI runtime session.
- Hybrid memory policy is documented separately in `docs/HYBRID_MEMORY_TCP.md`.
