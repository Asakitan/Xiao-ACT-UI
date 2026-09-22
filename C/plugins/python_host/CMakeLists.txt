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
    src/py_import_stubs.cpp
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

# ctx_surface advisory wiring — script_ctx is a sibling in the unified
# plugins tree but absent from standalone loader+host builds.
if (TARGET sao_plugins_script_ctx)
    target_link_libraries(sao_plugins_python_host PUBLIC sao_plugins_script_ctx)
    target_compile_definitions(sao_plugins_python_host PRIVATE SAO_PYHOST_HAS_CTX_SURFACE)
endif()

# 嵌入式 CPython 3.11+ (Development.Embed).
# QUIET 让本地无 Python dev headers 时 CMake 不 stop, 而是走 "无 embed" 分支
# (py_host.cpp 里 SAO_HAS_PYTHON_EMBED 未定义 → init 返 NOT_IMPLEMENTED).
#
option(SAO_PLUGINS_PYTHON_AUTO_FETCH
    "Legacy compatibility switch; runtime downloads are disabled." OFF)

# Embed discovery switch: ON lets the host compile the real CPython path
# when a Python3 Development.Embed install is found (SAO_PYHOST_HOME /
# Python3_ROOT / E:/Py / well-known roots below); OFF — or no headers
# found — keeps py_host.cpp on the explicit SAO_ERR_NOT_IMPLEMENTED
# capability return without importing python3xx.dll anywhere.
option(SAO_PLUGINS_PYTHON_EMBED
    "Embed CPython inside sao_plugins_python_host when Python3 Development.Embed is found" ON)
if (SAO_PLUGINS_PYTHON_AUTO_FETCH)
    message(WARNING
        "SAO_PLUGINS_PYTHON_AUTO_FETCH is stale and is being forced OFF; "
        "runtime acquisition remains offline.")
    set(SAO_PLUGINS_PYTHON_AUTO_FETCH OFF CACHE BOOL
        "Legacy compatibility switch; runtime downloads are disabled." FORCE)
endif()

# Locate a usable CPython 3.11+ Development.Embed root.  The host always
# builds (fail-closed when none is found); these hints let the C++ tree
# resolve a common local runtime without requiring PATH/registry entries.
#   Priority: SAO_PYHOST_HOME > Python3_ROOT > well-known dev roots >
#   per-user %LOCALAPPDATA% installs > registry/PATH (FindPython3 default).
set(SAO_PYHOST_HOME "" CACHE PATH
    "CPython install root to embed (contains include/Python.h and libs/)")
if (NOT Python3_ROOT AND NOT Python3_ROOT_DIR)
    foreach(_sao_py_candidate IN ITEMS
            "${SAO_PYHOST_HOME}"
            "E:/Py"
            "C:/Python313" "C:/Python312" "C:/Python311"
            "$ENV{LOCALAPPDATA}/Programs/Python/Python313"
            "$ENV{LOCALAPPDATA}/Programs/Python/Python312"
            "$ENV{LOCALAPPDATA}/Programs/Python/Python311")
        if (_sao_py_candidate AND EXISTS "${_sao_py_candidate}/include/Python.h")
            set(Python3_ROOT "${_sao_py_candidate}")
            set(Python3_ROOT_DIR "${_sao_py_candidate}")
            break()
        endif()
    endforeach()
endif()

if (SAO_PLUGINS_PYTHON_EMBED)
find_package(Python3 3.11 COMPONENTS Development.Embed QUIET)
endif()
set(_sao_python_resolved OFF)
set(_sao_python_runtime_dll "")
if (Python3_FOUND)
    message(STATUS "sao_plugins_python_host: Python3 embed found (${Python3_VERSION})")
    target_compile_definitions(sao_plugins_python_host PUBLIC
        SAO_HAS_PYTHON_EMBED
        # No python3xx.lib import library is linked anywhere: the launcher
        # payload must start on machines without CPython installed. The
        # runtime DLL is discovered inside python_home and bound lazily
        # through the __imp_* slots in py_import_stubs.cpp — the pragma
        # import hints in pyconfig.h are suppressed for every config.
        Py_NO_LINK_LIB
    )
    # Keep Python.h reachable without inheriting the python311.lib link edge.
    target_include_directories(sao_plugins_python_host PRIVATE
        $<TARGET_PROPERTY:Python3::Python,INTERFACE_INCLUDE_DIRECTORIES>)
    if (MSVC)
        # pyconfig.h on this header set has no Py_NO_LINK_LIB guard — its
        # pragma always emits /DEFAULTLIB for python3xx(_d).lib.  Ignore the
        # directive in every consumer; binding is entirely dynamic
        # (__imp_* slots filled by resolve_python_imports at runtime).
        set(_sao_pyhost_dead_libs
            "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.lib")
        set(_sao_pyhost_dead_libs_debug
            "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}_d.lib")
        set(_sao_pyhost_dead_libs_limited "python3.lib")
        target_link_options(sao_plugins_python_host PUBLIC
            /NODEFAULTLIB:${_sao_pyhost_dead_libs}
            /NODEFAULTLIB:${_sao_pyhost_dead_libs_debug}
            /NODEFAULTLIB:${_sao_pyhost_dead_libs_limited})
    else()
        target_link_libraries(sao_plugins_python_host PRIVATE Python3::Python)
    endif()
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

