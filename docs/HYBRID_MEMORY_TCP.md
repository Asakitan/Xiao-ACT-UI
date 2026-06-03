# Hybrid TCP And Memory Data Source

This document describes the conservative data-source policy for SAO Auto's ACT platform.

The short version: TCP remains the safe combat source. Memory probing is read-only, self-state focused, and must fail closed back to TCP-visible health instead of inventing combat data.

## Safety Boundary

Allowed:

- Read-only process memory reads.
- TCP-confirmed semantic anchors.
- Low-frequency self-state polling.
- Cached address reuse after validation.
- Clear health reporting and fallback reasons.

Not allowed:

- Memory writes.
- Code injection.
- Hooks or detours.
- Bypassing anti-cheat controls.
- Treating volatile heap addresses as durable facts.
- Letting memory data override TCP combat truth without explicit validation.

## Runtime Modes

`PacketBridge` accepts these `data_source` modes:

| Mode | Behavior |
| --- | --- |
| `tcp` | Start TCP capture and `StarResonanceParserAdapter`; no memory source is started. |
| `memory` | Start `UnifiedDataSource` only. Current scope is self-state; combat/entity/boss/scene streams are not produced by memory-only mode. |
| `hybrid` | Start `UnifiedDataSource`, then keep TCP capture as combat/entity/boss/scene ground truth. Memory enriches self identity/HP/resources when available. |
| `auto` | Try memory/hybrid startup first; if it fails, fall back to TCP where possible. |

## Current Implementation

`mem_probe/unified_source.py` provides `UnifiedDataSource`, the entry point expected by `PacketBridge` for memory/hybrid/auto modes.

Important constraints:

- `UnifiedDataSource` does not create another `PacketBridge`, so startup cannot recurse.
- It delegates memory self-state work to `MemStateBridge`.
- It passes the live `PacketBridge` into `MemStateBridge`, allowing TCP parser state to build anchor packs.
- Unsupported streams remain reported as TCP-owned or fallback-owned.

Current watcher ownership in health:

```json
{
  "self": "memory_first",
  "combat": "tcp_fallback",
  "entity": "tcp_fallback",
  "boss": "tcp_fallback",
  "scene": "tcp_fallback"
}
```

## Anchor Policy

The memory path should use TCP-confirmed semantic anchors before scanning.

Anchor examples:

- confirmed player uid;
- level/profession facts from parser state;
- observed skill level ids;
- scene/dungeon context when available.

`MemStateBridge` builds an `AnchorPack` from the live packet parser through `AnchorMemoryReader.build_anchor_from_parser(parser)`.

`AnchorMemoryReader.find_self(anchor)` should only scan when `anchor.is_strong()` is true. Candidate addresses are cross-validated through readbacks such as uid, max HP, role level, profession, and matched skill ids.

## Health Shape

`PacketBridge.health()` returns top-level bridge status and includes memory/parser details when present.

Typical top-level fields:

- `data_source`
- `running`
- `error_msg`
- `parser_adapter`
- `parser_adapter_selection`: requested adapter id, selected adapter id, selection mode (`builtin` or `plugin`), and fallback reason when the live bridge rejects a requested plugin adapter.
- `mem`

`UnifiedDataSource.health()` includes:

- `data_source`: `unified`
- `requested_mode`
- `mode`
- `running`
- `started`
- `alive`
- `is_memory_active`
- `status`
- `last_error`
- `started_at`
- `uptime_s`
- `watchers`
- `self`
- `snapshot_available`

The data-source health UI should prefer these health fields over guessing from mode labels.

## Failure And Fallback Rules

- If memory startup fails in `memory` mode, the bridge should report an error rather than silently claiming combat data exists.
- If memory startup fails in `auto` mode, TCP can continue as the safe fallback.
- If polling repeatedly fails, `MemSelfStateProvider` can switch its internal mode to `tcp` and report the error.
- Impossible self-state values must be ignored or treated as stale.
- Missing Cython scanner support must degrade with a visible warning instead of crashing the packet path.

## Configuration Guidance

Prefer TCP as the default operational mode.

When adding or exposing memory settings, keep these gates visible:

- `mem_data_source`
- `mem_auto_scan_enabled`
- `mem_auto_scan_interval_s`
- `mem_require_admin`
- `mem_max_scan_regions_mb`
- `mem_allow_static_fallback`
- `mem_show_risk_warning`

If a setting is not implemented yet, do not present it as an active runtime guarantee. Use this list as the policy target for future UI/config work.

## Troubleshooting

Use the shared data-source health helpers:

```powershell
e:\Py\python.exe -m unittest tools.unified_source_selftest tools.act_data_source_health_selftest
```

Manual checks when the game and UI runtime are available:

- `getDataSourceHealth` shows requested mode and active memory status.
- TCP damage still updates DPS in `hybrid`.
- Memory HP/self identity can update without producing combat events.
- Weak anchors do not trigger wide scans.
- Switching `tcp`/`hybrid`/`auto` restarts cleanly.
- Health reports fallback reason instead of hiding memory failures.

## Validation

Recommended offline validation:

```powershell
e:\Py\python.exe -m py_compile mem_probe\unified_source.py mem_probe\il2cpp\mem_state_bridge.py mem_probe\il2cpp\mem_self_state_provider.py mem_probe\il2cpp\mem_state_anchor.py net\packet_bridge.py
e:\Py\python.exe -m unittest tools.unified_source_selftest tools.act_data_source_health_selftest
e:\Py\python.exe -m act_replay.selftest
git diff --check
```

Live validation must be manual and opt-in. Do not require a live game process for CI-like tests.
