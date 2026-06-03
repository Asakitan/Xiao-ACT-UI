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
| Runtime helpers | `act_platform/runtime.py` | Shared WebView/Entity command helpers for plugins, reports, history, offline imports, timelines, action logs, death recaps, graphs, drilldowns, data-source health, and triggers. |
| Plugin SDK | `act_platform/plugins.py` | Discovers plugin folders, loads `plugin.json`, owns `PluginContext`, and exposes plugin status. |
| Parser adapters | `act_platform/adapters.py` | First-party parser adapter contract and built-in `star_resonance_tcp` wrapper. |
| Replay | `act_replay/` | Offline event fixtures, replay harness, and platform regression selftest. |
| Offline import | `act_replay/importer.py` | Loads normalized ACT JSON/JSONL files into replay-ready event lists. |
| Combat analytics | `engines/combat_analytics.py` | Builds ACT snapshots and render specs from DPS/game state. |
| Trigger engine | `engines/act_trigger_engine.py` | Shared trigger/timer rule evaluator. |
| History/export | `engines/dps_history.py` | Rolling encounter persistence, append-only JSONL archive, lightweight search, and JSON/CSV/HTML/XML plus compressed XML export/import. |
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
  -> plugins, action log, death recap, graph/timeseries, timeline/VCR, trigger consumers
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

`PacketBridge` now creates the live `packet_parser.PacketParser` through `StarResonanceParserAdapter`, while preserving `self._parser` for existing compatibility methods such as player cache, monster cache, scene reset, and profession skill cache restore. The live bridge defaults to `star_resonance_tcp`; advanced users can opt into a loaded plugin adapter with the `act_live_parser_adapter_id` setting. Live selection accepts only adapters whose metadata includes `packet` in `source_kinds`; missing adapters, unavailable plugin managers, or non-packet adapters fall back to `star_resonance_tcp` and expose the reason through `PacketBridge.health().parser_adapter_selection`.

Plugin parser adapter declarations registered through `ctx.register_parser_adapter(...)` can now be wrapped by `PluginParserAdapter` and invoked through `PluginManager.invoke_extension("parser_adapters", ...)` for controlled `start`, `stop`, `parse_packet`, `parse_log_line`, `import_file`, and `normalize_event` operations. Parser adapter metadata can opt into `"isolation": "process"`, which routes calls through the one-shot `act_platform.parser_worker` subprocess bridge with JSON stdin/stdout, base64 frame transfer, and timeout kill behavior. When selected for live TCP, plugin `parse_packet` results are treated as canonical ACT event rows (`event`, `events`, a single event dict, or a list) and published to the shared EventBus. Plugin trigger handlers registered through `ctx.register_trigger_type(...)` can be invoked by `plugin_trigger` rules through the same manager isolation path, with exception isolation, elapsed-time measurement, and time-budget failure accounting.

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

Current capability ids include live overview, history, export, triggers/timers, plugin manager, data-source health, action log, graph/timeseries, timeline/VCR, combatant drilldown, skill drilldown, death recap, and offline import.

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

The current history store is a rolling encounter store exposed through shared runtime helpers. `DpsHistoryStore` also appends compact finalized reports to a JSONL archive and an additive SQLite mirror for longer-lived retention. SQLite schema version 2 mirrors encounter/combatant summaries plus optional action rows, timeline events, trigger events, and source metadata when finalized reports include those lists. Shared action-log history queries now expose paged rows plus lightweight analytics groups by topic, actor, target, and action. JSON, CSV, static HTML, structured XML, and compressed SAO XML (`xml.gz` / `xml.zip`) export remain backed by the compact report shape.

Normalized offline imports are loaded by `act_replay.importer`, replayed through `ActReplayHarness`, finalized into the same DPS report shape as live encounters, and can be persisted with `act_platform.runtime.act_offline_import_file(owner, path, persist=True)`. If the built-in normalized importer rejects a file, the runtime can import plain or compressed SAO structured XML reports as finalized history reports, then fall back to active plugin parser adapters and call their controlled `import_file` operation. The helper returns import metadata, importer/parser ids, replay preview or report preview, the generated/imported report, the persisted history item, and refreshed history status. WebView and Entity expose this through both standalone offline import wizards and their shared report/export panels.

The bundled FastAPI repository server also exposes a local read-only ACT API for dynamic report consumers:

- `GET /api/act/health`
- `GET /api/act/reports?limit=20&q=`
- `GET /api/act/reports/latest`
- `GET /api/act/reports/{index}`
- `GET /api/act/actions?limit=100&encounter_id=`
- `WS /ws/act/reports`

These endpoints read the same `DpsHistoryStore` files and SQLite action history. They are localhost-only by default even when the server process binds to `0.0.0.0`; set `SAO_ACT_API_ALLOW_REMOTE=1` only for an explicitly trusted network/debug session.

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

## Plan Completion Matrix

This matrix defines the current completion line for the automatically implementable ACT platform plan. Items marked complete have shared backend APIs, WebView/Entity parity where user-facing, and offline regression coverage. Items marked blocked need concrete external input before implementation can be reliable.

| Area | Status | Implemented boundary | Regression gate |
| --- | --- | --- | --- |
| Shared ACT event runtime | Complete | Canonical event envelopes, bounded EventBus history, replay publication, and shared runtime helpers. | `python -m act_platform.selftest`, `python -m act_replay.selftest` |
| Plugin SDK/platform | Complete | Plugin discovery, SDK ergonomics, extension registry, plugin status, trigger handlers, parser adapters, and plugin fallback import. | `python -m unittest tools.act_plugin_ergonomics_selftest tools.act_plugin_extensions_selftest tools.act_plugin_examples_selftest tools.act_plugin_capability_selftest` |
| Parser adapters | Complete for built-in/plugin contracts | Built-in TCP adapter, plugin adapter metadata, live packet adapter selection/fallback, and opt-in process isolation. | `python -m unittest tools.act_parser_adapter_selftest` |
| Dual UI parity | Complete by contract | WebView and Entity/Tk expose the same ACT capability ids, shared actions, and payload fields. | `python -m tools.act_ui_parity`, `python -m unittest tools.act_ui_parity_selftest` |
| Reports/history/archive | Complete | Rolling JSON history, JSONL archive, SQLite encounter/combatant/action/timeline/trigger/source mirror, search/load/delete, action-history paging/analytics, and JSON/CSV/HTML/XML/compressed XML export/import. | `python -m unittest tools.act_history_selftest tools.act_action_log_selftest tools.act_report_export_selftest tools.act_offline_import_selftest` |
| Offline import | Complete for known schemas | Normalized JSON/JSONL/NDJSON replay import, SAO structured XML and compressed XML report round-trip import, and plugin parser-adapter fallback. | `python -m unittest tools.act_offline_import_selftest` |
| Local dynamic report access | Complete, local-only by default | Read-only HTTP endpoints and WebSocket snapshot/history/action access over the same `DpsHistoryStore`. | `python -m unittest tools.act_server_api_selftest` |
| Runtime dashboards | Complete by shared helper | Timeline/VCR, graph/timeseries, action log, combatant drilldown, skill drilldown, data-source health, trigger/timer, plugin manager, report/history, offline import, and death recap surfaces. | `python -m unittest tools.act_timeline_selftest tools.act_graph_timeseries_selftest tools.act_death_recap_selftest tools.act_combatant_drilldown_selftest tools.act_skill_drilldown_selftest tools.act_data_source_health_selftest tools.act_trigger_runtime_selftest` |
| Hybrid TCP+memory mode | Complete for policy/scaffold | Conservative scan gates, source health reporting, TCP fallback, and memory/hybrid policy exposure. | `python -m act_replay.selftest`; live validation remains manual |
| Entity/Tk performance guardrails | Complete for current panels | ACT Entity panels use throttled refreshes, bounded row counts, and dirty signatures; WebView/GPU remains preferred for high-frequency or animated views. | Targeted UI module `py_compile` plus manual runtime smoke when available |

Hard-blocked follow-ups:

- Third-party pcap/log import and non-SAO ACT compressed XML variants require real samples or authoritative schemas.
- ODBC/FTP export requires destination schema, credentials, and security policy decisions.
- Native C# ACT providers require concrete provider contracts and runtime integration targets.
- Manual WebView/Entity smoke requires a live UI/game runtime session.

## Open Edges

- Plugin parser handlers can now be selected for live packet EventBus publishing, offline import fallback, metadata discovery, and opt-in process isolation through `isolation: "process"`. In-process remains the default for low overhead.
- Offline import supports normalized JSON/JSONL replay files, plain/compressed SAO structured XML report round-trips, rolling-history persistence, and local dynamic report reads over HTTP/WebSocket; broader pcap/log import and third-party ACT-compatible compressed XML variants remain future work until concrete samples are available.
- History has lightweight rolling-store search, an append-only JSONL encounter archive, and an additive SQLite mirror for encounter/combatant/action/timeline/trigger/source metadata rows. The shared action-log surface can now switch between live EventBus rows and SQLite action-history rows by source/encounter filter, page through the result set, and return lightweight topic/actor/target/action analytics. Dedicated timeline/trigger/source-specific history tabs remain future UX work.
- Manual WebView/Entity smoke still depends on a UI runtime session.
- Hybrid memory source has conservative policy gates for scan enablement, admin requirement, poll interval, region scan cap, and static fallback control; live game/UI validation remains manual. See `docs/HYBRID_MEMORY_TCP.md`.
