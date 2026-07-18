# SAO Auto — platform/ layer.
#
# The platform layer is the *game-agnostic* runtime that ships inside
# SaoAuto.exe.  It provides the ACT event bus, overlay compositor, native
# widget kit, plugin SDK and script-engine abstractions.  It never imports
# Python and never references any code under plugins/.
#
# Sub-modules:
#   core       — universal runtime primitives (status, logging, memory,
#                process, window, time, string, threading, path, event).
#   net        — packet capture, HTTP/WS clients, TLS wrapper.
#   engine     — event bus, state store, UI spec, render hook, aggregate
#                runtime.  Game-agnostic — DPS / boss / encounter /
#                mechanic trigger logic lives in plugins.
#   rt_io      — game-agnostic kernel-IO bridge.  Owns the v3 wire
#                protocol between the main process (proxy) and the
#                SaoUiHelper.exe subprocess that holds the driver
#                handle.  See rt_io/README.md.
#   ui         — overlay host, compositor, DirectComposition/D3D11 device,
#                Direct2D widget kit, theme, popup, panel, input router,
#                z-order state machine.
#   sdk        — the single-header C ABI plugins consume, plus the
#                platform-side dispatch table exposed to plugin hosts.
#   scripting  — abstract IScriptEngine interface + registry (concrete
#                Python/Emma/AngelScript/Lua/C# hosts live under
#                sao_auto/C/plugins/).

set(SAO_PLATFORM_VERSION_MAJOR 1)
set(SAO_PLATFORM_VERSION_MINOR 0)
set(SAO_PLATFORM_VERSION_PATCH 0)
set(SAO_PLATFORM_VERSION
    "${SAO_PLATFORM_VERSION_MAJOR}.${SAO_PLATFORM_VERSION_MINOR}.${SAO_PLATFORM_VERSION_PATCH}")

message(STATUS "SAO platform layer version: ${SAO_PLATFORM_VERSION}")

# Modules are always built together — the platform's whole point is that
# core/net/engine/rt_io/ui/sdk/scripting form one coherent runtime.
# Individual module toggles land at the top-level CMake, not here.
add_subdirectory(core)
add_subdirectory(net)
add_subdirectory(engine)
add_subdirectory(rt_io)
add_subdirectory(ui)
add_subdirectory(sdk)
add_subdirectory(scripting)
add_subdirectory(gpu_hunt)

# Convenience meta-target: link this to pull the whole platform in.
add_library(sao_platform INTERFACE)
add_library(sao::platform ALIAS sao_platform)
target_link_libraries(sao_platform INTERFACE
    sao::core
    sao::net
    sao::engine
    sao::rt_io
    sao::ui
    sao::sdk
    sao::scripting
    sao::gpu_hunt)
target_compile_features(sao_platform INTERFACE cxx_std_20)
