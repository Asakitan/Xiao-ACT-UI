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
#   6. No configure-time download path exists; use a preprovisioned runtime.
#
# Detection is best-effort at configure time; the runtime source at
# cs_component.cpp still performs a live LoadLibrary probe, so a false
# negative here leaves the runtime capability unavailable until deployment
# provides the required PATH/environment.
# When any resolution step succeeds we populate:
#   SAO_CSHARP_RUNTIME_STATE   — one of "detected" or "missing"
#   SAO_CSHARP_HOSTFXR_DIR     — directory containing hostfxr.dll
#   SAO_CSHARP_SHARED_ROOT_DIR — directory containing Microsoft.NETCore.App
#   SAO_CSHARP_DOTNET_ROOT     — detected DOTNET_ROOT for runtime launch
option(SAO_PLUGINS_CSHARP_AUTO_FETCH
    "Legacy compatibility switch; runtime downloads are disabled." OFF)
if (SAO_PLUGINS_CSHARP_AUTO_FETCH)
    message(WARNING
        "SAO_PLUGINS_CSHARP_AUTO_FETCH is stale and is being forced OFF; "
        "runtime acquisition remains offline.")
    set(SAO_PLUGINS_CSHARP_AUTO_FETCH OFF CACHE BOOL
        "Legacy compatibility switch; runtime downloads are disabled." FORCE)
endif()
set(SAO_CSHARP_HOSTFXR_DLL "" CACHE FILEPATH
    "Absolute path to a preinstalled hostfxr.dll (skips runtime detection)")
set(SAO_CSHARP_EMBEDDED_RUNTIME_DIR "" CACHE PATH
    "Directory containing a preinstalled .NET runtime layout (host/fxr/... + shared/Microsoft.NETCore.App/...)")
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

if (SAO_CSHARP_RUNTIME_STATE STREQUAL "missing")
    message(STATUS
        "plugins/csharp_host: no .NET 8 runtime found; hostfxr LoadLibrary at runtime will decide fate")
endif()

# Publish resolution results as compile definitions so cs_host.cpp /
# cs_component.cpp can prefer the preprovisioned runtime over LoadLibrary("hostfxr").
if (SAO_CSHARP_HOSTFXR_DIR)
    target_compile_definitions(sao_plugins_csharp_host PRIVATE
        SAO_CSHARP_HOSTFXR_DIR="${SAO_CSHARP_HOSTFXR_DIR}")
endif()
if (SAO_CSHARP_DOTNET_ROOT)
    target_compile_definitions(sao_plugins_csharp_host PRIVATE
        SAO_CSHARP_DOTNET_ROOT="${SAO_CSHARP_DOTNET_ROOT}")
endif()

