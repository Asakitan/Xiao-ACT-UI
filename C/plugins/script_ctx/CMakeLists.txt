# plugins/script_ctx — shared script-side ctx helpers (ui.* builders,
# load_local runtime bridge, ctx surface capability table).
#
# Consumed by every script host (python_host/pymini, emma_host, lua_host,
# angel_host, csharp_host).  Static lib, C++20, in-tree only: hosts marshal
# their value domain <-> nlohmann::json and call the C++ API directly.
add_library(sao_plugins_script_ctx STATIC
    src/script_ui.cpp
    src/runtime_bridge.cpp
    src/ctx_surface.cpp
)
add_library(sao::plugins::script_ctx ALIAS sao_plugins_script_ctx)

target_include_directories(sao_plugins_script_ctx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
)

target_compile_features(sao_plugins_script_ctx PUBLIC cxx_std_20)
target_compile_definitions(sao_plugins_script_ctx PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_script_ctx PUBLIC
    sao_plugins_loader
)
