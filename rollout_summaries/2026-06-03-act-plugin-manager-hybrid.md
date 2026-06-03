# 2026-06-03 — ACT Plugin Manager UI + Hybrid Data Source Handoff

## Background

The ACT-like platform work is progressing in slices after the event bus and Python plugin runtime foundation landed in commit `8f3b71b`. The user asked to keep executing the plan autonomously, with WebView and Entity UI advancing 1:1 and Python external plugins made easy to use.

## Current state

Commit `aa7f2f1` adds the second slice:

- Rich ACT plugin manager UI for WebView and Entity/Tk.
- Plugin authoring documentation and a runnable example plugin.
- Missing `mem_probe.unified_source.UnifiedDataSource` adapter for PacketBridge memory/hybrid/auto modes.
- Hybrid mode now starts read-only memory self-state and keeps TCP combat/entity/boss fallback active.

## Completed work

### WebView plugin manager

- Added `web/plugin_manager.html`.
- Wired a hidden `SAO-PluginManager` pywebview window in `sao_webview.py`.
- Added show/hide/fade/click-through helpers and shutdown cleanup.
- Added `toggle_plugin_manager` menu action and WebView menu entry.
- Extended `web/pywebview-shim.js` with plugin/menu wrappers for C# compatibility fallback.

### Entity/Tk plugin manager

- Added `gui_modules/sao_gui_plugin_manager.py`.
- Wired `PluginManagerPanel` into `sao_gui.py`, `sao_gui_panels_mixin.py`, `sao_gui_menu_mixin.py`, and `sao_gui_lifecycle_mixin.py`.
- Entity menu now has `ACT插件管理面板`, matching the WebView Plugin Manager action surface.

### Plugin SDK and example

- Added `docs/PLUGIN_SDK.md` with manifest schema, lifecycle hooks, context methods, topics, and safety notes.
- Added example plugin under `plugins/hello_act_plugin/`:
  - `plugin.json`
  - `plugin.py`

### Hybrid data source

- Added `mem_probe/unified_source.py`.
- `UnifiedDataSource` wraps `MemStateBridge` conservatively for self-state only.
- `net/packet_bridge.py` now passes `packet_bridge=self` so semantic anchor packs can reuse live parser context.
- Hybrid mode keeps TCP fallback active for damage, monster, boss, scene streams because boss/entity HP memory decoding is still not proven.

## Validation

Run from `sao_auto/` with `e:\Py\python.exe`:

- `python -m py_compile` on touched Python files — passed.
- `python -m act_platform.selftest` — passed (`ok: true`, plugin count 1).
- `python -m unittest tools.act_ui_parity_selftest` — passed (4 tests OK).
- `python -m act_replay.selftest` — passed (`ok: true`, `boss_hp_source: tcp`, `render_boss_hp_source: tcp`).
- `python -c "from mem_probe.unified_source import UnifiedDataSource; print(UnifiedDataSource.__name__)"` — passed.
- `git diff --check` on touched files — passed.
- VS Code diagnostics on touched Python files — no errors.

## Open issues / risks

- WebView Plugin Manager uses the Python pywebview API as the real supported path. The C# bridge shim exposes command names, but C# command handlers for `act.plugins.*` are not implemented here.
- `UnifiedDataSource` intentionally does not claim boss/entity memory HP support. TCP remains authoritative for those streams in hybrid mode.
- `auto` mode keeps previous memory-first semantics: if memory starts successfully, TCP is not started. If the desired behavior becomes "auto = memory self-state + TCP fallback", adjust `PacketBridge.start()` separately.
- No visual browser/UI screenshot QA was run in this slice; validation was compile/selftest/contract based.

## Recommended next steps

1. Add live/manual UI smoke for both Plugin Manager panels:
   - WebView: menu opens `SAO-PluginManager`, enable/disable/reload buttons refresh cards.
   - Entity: menu opens `PluginManagerPanel`, enable/disable/reload buttons refresh cards.
2. Add a focused `UnifiedDataSource` unit test with a fake `MemStateBridge` if the codebase later adds a test seam.
3. Implement C# bridge handlers for `act.plugins.status/list/enable/disable/reload` only if the C# WebView2 host needs native plugin-management support.
4. Continue ACT roadmap slices: richer plugin API hooks, report/export surfaces, trigger/timer authoring UI, and deeper analysis drilldowns.
