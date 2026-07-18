# plugins/emma_host — Emma 自研零依赖解释器
if (NOT SAO_PLUGINS_ENABLE_EMMA)
    return()
endif()

add_library(sao_plugins_emma_host STATIC
    src/emma_lexer.cpp
    src/emma_parser.cpp
    src/emma_interpreter.cpp
    src/emma_stdlib.cpp
    src/emma_call.cpp
    src/emma_loader_adapter.cpp
    src/emma_error.cpp
)
add_library(sao::plugins::emma_host ALIAS sao_plugins_emma_host)

target_include_directories(sao_plugins_emma_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

target_compile_features(sao_plugins_emma_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_emma_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_emma_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
