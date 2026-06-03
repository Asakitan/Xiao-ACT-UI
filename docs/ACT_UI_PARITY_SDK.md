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

## Safety notes

Treat the SDK as a delivery gate, not documentation-only metadata. If a feature PR changes ACT behavior, it should update the registry, both UI declarations, and parity tests in the same change.

When replacing scaffold bindings with real adapter declarations, keep the SDK UI-framework neutral so replay, WebView, Entity, and plugin code can all share the same contract.