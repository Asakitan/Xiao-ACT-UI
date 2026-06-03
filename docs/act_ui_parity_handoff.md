# ACT WebView / Entity 1:1 Handoff

Date: 2026-06-03

## Background

The ACT UI platform has two first-class UI surfaces: WebView and Entity. The active product constraint is strict 1:1 capability parity: no ACT feature, milestone, or acceptance result is complete if it only ships on one UI surface.

## Current state

- A shared, UI-framework-neutral parity contract exists and is wired into the main ACT replay selftest.
- The contract defines a canonical ACT capability registry and lets WebView/Entity adapters declare routes, actions, payload fields, and render hints against the same IDs.
- A first in-process Python plugin platform now exists under `act_platform/`; replay, WebView, and Entity can publish canonical ACT events to the shared event bus.
- Plugin manager actions are exposed on both surfaces: WebView JS API methods and Entity menu entries use the same runtime helper functions.

## Completed work

- Added `tools.act_ui_parity` with:
  - canonical capability IDs for live overview, drilldowns, logs, graphs, VCR, history, export, triggers/timers, plugins, data health, and offline import;
  - default WebView and Entity adapter contracts;
  - parity validation for missing capabilities, missing actions, missing payload fields, route/render gaps, and WebView/Entity action/payload drift;
  - machine-readable selftest output and `assert_ui_parity()` helper.
- Added regression tests covering:
  - default WebView/Entity 1:1 success;
  - required capability matrix membership;
  - failure detection when Entity plugin manager loses an action;
  - selftest report structure.
- Added `act_platform` with:
  - canonical event envelope helpers;
  - synchronous callback-isolated `EventBus`;
  - in-process Python `PluginManager` with `plugin.json` manifests;
  - shared WebView/Entity runtime helpers for plugin status, enable, disable, and reload;
  - selftest plugin coverage using a temporary plugin directory.
- `act_replay.selftest` now checks both `act_ui_parity_contract` and `act_platform_contract`.

## Key files

- `tools/act_ui_parity.py`
- `tools/act_ui_parity_selftest.py`
- `act_platform/`
- `act_replay/harness.py`
- `act_replay/selftest.py`
- `sao_webview.py`
- `gui_modules/sao_gui_menu_mixin.py`
- `gui_modules/sao_gui_packet_callbacks_mixin.py`

## Validation

Suggested commands from `sao_auto`:

```powershell
python -m py_compile act_platform\*.py act_replay\harness.py act_replay\selftest.py tools\act_ui_parity.py tools\act_ui_parity_selftest.py
python -m tools.act_ui_parity
python -m unittest tools.act_ui_parity_selftest
python -m act_platform.selftest
python -m act_replay.selftest
```

Latest local validation completed with `e:\Py\python.exe` and standard-library unittest; pytest is not required.

## Open issues

- Wire real WebView adapter declarations to `UiAdapterContract` instead of only using default scaffold routes.
- Wire real Entity adapter declarations to `UiAdapterContract` instead of only using default scaffold routes.
- Decide whether the canonical registry should later be exported as JSON for front-end build-time validation.
- Add richer plugin manager panels/pages beyond the current WebView API + Entity menu entries.
- Add plugin sandbox/permission hardening if untrusted plugins are allowed; current platform is in-process and intended for trusted local Python plugins.

## Risks / notes

- Future developers must not treat Entity as a lightweight/minimal subset. Entity can render differently, but capability IDs, actions, shared payload fields, and acceptance gates must stay equivalent.
- Rich WebView graph/VCR rendering may map to compact Entity tables or panels, but filter/playback/export controls still need shared semantics.
- Plugin manager, history, export, trigger/timer, and data-source health actions are explicitly part of the parity gate.

## Recommended next steps

1. Run the validation commands above.
2. Replace default scaffold contracts with declarations sourced from real WebView and Entity adapters.
3. Build first real plugin manager UI page/panel while preserving the shared runtime helper surface.
4. Continue event-bus integration in live parser/bridge paths where any callbacks still bypass WebView/Entity owners.
5. For each new ACT feature PR, require updates to the canonical registry, both UI bindings, and parity tests in the same change.
