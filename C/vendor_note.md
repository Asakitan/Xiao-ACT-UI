# Vendor / external SDK notes

Not every dependency the C++ tree needs is available in vcpkg.  A handful of
libraries are either license-gated, redistributable only via the vendor's own
SDK, or intentionally kept out-of-tree because they're picked up at runtime by
plugin hosts rather than linked at build time.  This file lists all of them so
`cmake` can `find_package()` them and CI knows what to install.

## Npcap SDK (packet capture)

- Used by:            `platform/net/capture_pcap.cpp`
- Where to obtain:    https://npcap.com/#download (SDK, not the runtime)
- Install location:   any path; set `NPCAP_SDK_DIR` env var to it, or drop it
                      at `C:\Program Files\Npcap SDK\`.
- Linked libs:        `wpcap.lib`, `Packet.lib` (both x64)
- License note:       Npcap SDK is redistributable but users need to install
                      the Npcap driver from https://npcap.com/#download
                      separately.  We NEVER redistribute the driver.
- CMake glue:         `platform/net/CMakeLists.txt` uses
                      `find_path(NPCAP_INCLUDE_DIR pcap.h ...)` and skips the
                      capture target if not found (compile-time optional).

## Python 3.13 runtime (Python plugin host)

- Used by:            `plugins/host_python/`
- Where to obtain:    https://www.python.org/ftp/python/3.13.x/
- Load style:         Runtime-only.  The plugin host calls `LoadLibraryW` on
                      `python313.dll` at plugin activation, then resolves
                      `Py_InitializeEx`, `PyRun_SimpleString`, etc via
                      `GetProcAddress`.  We NEVER link against
                      `python313.lib` at build time — that would force the
                      C tree to bundle a Python runtime.
- Distribution:       Users install Python separately (or we vendor a
                      relocatable copy under `runtime/python/` at package
                      time via the `pack_cli` tool).

## hostfxr / hostpolicy / .NET runtime (C# plugin host)

- Used by:            `plugins/host_csharp/`
- Where to obtain:    Bundled with the .NET 8+ SDK / runtime.
- Load style:         Runtime-only, same pattern as Python.
                      `plugins/host_csharp/` calls `nethost.h` /
                      `hostfxr_initialize_for_runtime_config` and hosts
                      Roslyn+CoreCLR through `coreclr_delegate_type_t`.
- Distribution:       User must have .NET 8+ runtime installed, or we vendor
                      a self-contained runtime under `runtime/dotnet/`.

## AngelScript SDK (script host)

- Used by:            `plugins/host_angelscript/`
- Where in vcpkg:     `angelscript` (present).  Extension add-ons
                      (`scriptbuilder`, `scriptstdstring`, etc.) live under
                      `plugins/host_angelscript/add_on/` because vcpkg only
                      ships the core.
- License note:       zlib-style, redistributable.

## Lua (script host)

- Used by:            `plugins/host_lua/`
- Where in vcpkg:     `lua` (5.4.6+, present).
- We prefer 5.4 over LuaJIT for ABI stability (see the
  `project_script_plugin_candy_rgba_pipeline` memory: LuaJIT is a moving
  target, plain Lua 5.4 gives predictable calling conventions).

## Emmy / Emma / editor script formats

- Emma is an in-tree tiny DSL implemented in `plugins/host_emma/`.  No
  external SDK required.
- Emmy Lua debug protocol support is optional and pulled in via the vcpkg
  `lua` port + our own protocol implementation.

## DirectComposition / D3D11 / DXGI headers

- All Win32 headers ship with the Windows SDK (10.0.22621.0+).  Users need
  the Windows 11 SDK installed via Visual Studio 2022 Installer.
- We do NOT include `directx-headers` from vcpkg into every target; that
  port is a fallback for Win7/Win8 SDK gaps and we always target Win10+.
  Kept in vcpkg.json for tools that need DXC (HLSL compiler) via the same
  transitive tree.

## WinPcap (legacy)

Explicitly rejected.  WinPcap is unmaintained, does not work on Win10 22H2,
and the Npcap SDK provides a compat mode.  Do NOT add WinPcap to vcpkg.

## Detours

We do NOT use MS Detours.  All inline hooking goes through our own MinHook
port that lives in `security/anti_cheat/hook/minhook_port.cpp`.  Detours has
a per-machine license and a heavier ABI than we want.

## Signing certificate (release)

Not code, but part of the vendor chain.  See `tools/sign_cli/README.md` for
how the release pipeline consumes the code-signing cert (either from the
Windows cert store by SHA1 or a `.pfx` from the operator's local disk).

Test-signing certificates for driver development live under
`security/driver_loader/testcert/` and are regenerated on demand — do NOT
check test certs into git.
