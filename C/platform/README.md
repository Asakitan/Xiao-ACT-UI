# SAO Auto — `platform/`

The **game-agnostic** runtime that lives inside `SaoAuto.exe`.

`platform/` is the C++ 1:1 rewrite of the Python `sao_auto/python/act_platform/` +
`sao_auto/python/render/` + `sao_auto/python/ui_gpu/` + `sao_auto/python/mem_probe/`
stack.  It provides an event bus, overlay compositor, native widget kit, a
kernel-IO bridge and a plugin SDK — with **no game knowledge** baked in.
Everything star-resonance specific stays under `../plugins/star_resonance_plugin/`.

## Hard rule — `platform/engine/` is NOT the place for game logic

`platform/engine/` intentionally ships only five universal primitives:
event bus, state store, UI spec, render hook, aggregate runtime.  It
does NOT contain any of the following — they belong to game plugins:

- DPS trackers, damage rollups, per-actor combat stats
- Boss HP / breaking / shield / extinction state machines
- Encounter start/end clocks and rollups
- Mechanic triggers (banner / TTS / sound / custom actions)
- Skill / buff / element name tables
- Any protocol parsing (TCP frames, wire packets)

If you find yourself typing "boss" / "dps" / "encounter" / "mechanic"
under `platform/`, stop — that code belongs in `plugins/<game>_plugin/`
using the SDK to talk to the platform via the event bus + state store.

## Non-negotiable constraints

- The platform NEVER imports Python.  No `<Python.h>`, no pybind11, no ctypes
  hand-off.  `SaoAuto.exe` is a native executable.
- The platform NEVER references anything under `../plugins/`.  Plugins register
  themselves via the SDK C ABI (`sdk/`).
- All UTF-8, no BOM.  Windows-only for now (Win32/DWM/D3D11/DirectComposition
  are used throughout `ui/`).
- No API panics.  Every ABI entry returns a `sao_status_t` (int32_t) and never
  throws across the module boundary.

## Sub-modules

| Module      | One-line role                                             | Corresponding Python source           |
| ----------- | --------------------------------------------------------- | ------------------------------------- |
| `core/`     | Runtime primitives (status/log/process/memory/window/…)   | `mem_probe/`, misc `sao_gui/` helpers |
| `net/`      | Packet capture (Npcap), HTTP/WS/TLS clients               | `plugins/star_resonance_plugin/net/`  |
| `engine/`   | Event bus, state store, UI spec, render hook (no game logic) | `act_platform/`                    |
| `rt_io/`    | Game-agnostic kernel-IO bridge (main ↔ helper subprocess) | `mem_probe/rt_io.py`, `rt_io_proxy.py`|
| `ui/`       | Overlay host, compositor, widget kit, theme               | `render/`, `ui_gpu/`, `sao_theme/`    |
| `sdk/`      | Single-header C ABI for plugins to link against           | `act_platform/plugins.py` surface     |
| `scripting/`| Abstract `IScriptEngine` for 5 script hosts               | `act_platform/plugins.py` script side |

## Where the plugins live

Plugins are **not** in this directory.  They live under:

```
sao_auto/C/plugins/               ← 5 script hosts (Python/Emma/AS/Lua/C#)
                                     built by Agent 4
sao_auto/C/plugins/star_resonance_plugin/  ← the ex-game-specific plugin
```

Plugins include `<sao/sdk/sao_sdk.h>` and link `sao_sdk.dll` at load time.  The
platform side of that boundary is `sdk/src/sdk_export.cpp`.

## Build integration for Agent 6 (top-level CMake)

The top-level `sao_auto/C/CMakeLists.txt` should:

1. `add_subdirectory(platform)` after Agent 3 (kernel/security) but before
   `plugins/`.
2. Consume the aggregate target `sao::platform` from anything that wants the
   full runtime, or the individual `sao::core` / `sao::net` / etc. targets
   for finer-grained deps.
3. Read `SAO_PLATFORM_VERSION` if it needs to stamp the exe.

## ABI versions (each module owns its own)

Each module header exposes `sao_<mod>_abi_version()` returning a 32-bit
value: `(major << 16) | minor`.  Plugins call these on load and refuse to
proceed if the ABI mismatches.

| Module      | Symbol                       | Version |
| ----------- | ---------------------------- | ------- |
| core        | `sao_core_abi_version()`     | 1.0     |
| net         | `sao_net_abi_version()`      | 1.0     |
| engine      | `sao_engine_abi_version()`   | 1.0     |
| rt_io       | `sao_rt_io_abi_version()`    | 1.0     |
| ui          | `sao_ui_abi_version()`       | 1.2     |
| sdk         | `sao_sdk_abi_version()`      | 1.0     |
| scripting   | `sao_scripting_abi_version()`| 1.0     |

## Phase plan

This directory is **skeleton only** for the current phase — every function is
a stub returning `SAO_STATUS_NOT_IMPLEMENTED`.  Later phases fill in:

- Phase 2 — `core/` real implementations (RPM helpers, module enum, string, path)
  and `rt_io/` proxy + helper wire protocol.
- Phase 3 — `net/` Npcap capture + HTTP client; `rt_io/` helper bootstrap +
  authenticated session + cleanup.
- Phase 4 — `engine/` event bus + state store + UI spec pipeline.
- Phase 5 — `ui/` overlay compositor + widget kit ported from legacy overlay/.
- Phase 6 — `sdk/` full C ABI wired end-to-end; first non-star-resonance plugin.
- Phase 7 — `scripting/` five script-engine hosts (owned by Agent 4).
