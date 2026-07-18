# plugins/angel_host — 真 AngelScript SDK 嵌入
if (NOT SAO_PLUGINS_ENABLE_ANGEL)
    return()
endif()

add_library(sao_plugins_angel_host STATIC
    src/as_host.cpp
    src/as_module_bridge.cpp
    src/as_call.cpp
    src/as_generic_bindings.cpp
    src/as_loader_adapter.cpp
    src/as_sandbox.cpp
    src/as_stdlib.cpp
    src/as_error.cpp
    # Wave 8 / Agent d Phase 8: plugin lifecycle (真加载真跑)
    src/as_plugin_lifecycle.cpp
    # Phase 4: gpu_hunt SDK binding (Vec3 / Mat4 / GpuTracker → AS)
    src/as_gpu_hunt_bind.cpp
)
add_library(sao::plugins::angel_host ALIAS sao_plugins_angel_host)

target_include_directories(sao_plugins_angel_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

# Wave 18 / Agent b — SAO_ENC_STR (header-only, 无需链接)
target_include_directories(sao_plugins_angel_host PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>
)

target_compile_features(sao_plugins_angel_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_angel_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
    # Phase 4: gpu_hunt static lib supplies Vec3 / Mat4 / GpuTracker for
    # the standalone binding path.
    sao::gpu_hunt
)

# sao::sdk is linked PRIVATE so its PUBLIC cxx_std_20 requirement does
# not leak into downstream consumers of sao_plugins_angel_host.  The
# gpu-hunt binding source is the only consumer inside this target and
# only needs the header path (SaoSdkContext + SaoSdkGpuHuntTable) at
# compile time.
target_link_libraries(sao_plugins_angel_host PRIVATE
    sao::sdk
)

option(SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME
    "AngelScript SDK 缺失时终止配置，而不是构建 fail-closed stub" OFF)

# AngelScript SDK 只使用已安装 package；缺失时不联网下载。
find_package(unofficial-angelscript CONFIG QUIET)
if (unofficial-angelscript_FOUND)
    target_link_libraries(sao_plugins_angel_host PRIVATE
        unofficial::angelscript::angelscript
    )
    target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_HAS_ANGELSCRIPT=1)
    message(STATUS "plugins/angel_host: 用 vcpkg unofficial-angelscript (SAO_HAS_ANGELSCRIPT=1)")
elseif (SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME)
    message(FATAL_ERROR
        "plugins/angel_host: SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME=ON, "
        "但未找到 unofficial-angelscript package")
else()
    message(STATUS
        "plugins/angel_host: AngelScript package 未安装；构建 fail-closed stub")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
