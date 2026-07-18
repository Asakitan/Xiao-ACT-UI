# plugins/python_host — 嵌入式 CPython 3.11+
if (NOT SAO_PLUGINS_ENABLE_PYTHON)
    return()
endif()

add_library(sao_plugins_python_host STATIC
    src/py_host.cpp
    src/py_loader_adapter.cpp
    src/py_module_bridge.cpp
    src/py_call.cpp
    src/py_sandbox.cpp
    src/py_error.cpp
)
add_library(sao::plugins::python_host ALIAS sao_plugins_python_host)

target_include_directories(sao_plugins_python_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../compat/include>
)

target_compile_features(sao_plugins_python_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_python_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_python_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
    sao_plugins_compat
    sao::sdk
)

# Wave 2: 嵌入式 CPython 3.11+ (Development.Embed).
# QUIET 让本地无 Python dev headers 时 CMake 不 stop, 而是走 "无 embed" 分支
# (py_host.cpp 里 SAO_HAS_PYTHON_EMBED 未定义 → init 返 NOT_IMPLEMENTED).
find_package(Python3 3.11 COMPONENTS Development.Embed QUIET)
if (Python3_FOUND)
    message(STATUS "sao_plugins_python_host: Python3 embed found (${Python3_VERSION})")
    target_compile_definitions(sao_plugins_python_host PUBLIC
        SAO_HAS_PYTHON_EMBED
        # CPython's Windows headers automatically select python311_d.lib when
        # _DEBUG is set. An official release distribution does not ship that
        # debug import library, so suppress only the automatic link pragma.
        # Python3::Python explicitly supplies python311.lib.
        $<$<CONFIG:Debug>:Py_NO_LINK_LIB>
    )
    target_link_libraries(sao_plugins_python_host PUBLIC Python3::Python)
else()
    message(STATUS
        "sao_plugins_python_host: Python3 embed not found; "
        "py_host will report NOT_IMPLEMENTED at runtime")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
