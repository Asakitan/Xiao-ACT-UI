# script_ctx — shared script-side ctx helpers

Static support library linked by every script host.  Three pieces:

- `script_ui` — the shared `ctx.ui.*` builder.  Mirrors
  `act_platform/ui_spec.py::UI` (24 methods) 1:1: a host marshals call args to
  JSON (positional array or kwargs object) and gets back the same spec dict
  the Python builder would emit.  Downstream normalization stays in
  `sao_engine_ui_spec_normalize` — this lib emits *builder* shapes only.
- `runtime_bridge` — `ctx.load_local` dispatch + cross-language module
  proxies (`script_value`, `script_module`, `script_engine_ops` registry).
  Extension-keyed providers are registered by host adapters:
  `pymini` (priority 10) owns `.py` first, python_host (priority 90) backs it
  when a real CPython is embedded; each script host registers its own
  extension so nested same-language `load_local` returns a module, and other
  payloads fall through to `path_only`.  Unloadable scripts degrade to
  `unsupported` → host returns `nil`/`None`/`null` + `missing_runtime` log.
- `ctx_surface` — per-language ctx capability table.  Hosts call
  `ctx_surface_note(language, "ui.canvas")`/`ctx_surface_note_all` for every
  name they bind; adapters use `ctx_surface_missing` at load time to emit an
  advisory list when a manifest's `platform.binds` or `runtime_feature:<name>`
  requires entries exceed what the host actually binds.

Nothing here knows about games or specific plugins — routing keys on
`engine_kind` + file extension only.
