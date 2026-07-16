# `platform/sdk`

The **single ABI plugins see**.  Everything else in the platform is
reached through `SaoSdkContext`'s vtable — plugins never link
`sao_core.dll`, `sao_engine.dll` or `sao_ui.dll` directly.

## Design summary

- `include/sao/sdk/sao_sdk.h` is the one header a plugin includes.
- It transitively drags in the sub-tables (UI / event / mem / net /
  config / hotkey / TTS / banner) — every one is a `struct` of function
  pointers stored inside `SaoSdkContext`.
- Plugins export exactly two symbols:
  ```c
  sao_sdk_status_t SAO_SDK_CALL sao_plugin_init(const struct SaoSdkContext*);
  void            SAO_SDK_CALL sao_plugin_shutdown(const struct SaoSdkContext*);
  ```
- The platform's loader (owned by Agent 4) LoadLibrarys the plugin,
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

## Phase plan

- **Phase 1 (this skeleton)** — headers + stub `sao_sdk_bind_context`
  that only zeroes the context struct.
- **Phase 6** — implement `sao_sdk_bind_context` for real: build the
  vtables that forward into the platform modules, populate `ctx_impl`
  with a per-plugin state object, wire the scoped config prefix.
- **Phase 7** — first non-star-resonance plugin ships as end-to-end
  validation.

## Integration notes for Agent 6

- `sao_sdk.dll` **must** be co-located with `SaoAuto.exe` in the
  runtime output dir.  Plugins LoadLibrary it by relative path.
- No install-time registration needed — the DLL is not COM-registered,
  it's just a shared library.
- `.lib` import stub only exposes `sao_sdk_abi_version` and
  `sao_sdk_bind_context` — the vtable factory.  Every other symbol
  travels through the ctx vtable.
