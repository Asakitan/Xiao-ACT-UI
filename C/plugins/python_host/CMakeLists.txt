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
# CPython resolution order:
#   1. find_package(Python3 3.11 COMPONENTS Development.Embed) (vcpkg / system)
#   2. FetchContent embeddable + headers archives when
#      SAO_PLUGINS_PYTHON_AUTO_FETCH=ON
# Auto-fetch is opt-in so shipping / air-gapped builds stay reproducible.
option(SAO_PLUGINS_PYTHON_AUTO_FETCH
    "Download Python 3.11.8 embeddable + headers archives on demand (FetchContent) when no runtime is found." OFF)
set(SAO_PYTHON_EMBED_FETCH_URL
    "https://www.python.org/ftp/python/3.11.8/python-3.11.8-embed-amd64.zip"
    CACHE STRING "URL of the Windows embeddable Python zip (python311.dll + python311.zip stdlib)")
set(SAO_PYTHON_EMBED_FETCH_SHA256
    ""
    CACHE STRING "SHA256 of the embeddable Python zip (empty = skip integrity check)")
set(SAO_PYTHON_HEADERS_FETCH_URL
    "https://www.python.org/ftp/python/3.11.8/python-3.11.8-amd64.zip"
    CACHE STRING "URL of the Python amd64 source archive that publishes Include/ + python311.lib for embed linking")
set(SAO_PYTHON_HEADERS_FETCH_SHA256
    ""
    CACHE STRING "SHA256 of the Python amd64 source archive (empty = skip integrity check)")

find_package(Python3 3.11 COMPONENTS Development.Embed QUIET)
set(_sao_python_resolved OFF)
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
    install(IMPORTED_RUNTIME_ARTIFACTS Python3::Python
        RUNTIME DESTINATION bin
        COMPONENT ship)
    set(_sao_python_resolved ON)
endif()

if (NOT _sao_python_resolved AND SAO_PLUGINS_PYTHON_AUTO_FETCH)
    include(FetchContent)
    # Fetch the Windows embeddable zip (runtime dll + stdlib zip).
    # We stage it into the build tree under python3_embed/ so downstream
    # packaging can copy it verbatim next to the launcher exe.
    set(_sao_python_embed_args
        python3_embed
        URL "${SAO_PYTHON_EMBED_FETCH_URL}")
    if (SAO_PYTHON_EMBED_FETCH_SHA256)
        list(APPEND _sao_python_embed_args
            URL_HASH "SHA256=${SAO_PYTHON_EMBED_FETCH_SHA256}")
    endif()
    FetchContent_Declare(${_sao_python_embed_args})
    FetchContent_MakeAvailable(python3_embed)

    # Fetch the amd64 source archive that publishes Include/ + libs/python311.lib.
    # This URL is a cache-var so operators can point it at a vendored
    # zip (or NuGet-derived package) when python.org's layout diverges.
    set(_sao_python_headers_args
        python3_headers
        URL "${SAO_PYTHON_HEADERS_FETCH_URL}")
    if (SAO_PYTHON_HEADERS_FETCH_SHA256)
        list(APPEND _sao_python_headers_args
            URL_HASH "SHA256=${SAO_PYTHON_HEADERS_FETCH_SHA256}")
    endif()
    FetchContent_Declare(${_sao_python_headers_args})
    FetchContent_MakeAvailable(python3_headers)

    # Locate include dir + import lib inside the extracted headers tree.
    # Standard python.org layout puts headers under Include/ and libs under
    # libs/ (uppercase Include, lowercase libs on the amd64 tree).
    set(_sao_python_include_dir "")
    foreach (_sao_python_inc_candidate
             "${python3_headers_SOURCE_DIR}/Include"
             "${python3_headers_SOURCE_DIR}/include")
        if (EXISTS "${_sao_python_inc_candidate}/Python.h")
            set(_sao_python_include_dir "${_sao_python_inc_candidate}")
            break()
        endif()
    endforeach()

    set(_sao_python_import_lib "")
    foreach (_sao_python_lib_candidate
             "${python3_headers_SOURCE_DIR}/libs/python311.lib"
             "${python3_headers_SOURCE_DIR}/lib/python311.lib"
             "${python3_embed_SOURCE_DIR}/libs/python311.lib"
             "${python3_embed_SOURCE_DIR}/python311.lib")
        if (EXISTS "${_sao_python_lib_candidate}")
            set(_sao_python_import_lib "${_sao_python_lib_candidate}")
            break()
        endif()
    endforeach()

    if (_sao_python_include_dir AND _sao_python_import_lib)
        # Compose an INTERFACE target that publishes include dir + link path.
        # Kept as a plain INTERFACE lib (not an IMPORTED SHARED lib) so we
        # do not have to synthesize a full DLL location on non-Windows hosts.
        add_library(sao_python3_embedded INTERFACE)
        target_include_directories(sao_python3_embedded
            INTERFACE "${_sao_python_include_dir}")
        target_link_libraries(sao_python3_embedded
            INTERFACE "${_sao_python_import_lib}")
        # Same Py_NO_LINK_LIB dance as the vcpkg path: the CPython headers
        # emit an automatic link pragma for python311_d.lib under _DEBUG,
        # which the release distribution does not ship.
        target_compile_definitions(sao_plugins_python_host PUBLIC
            SAO_HAS_PYTHON_EMBED
            $<$<CONFIG:Debug>:Py_NO_LINK_LIB>)
        target_link_libraries(sao_plugins_python_host PUBLIC
            sao_python3_embedded)
        # Publish the staged embed tree so downstream packaging can copy the
        # runtime dll + python311.zip stdlib next to the launcher exe.
        set(SAO_PYTHON_EMBED_STAGED_DIR
            "${python3_embed_SOURCE_DIR}"
            CACHE PATH "Staged Python embeddable runtime tree (dll + stdlib zip)" FORCE)
        message(STATUS
            "sao_plugins_python_host: fetched Python 3.11.8 (embed=${python3_embed_SOURCE_DIR}, headers=${_sao_python_include_dir}, import_lib=${_sao_python_import_lib})")
        set(_sao_python_resolved ON)
    else()
        message(WARNING
            "sao_plugins_python_host: FetchContent succeeded but could not locate "
            "Python.h (${_sao_python_include_dir}) or python311.lib (${_sao_python_import_lib}); "
            "override SAO_PYTHON_HEADERS_FETCH_URL to a vendored archive that ships both.")
    endif()
endif()

if (NOT _sao_python_resolved)
    message(STATUS
        "sao_plugins_python_host: Python3 embed not found; "
        "py_host will report NOT_IMPLEMENTED at runtime")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
