# plugins/emma_host — Emma 自研零依赖解释器
if (NOT SAO_PLUGINS_ENABLE_EMMA)
    return()
endif()

add_library(sao_plugins_emma_host STATIC
    src/emma_lexer.cpp
    src/emma_parser.cpp
    src/emma_interpreter.cpp
    src/emma_stdlib.cpp
    src/emma_source_io.cpp
    src/emma_call.cpp
    src/emma_loader_adapter.cpp
    src/emma_error.cpp
)
add_library(sao::plugins::emma_host ALIAS sao_plugins_emma_host)

target_include_directories(sao_plugins_emma_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../script_ctx/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

target_compile_features(sao_plugins_emma_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_emma_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_emma_host PUBLIC
    sao_plugins_loader
    sao_plugins_script_ctx
    sao_plugins_sdk_binding
)

# sao::sdk 走 PRIVATE, 避免它的 PUBLIC cxx_std_20 泄漏给 emma_host 的下游
# 消费者 (同 angel_host 约定)。反射引擎面在 install_context 里用
# sao_sdk_context_create + bind_platform_services 持有宿主自有
# SaoSdkContext, 再经 sao_plugins_sdk_context_dispatch(method_engine_call)
# 走 binding_engine.h 契约。
target_link_libraries(sao_plugins_emma_host PRIVATE
    sao::sdk
)

