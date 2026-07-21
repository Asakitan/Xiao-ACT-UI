# `platform/engine`

**Game-agnostic** runtime primitives that ship inside `SaoAuto.exe`.
This layer knows nothing about any specific game — no DPS trackers, no
boss HP, no encounter clocks, no mechanic triggers.  Those concepts
belong to game plugins (see `plugins/star_resonance_plugin/` for the
star-resonance implementation).

## What `platform/engine/` provides

Only five universal abstractions:

| Header           | Role                                                            |
| ---------------- | --------------------------------------------------------------- |
| `event_bus.h`    | Sync publish/subscribe with ring buffer + ephemeral topics.     |
| `state.h`        | Typed key/value store with per-value source metadata.           |
| `ui_spec.h`      | Normalize / validate a plugin's declarative UI spec (JSON).     |
| `render_hook.h`  | Priority-ordered hook chain + per-plugin overlay registry.      |
| `runtime.h`      | Aggregate context owning the four objects above.                |

Plus:

- `abi.h` — export macro + `sao_engine_abi_version()`.

## What lives elsewhere (NOT here)

Game-specific state — everything below is deliberately outside this
directory and belongs to the individual game plugin that owns it:

| Concept                                    | Correct home                                |
| ------------------------------------------ | ------------------------------------------- |
| DPS tracker / damage rollups               | `plugins/<game>_plugin/`                    |
| Boss HP / breaking / shield / extinction   | `plugins/<game>_plugin/`                    |
| Combat encounter start/end + summary       | `plugins/<game>_plugin/`                    |
| Mechanic triggers (banner / TTS / sound)   | `plugins/<game>_plugin/`                    |
| Skill / buff / element name tables         | `plugins/<game>_plugin/`                    |
| Any protocol parsing (TCP frames, packets) | `plugins/<game>_plugin/` (using `platform/net/`) |
| IL2CPP klass indexes / field readers       | `plugins/<game>_plugin/` (using `platform/core/` + `platform/rt_io/`) |

Plugins allocate their own aggregates, run their own accumulation
logic and communicate outbound via the platform event bus (opaque JSON
payloads on plugin-owned topic names).  The platform side only sees
UTF-8 JSON crossing the event bus — it never parses it.

## Corresponding Python source

| C++ header       | Python source                                       |
| ---------------- | --------------------------------------------------- |
| `event_bus.h`    | `act_platform/event_bus.py`                         |
| `state.h`        | `mem_state_bridge.py`, `unified_data_source.py`     |
| `ui_spec.h`      | `act_platform/ui_spec.py`                           |
| `render_hook.h`  | `act_platform/render_hooks.py`                      |
| `runtime.h`      | `act_platform/runtime.py`                           |

## EventBus implementation status

`sao_platform_engine` ships `src/event_bus_adapter.cpp` as its production
EventBus path.  The adapter implements the base C ABI and the `_priority`
entry points, including synchronous exact/wildcard dispatch, owner and token
unsubscribe, ephemeral topics, bounded recent-event retention, counters,
priority ordering, and cancellation.  The `_wave5` symbols remain only as
compatibility ABI aliases that forward to `_priority`; new consumers should use
the `_priority` API.

`src/event_bus.cpp` is a historical translation unit that CMake deliberately
does not compile.  Its dead base-ABI section contains seven
`SAO_STATUS_ERR_NOT_IMPLEMENTED` returns; the same file also contains an
obsolete priority-dispatch implementation and a real destroy function, but
none of that file is part of the shipping DLL.  Keep those seven returns unchanged as audit
inventory.  Static occurrence counts for this dead file must not be reported
as runtime completeness; executable tests against the linked engine target
define production behavior.

The other engine primitives (`state`, `ui_spec`, `render_hook`, and `runtime`)
also have production implementations in the source files selected by this
module's CMake target.
