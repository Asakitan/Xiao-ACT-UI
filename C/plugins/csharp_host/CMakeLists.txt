# plugins/csharp_host — .NET hostfxr precompiled component host
if (NOT SAO_PLUGINS_ENABLE_CSHARP)
    return()
endif()

add_library(sao_plugins_csharp_host STATIC
    src/cs_host.cpp
    src/cs_compile.cpp
    src/cs_call.cpp
    src/cs_module_bridge.cpp
    src/cs_sdk_bridge.cpp
    src/cs_sdk_provider.cpp
    src/cs_sandbox.cpp
    src/cs_error.cpp
    src/cs_component.cpp
    src/cs_direct_plugin_lifecycle.cpp
    src/cs_loader_adapter.cpp
)
add_library(sao::plugins::csharp_host ALIAS sao_plugins_csharp_host)

target_include_directories(sao_plugins_csharp_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

target_compile_features(sao_plugins_csharp_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_csharp_host PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)

target_link_libraries(sao_plugins_csharp_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
    sao::sdk
)

# hostfxr 走随包 vendor (nethost.lib), 不进 vcpkg 图。
# 见 vendor_note.md。

# ── .NET 8 runtime resolution ────────────────────────────────────────
#
# Resolution order for the hostfxr / coreclr shared runtime that
# cs_component.cpp loads at runtime:
#   1. Cache-var overrides: SAO_CSHARP_HOSTFXR_DLL, SAO_CSHARP_EMBEDDED_RUNTIME_DIR
#   2. Environment: DOTNET_ROOT (mirrors the .NET host contract)
#   3. Windows registry: HKLM\SOFTWARE\dotnet\Setup\InstalledVersions\x64\InstallLocation
#   4. Well-known install root: C:/Program Files/dotnet
#   5. find_program(dotnet) walked back to its install root
#   6. FetchContent Microsoft.NETCore.App.Runtime.win-x64 nupkg when
#      SAO_PLUGINS_CSHARP_AUTO_FETCH=ON
#
# Detection is best-effort at configure time; the runtime source at
# cs_component.cpp still performs a live LoadLibrary probe, so a false
# negative here only means tests run without the extra PATH/env plumbing.
# When any resolution step succeeds we populate:
#   SAO_CSHARP_RUNTIME_STATE   — one of "detected", "fetched", "missing"
#   SAO_CSHARP_HOSTFXR_DIR     — directory containing hostfxr.dll
#   SAO_CSHARP_SHARED_ROOT_DIR — directory containing Microsoft.NETCore.App
#   SAO_CSHARP_DOTNET_ROOT     — value to publish as DOTNET_ROOT for tests
option(SAO_PLUGINS_CSHARP_AUTO_FETCH
    "Download Microsoft.NETCore.App.Runtime.win-x64 8.0.x nupkg on demand (FetchContent) when no .NET runtime is found." OFF)
set(SAO_CSHARP_HOSTFXR_DLL "" CACHE FILEPATH
    "Absolute path to a preinstalled hostfxr.dll (skips runtime detection)")
set(SAO_CSHARP_EMBEDDED_RUNTIME_DIR "" CACHE PATH
    "Directory containing a preinstalled .NET runtime layout (host/fxr/... + shared/Microsoft.NETCore.App/...)")
set(SAO_CSHARP_DOTNET_FETCH_VERSION "8.0.11" CACHE STRING
    "Microsoft.NETCore.App.Runtime.win-x64 version fetched when AUTO_FETCH=ON")
set(SAO_CSHARP_DOTNET_FETCH_URL
    "https://www.nuget.org/api/v2/package/Microsoft.NETCore.App.Runtime.win-x64/${SAO_CSHARP_DOTNET_FETCH_VERSION}"
    CACHE STRING
    "URL from which the .NET runtime nupkg is fetched when AUTO_FETCH=ON")
set(SAO_CSHARP_DOTNET_FETCH_SHA256 "" CACHE STRING
    "SHA256 of the fetched .NET runtime nupkg (empty = skip integrity check)")

set(SAO_CSHARP_RUNTIME_STATE "missing" CACHE INTERNAL "" FORCE)
set(SAO_CSHARP_HOSTFXR_DIR   ""        CACHE INTERNAL "" FORCE)
set(SAO_CSHARP_SHARED_ROOT_DIR ""      CACHE INTERNAL "" FORCE)
set(SAO_CSHARP_DOTNET_ROOT   ""        CACHE INTERNAL "" FORCE)

# _sao_cshost_resolve_root(<candidate-root>)
#
# Walks a candidate .NET install root and, on success, populates the
# SAO_CSHARP_* cache variables above.  A valid root has:
#   <root>/host/fxr/<version>/hostfxr.dll
#   <root>/shared/Microsoft.NETCore.App/<version>/
# where <version> is any 8.x directory (we only require the major to be 8 so
# a hotfix on 8.0.z still qualifies).
function(_sao_cshost_resolve_root candidate_root)
    if (NOT candidate_root OR NOT IS_DIRECTORY "${candidate_root}")
        return()
    endif()
    set(_fxr_root "${candidate_root}/host/fxr")
    set(_shared_root "${candidate_root}/shared/Microsoft.NETCore.App")
    if (NOT IS_DIRECTORY "${_fxr_root}" OR NOT IS_DIRECTORY "${_shared_root}")
        return()
    endif()
    file(GLOB _fxr_versions RELATIVE "${_fxr_root}" "${_fxr_root}/8.*")
    if (NOT _fxr_versions)
        return()
    endif()
    list(SORT _fxr_versions)
    list(REVERSE _fxr_versions)
    foreach(_ver IN LISTS _fxr_versions)
        set(_dll "${_fxr_root}/${_ver}/hostfxr.dll")
        if (EXISTS "${_dll}")
            set(SAO_CSHARP_HOSTFXR_DIR "${_fxr_root}/${_ver}" CACHE INTERNAL "" FORCE)
            set(SAO_CSHARP_DOTNET_ROOT "${candidate_root}"   CACHE INTERNAL "" FORCE)
            file(GLOB _shared_versions RELATIVE "${_shared_root}" "${_shared_root}/8.*")
            if (_shared_versions)
                list(SORT _shared_versions)
                list(REVERSE _shared_versions)
                list(GET _shared_versions 0 _shared_ver)
                set(SAO_CSHARP_SHARED_ROOT_DIR
                    "${_shared_root}/${_shared_ver}" CACHE INTERNAL "" FORCE)
            endif()
            return()
        endif()
    endforeach()
endfunction()

# 1. Explicit hostfxr override.
if (SAO_CSHARP_HOSTFXR_DLL AND EXISTS "${SAO_CSHARP_HOSTFXR_DLL}")
    get_filename_component(SAO_CSHARP_HOSTFXR_DIR
        "${SAO_CSHARP_HOSTFXR_DLL}" DIRECTORY)
    set(SAO_CSHARP_HOSTFXR_DIR "${SAO_CSHARP_HOSTFXR_DIR}" CACHE INTERNAL "" FORCE)
    set(SAO_CSHARP_RUNTIME_STATE "detected" CACHE INTERNAL "" FORCE)
    message(STATUS
        "plugins/csharp_host: using SAO_CSHARP_HOSTFXR_DLL=${SAO_CSHARP_HOSTFXR_DLL}")
endif()

# 2. Embedded runtime directory override.
if (SAO_CSHARP_RUNTIME_STATE STREQUAL "missing" AND SAO_CSHARP_EMBEDDED_RUNTIME_DIR)
    _sao_cshost_resolve_root("${SAO_CSHARP_EMBEDDED_RUNTIME_DIR}")
    if (SAO_CSHARP_HOSTFXR_DIR)
        set(SAO_CSHARP_RUNTIME_STATE "detected" CACHE INTERNAL "" FORCE)
        message(STATUS
            "plugins/csharp_host: using embedded runtime at ${SAO_CSHARP_EMBEDDED_RUNTIME_DIR}")
    endif()
endif()

# 3. DOTNET_ROOT environment variable.
if (SAO_CSHARP_RUNTIME_STATE STREQUAL "missing")
    if (DEFINED ENV{DOTNET_ROOT})
        _sao_cshost_resolve_root("$ENV{DOTNET_ROOT}")
        if (SAO_CSHARP_HOSTFXR_DIR)
            set(SAO_CSHARP_RUNTIME_STATE "detected" CACHE INTERNAL "" FORCE)
            message(STATUS
                "plugins/csharp_host: DOTNET_ROOT env resolved to $ENV{DOTNET_ROOT}")
        endif()
    endif()
endif()

# 4. Windows registry lookup.
if (WIN32 AND SAO_CSHARP_RUNTIME_STATE STREQUAL "missing")
    set(_reg_key
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x64;InstallLocation]")
    get_filename_component(_reg_root "${_reg_key}" ABSOLUTE)
    if (_reg_root AND NOT _reg_root STREQUAL "/registry")
        _sao_cshost_resolve_root("${_reg_root}")
        if (SAO_CSHARP_HOSTFXR_DIR)
            set(SAO_CSHARP_RUNTIME_STATE "detected" CACHE INTERNAL "" FORCE)
            message(STATUS
                "plugins/csharp_host: registry resolved DOTNET_ROOT to ${_reg_root}")
        endif()
    endif()
endif()

# 5. Well-known install root.
if (WIN32 AND SAO_CSHARP_RUNTIME_STATE STREQUAL "missing")
    _sao_cshost_resolve_root("C:/Program Files/dotnet")
    if (SAO_CSHARP_HOSTFXR_DIR)
        set(SAO_CSHARP_RUNTIME_STATE "detected" CACHE INTERNAL "" FORCE)
        message(STATUS
            "plugins/csharp_host: default DOTNET_ROOT C:/Program Files/dotnet resolved")
    endif()
endif()

# 6. dotnet CLI walkback.
if (SAO_CSHARP_RUNTIME_STATE STREQUAL "missing")
    find_program(SAO_CSHARP_DOTNET_CLI NAMES dotnet)
    if (SAO_CSHARP_DOTNET_CLI)
        get_filename_component(_dotnet_root
            "${SAO_CSHARP_DOTNET_CLI}" DIRECTORY)
        _sao_cshost_resolve_root("${_dotnet_root}")
        if (SAO_CSHARP_HOSTFXR_DIR)
            set(SAO_CSHARP_RUNTIME_STATE "detected" CACHE INTERNAL "" FORCE)
            message(STATUS
                "plugins/csharp_host: dotnet CLI walked back to ${_dotnet_root}")
        endif()
    endif()
endif()

# 7. FetchContent Microsoft.NETCore.App.Runtime.win-x64 nupkg.
#
# The nupkg is a zip archive; FetchContent unpacks it into
# <cache>/dotnet_runtime_source-src and the payload we care about lives at
# runtimes/win-x64/native/hostfxr.dll and shared/Microsoft.NETCore.App/<ver>.
# We reshuffle the pieces into a real DOTNET_ROOT layout so cs_component.cpp
# can consume it without knowing about the nupkg internals.
if (SAO_CSHARP_RUNTIME_STATE STREQUAL "missing" AND SAO_PLUGINS_CSHARP_AUTO_FETCH)
    if (NOT WIN32)
        message(STATUS
            "plugins/csharp_host: AUTO_FETCH requested but current platform is not win-x64; skipping")
    else()
        include(FetchContent)
        set(_sao_cshost_fetch_args
            dotnet_runtime_source
            URL "${SAO_CSHARP_DOTNET_FETCH_URL}")
        if (SAO_CSHARP_DOTNET_FETCH_SHA256)
            list(APPEND _sao_cshost_fetch_args
                URL_HASH "SHA256=${SAO_CSHARP_DOTNET_FETCH_SHA256}")
        endif()
        # nupkg content-type is application/octet-stream; force the extractor
        # to treat it as a zip regardless of the .nupkg extension the server
        # advertises.
        list(APPEND _sao_cshost_fetch_args
            DOWNLOAD_NAME "microsoft.netcore.app.runtime.win-x64.zip")
        FetchContent_Declare(${_sao_cshost_fetch_args})
        FetchContent_MakeAvailable(dotnet_runtime_source)

        set(_pkg_root "${dotnet_runtime_source_SOURCE_DIR}")
        set(_native_root "${_pkg_root}/runtimes/win-x64/native")
        # NuGet nupkgs enumerate their shared/Microsoft.NETCore.App payload
        # under runtimes/win-x64/lib/net8.0.  Grab the first 8.x version we
        # find and re-lay it into a DOTNET_ROOT clone under the cache dir.
        set(_pkg_shared_root "${_pkg_root}/runtimes/win-x64/lib")
        set(_dotnet_root
            "${CMAKE_BINARY_DIR}/plugin_runtime_cache/dotnet_runtime/${SAO_CSHARP_DOTNET_FETCH_VERSION}")

        if (NOT EXISTS "${_native_root}/hostfxr.dll")
            message(WARNING
                "plugins/csharp_host: fetched nupkg missing runtimes/win-x64/native/hostfxr.dll")
        else()
            set(_fxr_dst "${_dotnet_root}/host/fxr/${SAO_CSHARP_DOTNET_FETCH_VERSION}")
            file(MAKE_DIRECTORY "${_fxr_dst}")
            file(COPY "${_native_root}/hostfxr.dll" DESTINATION "${_fxr_dst}")
            foreach(_extra hostpolicy.dll coreclr.dll nethost.dll)
                if (EXISTS "${_native_root}/${_extra}")
                    file(COPY "${_native_root}/${_extra}" DESTINATION "${_fxr_dst}")
                endif()
            endforeach()

            set(_shared_dst
                "${_dotnet_root}/shared/Microsoft.NETCore.App/${SAO_CSHARP_DOTNET_FETCH_VERSION}")
            file(MAKE_DIRECTORY "${_shared_dst}")
            # Copy every DLL the nupkg ships under runtimes/win-x64/lib/net<major>.
            file(GLOB _pkg_shared_versions RELATIVE
                "${_pkg_shared_root}" "${_pkg_shared_root}/net*")
            foreach(_pkg_ver IN LISTS _pkg_shared_versions)
                file(GLOB _pkg_shared_dlls
                    "${_pkg_shared_root}/${_pkg_ver}/*.dll")
                if (_pkg_shared_dlls)
                    file(COPY ${_pkg_shared_dlls} DESTINATION "${_shared_dst}")
                endif()
            endforeach()
            # The nupkg also carries the extra native runtime bits used by
            # the shared framework (clrjit, hostpolicy, ...).  Mirror them so
            # coreclr can find its own dependencies.
            file(GLOB _pkg_native_dlls "${_native_root}/*.dll")
            if (_pkg_native_dlls)
                file(COPY ${_pkg_native_dlls} DESTINATION "${_shared_dst}")
            endif()

            _sao_cshost_resolve_root("${_dotnet_root}")
            if (SAO_CSHARP_HOSTFXR_DIR)
                set(SAO_CSHARP_RUNTIME_STATE "fetched" CACHE INTERNAL "" FORCE)
                set(SAO_CSHARP_EMBEDDED_RUNTIME_DIR "${_dotnet_root}"
                    CACHE PATH "" FORCE)
                message(STATUS
                    "plugins/csharp_host: fetched Microsoft.NETCore.App.Runtime.win-x64 ${SAO_CSHARP_DOTNET_FETCH_VERSION} into ${_dotnet_root}")
            else()
                message(WARNING
                    "plugins/csharp_host: unpacked nupkg but resulting DOTNET_ROOT was rejected: ${_dotnet_root}")
            endif()
        endif()
    endif()
endif()

if (SAO_CSHARP_RUNTIME_STATE STREQUAL "missing")
    message(STATUS
        "plugins/csharp_host: no .NET 8 runtime found; hostfxr LoadLibrary at runtime will decide fate")
endif()

# Publish resolution results as compile definitions so cs_host.cpp /
# cs_component.cpp can prefer the fetched runtime over LoadLibrary("hostfxr").
if (SAO_CSHARP_HOSTFXR_DIR)
    target_compile_definitions(sao_plugins_csharp_host PRIVATE
        SAO_CSHARP_HOSTFXR_DIR="${SAO_CSHARP_HOSTFXR_DIR}")
endif()
if (SAO_CSHARP_DOTNET_ROOT)
    target_compile_definitions(sao_plugins_csharp_host PRIVATE
        SAO_CSHARP_DOTNET_ROOT="${SAO_CSHARP_DOTNET_ROOT}")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
