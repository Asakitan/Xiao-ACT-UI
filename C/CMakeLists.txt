cmake_minimum_required(VERSION 3.28)

# vcpkg must be selected before project() initializes languages.  When it is
# absent, the skeleton keeps configuring and optional consumers self-disable.
if (DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

# ---------------------------------------------------------------------------
# SAO Auto — top-level project file.
#
# This is a fully-independent native C++ platform.  It never imports Python,
# never depends on any file under sao_auto/python/, and produces a single
# distributable end-artifact: SaoAuto.exe (plus a handful of DLLs, plugin
# hosts, tools and drivers).
#
# The tree is organised into six top-level subsystems + a launcher:
#
#   platform/   — game-agnostic runtime (core, net, engine, ui, sdk, scripting)
#   security/   — anti-cheat, obfuscation, crypto, anti-debug, anti-dump,
#                 driver loader, shellcode
#   shell/      — dynamic crypter shell + cloud key delivery
#   license/    — cloud license client + protocol + SDK
#   server/     — server-side components (only server/freetier is in-repo,
#                 server/private stays out of source control)
#   plugins/    — multi-engine plugin hosts (Python, Lua, AngelScript, C#,
#                 Emma) + SDK binding + loader + example plugins
#   launcher/   — the standalone SaoAuto.exe entry point
#
# Plus:
#   tests/      — Catch2 aggregator and cross-subsystem smoke tests
#   tools/      — CLI utilities (sign, pack, crypter, obfuscate, shellcode_gen,
#                 plugin_pack)
#
# Each subsystem owns its own CMakeLists.txt.  This file only defines the
# global compile / link posture and stitches subsystems together.
# ---------------------------------------------------------------------------

project(SaoAutoC
    VERSION 0.2.0
    DESCRIPTION "SAO Auto — fully independent native C++ platform"
    LANGUAGES C CXX ASM_MASM)

# ---------------------------------------------------------------------------
# Platform gate
# ---------------------------------------------------------------------------
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR
        "SaoAutoC targets Windows only (Win32, DirectComposition, D3D11, WGL, "
        "kernel drivers).  Cross-compilation is not supported.")
endif()

# ---------------------------------------------------------------------------
# Language settings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Output layout
#
# All runtime artefacts (SaoAuto.exe, DLLs, plugin hosts, drivers) land under
# ${CMAKE_BINARY_DIR}/bin so downstream packagers/tools find them in one
# predictable place.  Import libraries and static archives go to lib/.
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------
# Subsystem toggles
#
# Runtime skeleton targets default ON so a fresh `cmake --preset windows-debug`
# proves the whole tree is coherent. Optional tests and SDK-backed hosts stay
# off until their dependencies or implementations are available.
# ---------------------------------------------------------------------------
option(SAO_BUILD_PLATFORM   "Platform layer (core/net/engine/ui/sdk/scripting)"       ON)
option(SAO_BUILD_SECURITY   "Security layer (anti-cheat/obf/crypto/anti-debug/...)"  ON)
option(SAO_BUILD_SHELL      "Dynamic crypter shell + cloud key delivery"              ON)
option(SAO_BUILD_LICENSE    "Cloud license client + SDK"                              ON)
option(SAO_BUILD_SERVER     "Server components (freetier only in-repo)"               ON)
option(SAO_BUILD_PLUGINS    "Multi-engine plugin hosts"                               ON)
option(SAO_BUILD_LAUNCHER   "Standalone SaoAuto.exe launcher"                         ON)
option(SAO_BUILD_TESTS      "Catch2 unit tests + integration smoke tests"             OFF)
option(SAO_BUILD_TOOLS      "CLI tools: sign / pack / crypter / obfuscate / ..."      ON)

# Hardening posture — flip ON for ship builds. Adds LTO, CFG, CET when the
# linker supports it, ASLR/DEP, and reproducible-link settings without
# changing the public ABI.
option(SAO_HARDENED_BUILD   "Hardened release: LTO/CFG/CET/ASLR/DEP"                   OFF)

# ---------------------------------------------------------------------------
# Global compile posture
# ---------------------------------------------------------------------------
if (MSVC)
    add_compile_options(
        /W4                 # warning level 4
        /permissive-        # strict conformance
        /utf-8              # source + exec charset = UTF-8
        /MP                 # parallel compilation
        /GR-                # disable RTTI globally (subsystems that need it opt in)
        /guard:cf           # control-flow guard
        /Zc:__cplusplus     # correct __cplusplus macro
        /Zc:preprocessor    # standards-conforming preprocessor
    )
    add_compile_definitions(
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0A00     # Windows 10 minimum
        NTDDI_VERSION=0x0A000000
    )

    include(CheckLinkerFlag)
    check_linker_flag(CXX "/CETCOMPAT" SAO_MSVC_SUPPORTS_CETCOMPAT)

    # Release configurations and hardened builds share a deterministic,
    # optimized link posture. Keep this language-scoped so MASM sources do
    # not receive C/C++ compiler switches.
    foreach (_sao_lang IN ITEMS C CXX)
        add_compile_options(
            "$<$<AND:$<COMPILE_LANGUAGE:${_sao_lang}>,$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>>:/GL>"
            "$<$<AND:$<COMPILE_LANGUAGE:${_sao_lang}>,$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>>:/Gy>"
            "$<$<AND:$<COMPILE_LANGUAGE:${_sao_lang}>,$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>>:/Gw>"
            "$<$<AND:$<COMPILE_LANGUAGE:${_sao_lang}>,$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>>:/Brepro>"
        )
    endforeach()

    add_link_options(
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/LTCG>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/OPT:REF>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/OPT:ICF>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/INCREMENTAL:NO>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/GUARD:CF>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/DYNAMICBASE>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/HIGHENTROPYVA>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/NXCOMPAT>"
        "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/Brepro>"
    )
    if (SAO_MSVC_SUPPORTS_CETCOMPAT)
        add_link_options(
            "$<$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>:/CETCOMPAT>"
        )
    endif()

    if (SAO_HARDENED_BUILD)
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:C>:/Zi>"
            "$<$<COMPILE_LANGUAGE:CXX>:/Zi>"
        )
        add_link_options(/DEBUG:FULL /PDBALTPATH:%_PDB%)
        add_compile_definitions(SAO_HARDENED=1)
    endif()

    # /std:c++20 already set by CMAKE_CXX_STANDARD, but reinforce here so
    # CMake generators (Ninja, VS2022) both honour it.
    add_compile_options(/std:c++20)
endif()

# ---------------------------------------------------------------------------
# Subsystem wiring
#
# NOTE: legacy top-level layout:
#   overlay/, engine/, ui/, pybridge/  -> archived under _legacy_skeleton/,
#                                         no consumers, physically moved out
#                                         of the build tree.
#   core/                              -> KEPT as compatibility target: it
#                                         still exports sao_core + the
#                                         include/sao_core/ header layout
#                                         that security/ and legacy plugins
#                                         directly reference.  Depth impl
#                                         lives in platform/core/
#                                         (sao_platform_core, include/sao/core/).
#                                         Phase-1 will merge these; keeping
#                                         both is a skeleton-only bridge.
#   plugins/{hide_seek,midi_piano,include,src,tests}  -> kept, folded into
#                                         plugins/CMakeLists.txt as v1
#                                         compat layer alongside v2 hosts.
# ---------------------------------------------------------------------------

# `platform/core` owns the canonical sao::core target.  The old `core/`
# library is kept under its un-namespaced target plus sao::legacy_core for
# headers and DLL imports that have not migrated yet.
option(SAO_BUILD_LEGACY_CORE "Build legacy sao_core compat target (required by security/)" ON)

# Bootstrap Catch2 process-wide when tests are enabled.  Subsystem tests
# use `find_package(Catch2 3 CONFIG REQUIRED)`; when the host lacks vcpkg
# / system Catch2, FetchContent v3.5.4 populates the Catch2::* targets
# globally via the FIND_PACKAGE_ARGS integration (CMake >= 3.24) so
# subsystem `find_package(Catch2 3 CONFIG REQUIRED)` calls succeed.  A
# vcpkg-provisioned host still takes precedence -- FetchContent only
# builds when find_package fails.  Must run BEFORE subsystems are added.
if (SAO_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4
        FIND_PACKAGE_ARGS 3 CONFIG
    )
    FetchContent_MakeAvailable(Catch2)
    if (catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
        # Top-level `add_compile_definitions(UNICODE _UNICODE)` makes
        # Catch2's `catch_main.cpp` define `wmain` instead of `main`,
        # but MSVC /SUBSYSTEM:CONSOLE's default entry references `main`.
        # `DO_NOT_USE_WMAIN` forces the `main` variant so no test target
        # has to override `/ENTRY:` on the linker.
        if (TARGET Catch2WithMain)
            target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)
        endif()
    endif()
endif()

# nlohmann_json is a header-only runtime dependency of platform/core (Wave 5
# Phase 1 settings envelope).  Prefer a vcpkg / system installation; when the
# host lacks either, fall back to FetchContent so the build gate is still
# satisfiable on a bare CI runner.  Header-only + CMake 3.24 FIND_PACKAGE_ARGS
# means find_package(nlohmann_json 3 CONFIG REQUIRED) at the subsystem level
# picks up whichever provider resolves first.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    FIND_PACKAGE_ARGS 3 CONFIG
)
FetchContent_MakeAvailable(nlohmann_json)

if (SAO_BUILD_PLATFORM)
    add_subdirectory(platform)
endif()
if (SAO_BUILD_LEGACY_CORE)
    add_subdirectory(core)
endif()
if (SAO_BUILD_SECURITY)
    add_subdirectory(security)
endif()
if (SAO_BUILD_LICENSE)
    add_subdirectory(license)
endif()
if (SAO_BUILD_SHELL)
    add_subdirectory(shell)
endif()
if (SAO_BUILD_SERVER)
    add_subdirectory(server)
endif()
if (SAO_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()
if (SAO_BUILD_LAUNCHER)
    add_subdirectory(launcher)
endif()
if (SAO_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Ship staging and audit
#
# The staged directory is intentionally fed only by install() rules. PDBs and
# intermediate linker outputs remain in the build diagnostics tree rather
# than entering the distributable directory. The audit target inspects the
# final PE's headers, imports, exports, embedded strings, and resources.
# ---------------------------------------------------------------------------
if (TARGET SaoAuto)
    set(SAO_SHIP_STAGE_DIRECTORY "${CMAKE_BINARY_DIR}/ship" CACHE PATH
        "Temporary install root inspected by sao_release_artifact_audit")

    add_custom_target(sao_release_ship_stage
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAO_SHIP_STAGE_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
            --config $<CONFIG> --component ship
            --prefix "${SAO_SHIP_STAGE_DIRECTORY}"
        DEPENDS SaoAuto
        COMMENT "Staging installable release artifacts"
        VERBATIM
    )

    get_filename_component(_sao_linker_directory "${CMAKE_LINKER}" DIRECTORY)
    find_program(SAO_DUMPBIN_EXECUTABLE NAMES dumpbin.exe dumpbin
        HINTS "${_sao_linker_directory}")

    add_custom_target(sao_release_artifact_audit
        COMMAND "${CMAKE_COMMAND}"
                "-DSAO_AUDIT_BINARY=$<TARGET_FILE:SaoAuto>"
                "-DSAO_AUDIT_SHIP_DIRECTORY=${SAO_SHIP_STAGE_DIRECTORY}"
                "-DSAO_AUDIT_DUMPBIN=${SAO_DUMPBIN_EXECUTABLE}"
                "-DSAO_AUDIT_EXPECT_HARDENING=$<IF:$<OR:$<CONFIG:Release>,$<BOOL:${SAO_HARDENED_BUILD}>>,ON,OFF>"
                "-DSAO_AUDIT_EXPECT_CET=$<IF:$<BOOL:${SAO_MSVC_SUPPORTS_CETCOMPAT}>,ON,OFF>"
                "-DSAO_AUDIT_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/audit_release_artifact.cmake"
        DEPENDS sao_release_ship_stage
        COMMENT "Auditing release PE hardening and ship-stage contents"
        VERBATIM
    )
endif()

include(CTest)
if (SAO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Wave-specific implementation archives were temporary delivery scaffolding.
# Their sources must live in stable production targets; wave-named test
# executables remain valid.  Fail during configure if an implementation
# archive is reintroduced anywhere below the source root.
function(sao_assert_no_wave_impl_libraries directory)
    get_property(_sao_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_sao_target IN LISTS _sao_targets)
        get_target_property(_sao_target_type "${_sao_target}" TYPE)
        if (_sao_target MATCHES "(^|_)wave[0-9]+[A-Za-z0-9_]*_impl$" AND
            _sao_target_type MATCHES "^(STATIC|OBJECT|INTERFACE)_LIBRARY$")
            message(FATAL_ERROR
                "Temporary wave implementation library '${_sao_target}' is forbidden; "
                "merge its sources into a stable production target")
        endif()
    endforeach()

    get_property(_sao_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach (_sao_subdirectory IN LISTS _sao_subdirectories)
        sao_assert_no_wave_impl_libraries("${_sao_subdirectory}")
    endforeach()
endfunction()

sao_assert_no_wave_impl_libraries("${CMAKE_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "SAO Auto configuration:")
message(STATUS "  version         : ${PROJECT_VERSION}")
message(STATUS "  hardened build  : ${SAO_HARDENED_BUILD}")
message(STATUS "  CETCOMPAT       : ${SAO_MSVC_SUPPORTS_CETCOMPAT}")
message(STATUS "  legacy core     : ${SAO_BUILD_LEGACY_CORE}")
message(STATUS "  platform layer  : ${SAO_BUILD_PLATFORM}")
message(STATUS "  security layer  : ${SAO_BUILD_SECURITY}")
message(STATUS "  shell           : ${SAO_BUILD_SHELL}")
message(STATUS "  license         : ${SAO_BUILD_LICENSE}")
message(STATUS "  server          : ${SAO_BUILD_SERVER}")
message(STATUS "  plugins         : ${SAO_BUILD_PLUGINS}")
message(STATUS "  launcher        : ${SAO_BUILD_LAUNCHER}")
message(STATUS "  tools           : ${SAO_BUILD_TOOLS}")
message(STATUS "  tests           : ${SAO_BUILD_TESTS}")
