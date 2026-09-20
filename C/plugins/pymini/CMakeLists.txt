# plugins/pymini — 纯 C++ Python 子集解释器 + engine_kind::python composite
# adapter. 不嵌入 CPython：合法 plugins 的 Python 语法子集由本机
# lexer/parser/interp 直接执行，ctx.* 绑到 loader 的 C ABI。
#
# 路由（adapter 级）：manifest.py_runtime 或源码子集预检命中 cpython-only
# 特性 → pyhost 委托（SAO_PLUGINS_ENABLE_PYTHON + python_home 可用时），
# 否则 SAO_PLUGINS_ERR_UNSUPPORTED 携带特性名。
add_library(sao_plugins_pymini STATIC
    src/pymini_lexer.cpp
    src/pymini_parser.cpp
    src/pymini_value.cpp
    src/pymini_interp.cpp
    src/pymini_ops.cpp
    src/pymini_calls.cpp
    src/pymini_builtins.cpp
    src/pymini_imports.cpp
    src/pymini_stdlib.cpp
    src/pymini_stdlib2.cpp
    src/pymini_ctx.cpp
    src/pymini_host.cpp
    src/pymini_bridge.cpp
    src/pymini_loader_adapter.cpp
)
add_library(sao::plugins::pymini ALIAS sao_plugins_pymini)

target_include_directories(sao_plugins_pymini PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../script_ctx/include>
)

target_compile_features(sao_plugins_pymini PUBLIC cxx_std_20)
target_compile_definitions(sao_plugins_pymini PRIVATE SAO_PLUGINS_BUILDING_DLL)
if (MSVC)
    target_compile_definitions(sao_plugins_pymini PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    # Stdlib/builtin entry points keep a uniform (interpreter&, args) signature;
    # unused params on stub functions are intentional → silence C4100 only.
    target_compile_options(sao_plugins_pymini PRIVATE /wd4100)
endif()

target_link_libraries(sao_plugins_pymini PUBLIC
    sao_plugins_loader
    sao_plugins_script_ctx
    sao_plugins_compat
    nlohmann_json::nlohmann_json
    bcrypt
)

# 反射引擎面（pymini_ctx.cpp 的 ctx.engine.* / ctx.engine_call）：
# sao::sdk 提供 SaoSdkContext + sao_sdk_context_create /
# bind_platform_services / destroy；sdk_binding 提供
# sao_plugins_sdk_context_dispatch 与 sdk_engine_catalog_* 目录表。
# 二者均已随 sao_plugins_compat 的 PUBLIC 链传递进来，此处显式 PRIVATE
# 声明直接依赖与头文件可见性。
target_link_libraries(sao_plugins_pymini PRIVATE
    sao::sdk
    sao_plugins_sdk_binding
)
target_include_directories(sao_plugins_pymini PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

# cpython delegate: 当 python_host 目标存在时链接并把
# sao_plugins_pyhost_* 暴露为委托通道（gil_scope_enter/leave + load/call）。
if (SAO_PLUGINS_ENABLE_PYTHON AND TARGET sao_plugins_python_host)
    target_link_libraries(sao_plugins_pymini PUBLIC sao_plugins_python_host)
    target_include_directories(sao_plugins_pymini PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../python_host/include)
    target_compile_definitions(sao_plugins_pymini PRIVATE
        SAO_PLUGINS_ENABLE_PYTHON)
endif()
