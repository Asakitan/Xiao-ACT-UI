# `platform/scripting`

The abstract `IScriptEngine` interface + registry that lets the platform
run five different scripting languages *without* knowing anything about
their interpreters.

## Non-goal

This module **does not** ship any interpreter code.  The concrete hosts
for Python, Emma, AngelScript, Lua and C# live in the plugin host layer
under:

```
../plugins/python_host/
../plugins/emma_host/
../plugins/angelscript_host/
../plugins/lua_host/
../plugins/csharp_host/
```

Each host DLL implements `SaoScriptEngineVTable`, registers itself via
`sao_script_registry_register()` at DllMain time and answers
`context_create` / `context_run` / `context_call` for the platform.

## Corresponding Python source

Python has no direct 1:1 equivalent — the closest existing surface is
the ad-hoc language dispatch inside
`sao_auto/python/plugins/workshop/lang_hosts/` which resolves imports by
suffix.  This module formalises that into a proper factory registry.

## Public headers

- `abi.h` — export macro + `sao_scripting_abi_version()`.
- `script_error.h` — `SaoScriptError` rich error struct.
- `script_context.h` — per-script execution context (config, run, call).
- `script_engine.h` — `SaoScriptEngineVTable`: what concrete hosts fill in.
- `script_registry.h` — process-scoped factory registry.

## Implementation map

- **Registry foundation** — headers + registry implementation.
- **Concrete hosts** — the five language hosts are implemented and
  wire themselves into the registry at DLL init.

## Build integration notes

`sao_platform_scripting` is a **STATIC** library.  Every concrete host
under `../plugins/*_host/` links it privately so each host DLL contains
its own inlined copy of the registry symbols.  This is deliberate: it
lets a host be unloaded without dragging the registry with it.

If the top-level build links `sao::scripting` into more than one
SHARED target simultaneously (e.g. into `sao_sdk.dll` AND into a host
DLL), that's a design smell — the registry should live in exactly one
place, currently the host DLLs.
