# plugins/sdk_binding — SDK C ABI ↔ 5 种脚本引擎的桥
add_library(sao_plugins_sdk_binding STATIC
    src/binding_common.cpp
    src/binding_context_dispatch.cpp
    src/binding_python.cpp
    src/binding_emma.cpp
    src/binding_angel.cpp
    src/binding_lua.cpp
    src/binding_csharp.cpp
)
add_library(sao::plugins::sdk_binding ALIAS sao_plugins_sdk_binding)

target_include_directories(sao_plugins_sdk_binding PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
)

target_compile_features(sao_plugins_sdk_binding PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_sdk_binding PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_sdk_binding
    PUBLIC sao_plugins_loader
    PRIVATE sao::sdk)

