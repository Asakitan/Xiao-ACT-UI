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

if (NOT TARGET sao_plugins_loader)
    message(FATAL_ERROR
        "sao_plugins_python_host requires the canonical loader target")
endif()
target_compile_definitions(sao_plugins_python_host PUBLIC
    SAO_PYHOST_HAS_CONTEXT_ENTITY_PROVIDER
)

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

# 嵌入式 CPython 3.11+ (Development.Embed).
# QUIET 让本地无 Python dev headers 时 CMake 不 stop, 而是走 "无 embed" 分支
# (py_host.cpp 里 SAO_HAS_PYTHON_EMBED 未定义 → init 返 NOT_IMPLEMENTED).
#
option(SAO_PLUGINS_PYTHON_AUTO_FETCH
    "Legacy compatibility switch; runtime downloads are disabled." OFF)
if (SAO_PLUGINS_PYTHON_AUTO_FETCH)
    message(WARNING
        "SAO_PLUGINS_PYTHON_AUTO_FETCH is stale and is being forced OFF; "
        "runtime acquisition remains offline.")
    set(SAO_PLUGINS_PYTHON_AUTO_FETCH OFF CACHE BOOL
        "Legacy compatibility switch; runtime downloads are disabled." FORCE)
endif()

find_package(Python3 3.11 COMPONENTS Development.Embed QUIET)
set(_sao_python_resolved OFF)
set(_sao_python_runtime_dll "")
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
    set(_sao_python_runtime_dll
        "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.dll")
    set(_sao_python_resolved ON)
endif()

set_property(TARGET sao_plugins_python_host PROPERTY
    SAO_PYHOST_PYTHON_RESOLVED "${_sao_python_resolved}")

if (_sao_python_resolved AND MSVC)
    target_compile_definitions(sao_plugins_python_host PRIVATE
        SAO_PYHOST_DYNAMIC_PY_DATA)
    set_property(TARGET sao_plugins_python_host PROPERTY
        SAO_PYHOST_RUNTIME_DLL "${_sao_python_runtime_dll}")
endif()

if (NOT _sao_python_resolved)
    message(STATUS
        "sao_plugins_python_host: Python3 embed not found; "
        "py_host will report NOT_IMPLEMENTED at runtime")
endif()

