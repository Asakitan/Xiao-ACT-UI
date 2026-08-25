# platform/engine — game-agnostic runtime primitives.
#
# Ships only five abstractions:
#   * event_bus     — sync publish/subscribe with ring buffer
#   * state         — typed key/value store with source metadata
#   * ui_spec       — normalize/validate a plugin's declarative UI spec
#   * render_hook   — priority-ordered hook chain + per-plugin overlays
#   * runtime       — aggregate context owning the above
#
# Game-specific concepts (DPS, boss state, encounters, mechanic
# triggers) live inside plugins; this engine layer knows nothing about
# them.
#
# SHARED because the ABI is what plugins bind against; a SHARED build
# forces us to keep the boundary clean (import-lib for plugins, no
# accidental static duplication).

# Shipping EventBus path: src/event_bus_adapter.cpp.  The historical
# src/event_bus.cpp file is intentionally excluded; its dead return count is
# not runtime status.
add_library(sao_platform_engine SHARED
    src/abi.cpp
    src/event_bus_adapter.cpp
    src/state.cpp
    src/ui_spec.cpp
    src/render_hook.cpp
    src/runtime.cpp
    # Generic automation SDK primitives.  All three
    # sources are game-agnostic: state names, history entry shapes and
    # trigger condition parameters are supplied by the caller (or the
    # plugin that owns the trigger) as opaque data.
    src/state_machine.cpp
    src/history_ring.cpp
    src/trigger_engine.cpp
)
add_library(sao::engine ALIAS sao_platform_engine)

target_compile_definitions(sao_platform_engine PRIVATE SAO_ENGINE_BUILDING_DLL)

target_include_directories(sao_platform_engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(sao_platform_engine PUBLIC cxx_std_20)

# Automation primitives parse condition_params / state-machine config JSON
# via nlohmann::json.  Header-only so it stays a PRIVATE dep.
find_package(nlohmann_json 3 CONFIG REQUIRED)
target_link_libraries(sao_platform_engine
    PUBLIC sao::core
    PRIVATE nlohmann_json::nlohmann_json
)

