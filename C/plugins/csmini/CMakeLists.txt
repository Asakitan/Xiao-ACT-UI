# plugins/csmini — 纯 C++ C# 子集解释器 + engine_kind::csharp composite
# adapter. 不嵌入 CoreCLR：合法 plugins 的 C# 语法子集由本机
# lexer/parser/interp 直接执行，ctx.* 绑到 loader 的 C ABI。
#
# 路由（adapter 级）：manifest.cs_runtime 或源码子集预检命中 coreclr-only
# 特性 / .dll entry → coreclr 委托（SAO_PLUGINS_ENABLE_CSHARP +
# sao_plugins_csharp_host 可用时），否则 SAO_PLUGINS_ERR_UNSUPPORTED
# 携带特性名。
add_library(sao_plugins_csmini STATIC
    src/csmini_lexer.cpp
    src/csmini_parser.cpp
    src/csmini_value.cpp
    src/csmini_interp.cpp
    src/csmini_ops.cpp
    src/csmini_calls.cpp
    src/csmini_builtins.cpp
    src/csmini_stdlib.cpp
    src/csmini_stdlib2.cpp
    src/csmini_ctx.cpp
    src/csmini_host.cpp
    src/csmini_bridge.cpp
    src/csmini_loader_adapter.cpp
)
add_library(sao::plugins::csmini ALIAS sao_plugins_csmini)

target_include_directories(sao_plugins_csmini PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../script_ctx/include>
)

target_compile_features(sao_plugins_csmini PUBLIC cxx_std_20)
target_compile_definitions(sao_plugins_csmini PRIVATE SAO_PLUGINS_BUILDING_DLL)
if (MSVC)
    target_compile_definitions(sao_plugins_csmini PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    # Uniform stub entry-point signatures keep an unused interpreter param →
    # silence C4100 only; other warnings stay live.
    target_compile_options(sao_plugins_csmini PRIVATE /EHsc /utf-8 /wd4100)
endif()

target_link_libraries(sao_plugins_csmini PUBLIC
    sao_plugins_loader
    sao_plugins_script_ctx
    sao_plugins_compat
    nlohmann_json::nlohmann_json
)

# Reflective engine surface (binding_engine.h): csmini_ctx.cpp dispatches
# through sao_plugins_sdk_context_dispatch and lazily binds an owned
# SaoSdkContext via sao_sdk_bind_context + bind_platform_services.
target_link_libraries(sao_plugins_csmini PRIVATE
    sao_plugins_sdk_binding
    sao::sdk
)
if (WIN32)
    target_link_libraries(sao_plugins_csmini PRIVATE bcrypt)
endif()
target_include_directories(sao_plugins_csmini PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

# coreclr delegate: 当 csharp_host 目标存在时链接并把
# sao_plugins_cshost_* 暴露为委托通道（内部 component/sdk_bridge 链）。
if (SAO_PLUGINS_ENABLE_CSHARP AND TARGET sao_plugins_csharp_host)
    target_link_libraries(sao_plugins_csmini PUBLIC sao_plugins_csharp_host)
    target_include_directories(sao_plugins_csmini PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../csharp_host/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../csharp_host/include)
    target_compile_definitions(sao_plugins_csmini PRIVATE
        SAO_PLUGINS_ENABLE_CORECLR)
endif()
