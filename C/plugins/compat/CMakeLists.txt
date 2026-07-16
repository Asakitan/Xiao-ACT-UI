# plugins/compat — 老 Python 平台插件向下兼容层
add_library(sao_plugins_compat STATIC
    src/py_v1_manifest.cpp
    src/py_v1_ctx_shim.cpp
    src/libs_vendor_bridge.cpp
    src/migration.cpp
)
add_library(sao::plugins::compat ALIAS sao_plugins_compat)

target_include_directories(sao_plugins_compat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
)

target_compile_features(sao_plugins_compat PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_compat PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_compat PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
    PRIVATE sao::sdk)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
