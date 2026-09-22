# `platform/sdk`

The **single ABI plugins see**.  Everything else in the platform is
reached through `SaoSdkContext`'s vtable — plugins never link
`sao_core.dll`, `sao_engine.dll` or `sao_ui.dll` directly.

## Design summary

The loader-to-launcher platform-provider bridge is independently versioned at 1.5;
it appends MMF/shared-texture source and truthful GPU-availability/source-active slots
without changing `SaoSdkContext` or flat Entity row layouts. Language hosts normalize
nested menus into current-page Entity rows, retaining navigation and callback ownership
outside the platform. Managed C# uses the native source APIs rather than polling MMF
and uploading frames from a managed timer. UI's source/state contract is ABI 1.19;
the color route is GPU copy plus shader premultiplication, with native MMF alpha/
hit-test and fallback retained. Navigation queues Back/open requests until candidate
publication, without a fixed depth cap; budgets are 4096 nodes, 1 MiB text, 1024 root
data rows and 1023 child data rows plus Back. CPython/pymini execute callable submenus
lazily along the selected path; other hosts currently run subbuilders during refresh.

The pre-timer-revision baseline includes Debug compilation, Hardened acceptance
(22 PEs/76 files), six-host 128-level roundtrips and 566 direct UI texture checks.
The host matrix is CPython, pymini, Lua, Emma, AngelScript and managed C#; native csmini
has static-review/build evidence only, not that 128-level runtime coverage.
Cutegirl's per-connection pipe inbox and Stop handshake reset are implemented, and
its rebuilt managed artifact matches the Debug mirror; neither fact proves owner-thread
dispatch. Core timer callbacks previously ran on a worker thread. The new launcher
owner bind/pump/unbind integration passed independent review and final Debug/Hardened
builds. Its real-provider probe passed 94 checks/seven native callbacks with zero
timer/worker residue, including self/peer cancellation, headless layer replacement
and retained input reentry checks. It is not GPU or full-product coverage. All five
existing regression modes passed again; Hardened acceptance passed 22 PEs/76 files.
Final bundle identity is recorded in session-77; full external-engine integration
remains a separate gate.

Legacy callback-backed panels now refresh their real native body on open, redraw and
actions. `sao_sdk_panel_open` validates context ownership before showing an existing
panel. Legacy canvas placeholders are removed; canonical panel parsing owns drawing.
The platform overlay provider renders bounded canvas/RGBA documents into transparent
compositor layers and replaces them transactionally; stale tokens never clear a newer
surface. Native/SDK mutation reentry returns BUSY rather than nesting panel transactions.

- `include/sao/sdk/sao_sdk.h` is the one header a plugin includes.
- It transitively drags in the sub-tables (UI / event / mem / net /
  config / hotkey / TTS / banner) — every one is a `struct` of function
  pointers stored inside `SaoSdkContext`.
- Plugins export exactly two symbols:
  ```c
  sao_sdk_status_t SAO_SDK_CALL sao_plugin_init(const struct SaoSdkContext*);
  void            SAO_SDK_CALL sao_plugin_shutdown(const struct SaoSdkContext*);
  ```
- The platform loader loads the plugin,
  GetProcAddresses those two symbols, calls `sao_sdk_bind_context` to
  populate a per-plugin `SaoSdkContext`, then invokes init.
- Growth is append-only: NEVER reorder or remove fields from
  `SaoSdkContext` or its sub-tables.  Bump `SAO_SDK_ABI_VERSION_MINOR`.

## Corresponding Python source

| C++ sub-table         | Python surface                                    |
| --------------------- | ------------------------------------------------- |
| `SaoSdkUiTable`       | `act_platform/plugins.py::PluginContext.ui`       |
| `SaoSdkEventTable`    | `act_platform/plugins.py::PluginContext.event_bus`|
| `SaoSdkMemTable`      | `mem_probe/` surfaces exposed to plugins          |
| `SaoSdkNetTable`      | `plugins/star_resonance_plugin/net/` frame feeder |
| `SaoSdkConfigTable`   | `act_platform/plugins.py::PluginContext.config`   |
| `SaoSdkHotkeyTable`   | `sao_gui_hotkey.py` surface                       |
| `SaoSdkTtsTable`      | `plugins/star_resonance_plugin/tts.py`            |
| `SaoSdkBannerTable`   | banner popup in the ACT mechanics UI              |

## Public headers

- `sao_sdk.h` — single include for plugins.
- `sao_sdk_version.h` — ABI version + export macro.
- `sao_sdk_context.h` — `SaoSdkContext` + every sub-table struct.
- `sao_sdk_ui.h`, `sao_sdk_event.h`, `sao_sdk_mem.h`, `sao_sdk_net.h`,
  `sao_sdk_config.h`, `sao_sdk_hotkey.h`, `sao_sdk_tts.h`,
  `sao_sdk_banner.h` — inline convenience wrappers for each sub-table.
- `sao_sdk_provider.h` — ctx-level capability surface:`overlay_set/clear`
  token 对、`sao_sdk_overlay_clear_surface`（session-44，surface-keyed）、
  `sao_sdk_notify_show/dismiss`、`sao_sdk_platform_*`（render dispatch、
  panel open、streaming apply）等独立导出，不经 ctx vtable。

## Implementation map

- **ABI foundation** — headers + baseline `sao_sdk_bind_context`
  that only zeroes the context struct.
- **Runtime binding** — implement `sao_sdk_bind_context`: build the
  vtables that forward into the platform modules, populate `ctx_impl`
  with a per-plugin state object, wire the scoped config prefix.
- **Plugin validation** — first non-star-resonance plugin ships as end-to-end
  validation.

## Build integration notes

- `sao_sdk.dll` **must** be co-located with `SaoAuto.exe` in the
  runtime output dir.  Plugins LoadLibrary it by relative path.
- No install-time registration needed — the DLL is not COM-registered,
  it's just a shared library.
- `.lib` import stub exposes `sao_sdk_abi_version` and
  `sao_sdk_bind_context` — the vtable factory — plus the independent
  `sao_sdk_provider.h` exports (overlay/notify/platform surface, e.g.
  `sao_sdk_overlay_clear_surface`, `sao_sdk_notify_dismiss`,
  `sao_sdk_platform_render_dispatch`).  Everything else travels through
  the ctx vtable.
