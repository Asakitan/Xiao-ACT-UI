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
  `pymini` (priority 10) provides `.py`; no CPython fallback provider is
  registered in the current tree. CPython's own `load_local` uses its native
  import path and reports foreign script modules as unsupported. Non-script
  payloads return `path_only`; built-in `.py/.lua/.emma/.as/.cs` files remain
  scripts even when their runtime is absent. Only an unsupported provider
  preflight advances to the next candidate. Once execution starts, its result
  is terminal; syntax/runtime/I/O failures are not retried in another engine.
  A contained absent file is `missing`, missing runtime capability is
  `unsupported`, and invalid arguments, escaped paths, inspection failures or
  execution failures return a nonzero status with `failed`. Language bridges
  propagate failures through their native error channel rather than silently
  converting them into a missing file.
- `ctx_surface` — per-language ctx capability table.  Hosts call
  `ctx_surface_note(language, "ui.canvas")`/`ctx_surface_note_all` for every
  name they bind; adapters use `ctx_surface_missing` at load time to emit an
  advisory list when a manifest's `platform.binds` or `runtime_feature:<name>`
  requires entries exceed what the host actually binds.

Nothing here knows about games or specific plugins — routing keys on
`engine_kind` + file extension only.

## Runtime identities and ownership

Python, Lua, Emma, AngelScript and C# remain five language identities.
Python has distinct CPython and native-subset pymini implementations; C# has
distinct managed and native-subset csmini implementations. Lua and AngelScript
module providers require an existing state for the caller's context; the
managed C# provider does not compile source modules or expose a callable
foreign-module proxy through its JSON result.

Emma exported module callables retain their module owner, including callables
nested in containers or returned by another callable. Exceptional helper-load
exits release the acquired Emma handle, and member-read errors propagate.
AngelScript exported helper callables set the same plugin context user-data
slot as direct module calls.

The loader owns pymini/csmini helper attachments per context generation through
the internal `context_runtime_lease` API instead of global helper maps.
`runtime_bridge_load_local`, mini module access, exported function calls and
entry/helper SDK callbacks hold invocation leases. Busy/reentrant unload returns
`BUSY`; resource release closes admission and retains attachments for retry
while invocations or registrations remain active, without waiting on itself.

After event/platform drain, data-source stop, Entity destruction, platform
release and registration removal, loader retirement calls `*_drop_callbacks`
before releasing helper values and interpreters. Finalizers run outside loader,
context and helper mutexes and interpreter guards; builder SDK cleanup drains
before callback boxes are released. Synchronous data-source stop callbacks can
call existing helpers under a scoped drain lease, but cannot add registrations.
This is independent of the calling host and adds no public C ABI layout change
or loader-to-script_ctx dependency.

Retained module proxies and exported functions hold weak helper identity and
opaque value keys. Actual values remain helper-owned until retirement, including
functions in containers, callback arguments and returned callables. Retired
`get`, `call` and exported calls return `SAO_ERR_HANDLE_INVALID` before touching
the interpreter/context; member enumeration returns an empty list. Weak
backreferences, including each interpreter's captured context generation, avoid
helper/proxy ownership cycles; SDK callbacks never rebind an expired generation
through a reused context address. Calls pin their callable
across nested callbacks that replace the original binding. Provider removal
alone is not teardown.

This review used source/diff inspection and editor diagnostics only; it does
not establish build, unload, cross-language runtime or full-language coverage.
