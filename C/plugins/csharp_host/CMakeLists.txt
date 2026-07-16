# plugins/csharp_host — .NET hostfxr + Roslyn
if (NOT SAO_PLUGINS_ENABLE_CSHARP)
    return()
endif()

add_library(sao_plugins_csharp_host STATIC
    src/cs_host.cpp
    src/cs_compile.cpp
    src/cs_call.cpp
    src/cs_module_bridge.cpp
    src/cs_sandbox.cpp
    src/cs_error.cpp
    # Wave 8 / Agent d Phase 8: hello_csharp plugin lifecycle
    src/cs_plugin_lifecycle.cpp
)
add_library(sao::plugins::csharp_host ALIAS sao_plugins_csharp_host)

target_include_directories(sao_plugins_csharp_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

target_compile_features(sao_plugins_csharp_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_csharp_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_csharp_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
)

# hostfxr 走随包 vendor (nethost.lib), 不进 vcpkg 图。
# 见 vendor_note.md。

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
