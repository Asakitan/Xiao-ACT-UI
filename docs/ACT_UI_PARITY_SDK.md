# ACT UI Parity SDK

SAO Auto exposes a small Python SDK for keeping ACT WebView and Entity UI surfaces 1:1. Use this contract whenever an ACT feature adds or changes routes, actions, payload fields, or render behavior.

No ACT capability is considered complete if it only ships on one UI surface.

## Module

The SDK lives in `tools.act_ui_parity`.

```python
from tools.act_ui_parity import (
    ENTITY_UI,
    WEBVIEW_UI,
    CapabilityBinding,
    UiAdapterContract,
    assert_ui_parity,
    build_capability_registry,
    validate_ui_parity,
)
```

## Capability registry

`build_capability_registry()` returns the canonical ACT capability matrix. The registry is the source of truth for both UIs.

Each `CapabilitySpec` defines:

- `capability_id`: stable capability id.
- `title`: display name for diagnostics.
- `required_actions`: actions every UI must expose.
- `required_payload_fields`: shared payload fields every UI must provide.
- `webview_render_hint`: intended WebView presentation.
- `entity_render_hint`: intended Entity presentation.

Known capability IDs:

- `live_overview`
- `combatant_drilldown`
- `skill_drilldown`
- `action_log`
- `graph_timeseries`
- `encounter_timeline_vcr`
- `history_browser`
- `export`
- `triggers_timers`
- `plugin_manager`
- `data_source_health`
- `offline_import`

## Adapter contract

Each UI declares its ACT surface with `UiAdapterContract` and `CapabilityBinding`.

```python
webview = UiAdapterContract(
    ui_kind=WEBVIEW_UI,
    bindings={
        "plugin_manager": CapabilityBinding(
            capability_id="plugin_manager",
            actions=("list_plugins", "enable", "disable", "reload", "status"),
            payload_fields=("plugins", "enabled", "status", "version", "errors"),
            render_hint="plugin manager page",
            route="/act/plugin_manager",
        ),
    },
)

entity = UiAdapterContract(
    ui_kind=ENTITY_UI,
    bindings={
        "plugin_manager": CapabilityBinding(
            capability_id="plugin_manager",
            actions=("list_plugins", "enable", "disable", "reload", "status"),
            payload_fields=("plugins", "enabled", "status", "version", "errors"),
            render_hint="plugin menu/detail panel with identical operations",
            route="entity://act/plugin_manager",
        ),
    },
)
```

`route` and `render_hint` may differ by UI. `actions` and shared `payload_fields` must stay equivalent.

## Default contracts

Use `build_default_contracts()` for the current scaffold WebView and Entity declarations.

```python
from tools.act_ui_parity import build_default_contracts

webview, entity = build_default_contracts()
assert_ui_parity(webview, entity)
```

The default contracts are useful for tests and smoke checks. Real UI adapters should eventually declare bindings from their actual WebView and Entity route/action registrations.

## Validation

Use `validate_ui_parity(webview, entity)` when you need structured issues.

```python
issues = validate_ui_parity(webview, entity)
for issue in issues:
    print(issue.code, issue.ui_kind, issue.capability_id, issue.message)
```

Use `assert_ui_parity(webview, entity)` in tests or feature gates. It raises `AssertionError` when either surface drifts.

Validation catches:

- missing or unknown capability IDs;
- missing required actions;
- missing required payload fields;
- missing route or render hints;
- WebView/Entity action drift;
- WebView/Entity payload drift;
- wrong UI kind declarations.

## Selftest report

Run the module directly to emit a machine-readable parity report.

```powershell
python -m tools.act_ui_parity
```

The report includes:

- `ok`: whether the parity gate passed.
- `summary`: capability and issue counts.
- `issues`: serialized parity failures.
- `registry`: canonical capability specs.
- `adapters`: WebView and Entity bindings.

## Regression tests

Recommended validation from the `sao_auto` directory:

```powershell
python -m py_compile tools\act_ui_parity.py tools\act_ui_parity_selftest.py act_replay\selftest.py
python -m tools.act_ui_parity
python -m unittest tools.act_ui_parity_selftest
python -m act_replay.selftest
```

`act_replay.selftest` includes the ACT UI parity contract and the ACT plugin platform contract.

## Adding or changing an ACT capability

1. Add or update the `CapabilitySpec` in `build_capability_registry()`.
2. Add equivalent WebView and Entity bindings.
3. Keep action names and shared payload fields identical across both surfaces.
4. Let route and render hints describe each UI's own presentation.
5. Add or update regression coverage in `tools.act_ui_parity_selftest`.
6. Run the validation commands above.

## UI rules

- Entity is not a lightweight subset of WebView.
- WebView may use rich charts, pages, or dialogs.
- Entity may use compact panels, tables, or menus.
- Both UIs must preserve the same capability IDs, operations, shared payload fields, and acceptance gates.
- Plugin manager, history, export, trigger/timer, data-source health, and offline import capabilities are all part of the parity gate.

## Current trigger/timer management surface

`triggers_timers` is wired through shared `act_platform.runtime` helpers so WebView and Entity call the same rule backend:

- status: `act_trigger_status(owner)`
- enable: `act_trigger_enable(owner, rule_id)`
- disable: `act_trigger_disable(owner, rule_id)`
- reload: `act_trigger_reload(owner)`
- test: `act_trigger_test(owner, rule_id)`
- export presets: `act_trigger_export_presets(owner, rule_ids=None)`
- import presets: `act_trigger_import_presets(owner, payload, replace=False)`

WebView route: `web/trigger_timer_manager.html`, opened from `SAO Menu > ACT 触发/计时 Trigger Timer`.

Entity route: `entity://act/triggers_timers`, opened from `SAO 菜单 > 面板 > ACT触发/计时`.

The current panel reads rules from `settings.json` key `act_trigger_rules`. It supports enable/disable/reload/test and shared preset import/export helpers. Rule evaluation supports the original summary rules plus `field_match` over dot-path ACT snapshot/render fields and `timer_preset` countdown-style rules.

## Current data-source health surface

`data_source_health` is wired through shared `act_platform.runtime` helpers so WebView and Entity/Tk inspect the same PacketBridge + memory fallback snapshot:

- status: `act_data_source_health(owner)`
- refresh: `act_data_source_health(owner)`
- diagnose: `act_data_source_diagnose(owner)`
- copy: copies the JSON health/diagnostic payload

WebView route: `web/data_source_health.html`, opened from `SAO Menu > ACT 数据源健康 Data Source Health`.

Entity route: `entity://act/data_source_health`, opened from `SAO 菜单 > 面板 > ACT数据源健康`.

The shared payload exposes `sources`, `status`, `latency_ms`, `last_event_ms`, and `errors`. For Entity/Tk, the panel is intentionally low-frequency: refreshes are throttled and source/diagnostic cards use dirty signatures before rebuilding widgets. High-frequency or animated ACT panels should prefer WebView/GPU overlay rendering, with Cython reserved for measured hot arithmetic/signature paths.

## Current report/export surface

`export` is wired through shared `act_platform.runtime` helpers and the existing `DpsHistoryStore` exporter:

- status: `act_report_status(owner, limit=20, fmt="json")`
- save: `act_report_export(owner, fmt="json" | "csv")`
- copy: `act_report_copy(owner, fmt="json")`

WebView route: `web/act_report_export.html`, opened from `SAO Menu > ACT 报告/导出 Report Export`.

Entity route: `entity://act/export`, opened from `SAO 菜单 > 面板 > ACT报告/导出`.

The shared payload preserves the parity fields `encounter_id`, `formats`, `selected_format`, and `preview`, with additional `history` and `storage_status` fields for user feedback. Entity/Tk uses a low-frequency diagnostic panel with refresh throttling and dirty render signatures; file writing remains in `DpsHistoryStore.export_report()` so UI code does not duplicate export format logic.

## Current history browser surface

`history_browser` is wired through shared `act_platform.runtime` helpers and the existing rolling `DpsHistoryStore`:

- status/search/filter: `act_history_status(owner, limit=20, query="")`
- load: `act_history_load(owner, index=0, show=True)`
- delete one encounter: `act_history_delete(owner, index=0)`
- clear all encounters: `act_history_delete(owner, clear=True)`

WebView route: `web/act_report_export.html`, opened from `SAO Menu > ACT 报告/导出 Report Export`; its history sidebar supports search, load, delete, and clear actions.

Entity route: `entity://act/export`, opened from `SAO 菜单 > 面板 > ACT报告/导出`; its history sidebar exposes equivalent load/delete/clear controls.

The shared payload preserves the parity fields `encounters`, `filters`, `cursor`, and `storage_status`. Every listed encounter also carries a stable newest-first `_history_index`, so filtered UI lists can load/delete the intended stored report instead of deleting by filtered-row position. Entity/Tk keeps this low-frequency and dirty-signature gated; destructive actions refresh the cached panel state explicitly rather than polling.

## Current timeline/VCR surface

`encounter_timeline_vcr` is wired through shared `act_platform.runtime` helpers and the owner-scoped ACT `EventBus`:

- status/open: `act_timeline_status(owner, limit=80, query="")`
- play: `act_timeline_play(owner, speed=1.0)`
- pause: `act_timeline_pause(owner)`
- step: `act_timeline_step(owner, delta_ms=1000)`
- seek: `act_timeline_seek(owner, cursor_ms=0)`
- set speed: `act_timeline_set_speed(owner, speed=1.0)`
- filter: `act_timeline_filter(owner, query="")`

WebView route: `web/act_timeline_vcr.html`, opened from `SAO Menu > ACT 时间线/VCR Timeline`.

Entity route: `entity://act/encounter_timeline_vcr`, opened from `SAO 菜单 > 面板 > ACT时间线/VCR`.

The shared payload preserves the parity fields `encounter_id`, `events`, `cursor_ms`, `speed`, and `filters`. Event rows are compact envelopes derived from `EventBus.recent_events()` so UI code does not parse packet payloads directly. Entity/Tk keeps the panel low-frequency with a 350ms refresh throttle and dirty event signatures before rebuilding widgets; animated or high-frequency playback visuals should move to WebView/GPU paths before adding more Tk redraw pressure.

## Current action-log surface

`action_log` is wired through shared `act_platform.runtime` helpers over compact owner-scoped ACT `EventBus` rows:

- status/open: `act_action_log_status(owner, limit=80, query="", topic="", cursor_ms=None)`
- search: `act_action_log_search(owner, query="", limit=80)`
- filter: `act_action_log_filter(owner, topic="", query=None, limit=80)`
- jump to time: `act_action_log_jump_to_time(owner, cursor_ms=0, limit=80)`
- copy: `act_action_log_copy(owner, limit=80, query="", topic="")`

WebView route: `web/act_action_log.html`, opened from `SAO Menu > ACT 行为日志 Action Log`.

Entity route: `entity://act/action_log`, opened from `SAO 菜单 > 面板 > ACT行为日志`.

The shared payload preserves the parity fields `encounter_id`, `rows`, `columns`, `filters`, and `cursor`. Rows are compact table entries derived from EventBus envelopes and include cursor highlighting metadata. Entity/Tk keeps rendering bounded to the first 80 rows, refresh-throttled at 350ms, and dirty-signature gated before rebuilding widgets.

## Current graph/timeseries surface

`graph_timeseries` is wired through shared `act_platform.runtime` helpers over compact owner-scoped ACT `EventBus` rows:

- status/open: `act_graph_timeseries_status(owner, metric="damage", limit=120, query=None, topic=None, time_range_ms=None)`
- select metric: `act_graph_timeseries_select_metric(owner, metric="damage", limit=120)`
- zoom: `act_graph_timeseries_zoom(owner, time_range_ms=0, limit=120)`
- filter: `act_graph_timeseries_filter(owner, query=None, topic=None, limit=120)`
- export: `act_graph_timeseries_export(owner, metric=None, limit=120, query=None, topic=None)`

WebView route: `web/act_graph_timeseries.html`, opened from `SAO Menu > ACT 图表/曲线 Graph`.

Entity route: `entity://act/graph_timeseries`, opened from `SAO 菜单 > 面板 > ACT图表/曲线`.

The shared payload preserves the parity fields `encounter_id`, `series`, `metrics`, `time_range_ms`, and `filters`. Current series are lightweight cumulative curves for `damage`, `heal`, and `event_count`, plus gauge-style `boss_hp_pct` when boss payloads provide HP percentages. WebView owns the richer canvas chart path; Entity/Tk renders a bounded compact bar/table representation with 350ms refresh throttling and dirty signatures so CPU-heavy redraw pressure stays low.

## Current combatant drilldown surface

`combatant_drilldown` is wired through shared `act_platform.runtime` helpers and reuses existing DPS tracker entity detail data:

- status/open: `act_combatant_drilldown_status(owner, combatant_id=None, query=None, focus_target=None)`
- back: `act_combatant_drilldown_back(owner)`
- filter: `act_combatant_drilldown_filter(owner, combatant_id=None, query="")`
- focus target: `act_combatant_drilldown_focus_target(owner, combatant_id=None, target_id="")`

WebView route: `web/act_combatant_drilldown.html`, opened from `SAO Menu > ACT 战斗成员 Drilldown`.

Entity route: `entity://act/combatant_drilldown`, opened from `SAO 菜单 > 面板 > ACT成员钻取`.

The shared payload preserves the parity fields `encounter_id`, `combatant_id`, `summary`, `skills`, `incoming`, and `outgoing`. Live details come from `DpsTracker.get_entity_detail(uid)`; report fallback can read stored report entity rows when available. Entity/Tk keeps the surface bounded, refresh-throttled at 350ms, and dirty-signature gated before rebuilding widgets.

## Current skill drilldown surface

`skill_drilldown` is wired through shared `act_platform.runtime` helpers and reuses combatant skill detail plus owner-scoped ACT `EventBus` timeline references:

- status/open: `act_skill_drilldown_status(owner, combatant_id=None, skill_id=None, query=None, limit=80)`
- back: `act_skill_drilldown_back(owner)`
- filter: `act_skill_drilldown_filter(owner, combatant_id=None, skill_id=None, query="", limit=80)`
- copy: `act_skill_drilldown_copy(owner, combatant_id=None, skill_id=None, query=None, limit=80)`

WebView route: `web/act_skill_drilldown.html`, opened from `SAO Menu > ACT 技能 Drilldown`.

Entity route: `entity://act/skill_drilldown`, opened from `SAO 菜单 > 面板 > ACT技能钻取`.

The shared payload preserves the parity fields `encounter_id`, `combatant_id`, `skill_id`, `casts`, `hits`, `crit_rate`, and `timeline_refs`. Static skill facts come from `DpsTracker.get_entity_detail(uid)`; live references are filtered from `EventBus.recent_events()` by `skill_id`. Entity/Tk keeps the surface bounded to 80 timeline refs, refresh-throttled at 350ms, and dirty-signature gated before rebuilding widgets.

## Safety notes

Treat the SDK as a delivery gate, not documentation-only metadata. If a feature PR changes ACT behavior, it should update the registry, both UI declarations, and parity tests in the same change.

When replacing scaffold bindings with real adapter declarations, keep the SDK UI-framework neutral so replay, WebView, Entity, and plugin code can all share the same contract.
