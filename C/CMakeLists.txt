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

if (SAO_BUILD_PLUGINS AND NOT SAO_BUILD_PLATFORM)
    message(FATAL_ERROR
        "SAO_BUILD_PLUGINS=ON requires SAO_BUILD_PLATFORM=ON. "
        "For the supported platform-off matrix configure with "
        "-DSAO_BUILD_PLATFORM=OFF -DSAO_BUILD_PLUGINS=OFF.")
endif()

# Hardening posture — flip ON for ship builds. Adds LTO, CFG, CET when the
# linker supports it, ASLR/DEP, and reproducible-link settings without
# changing the public ABI.
option(SAO_HARDENED_BUILD   "Hardened release: LTO/CFG/CET/ASLR/DEP"                   OFF)

# ---------------------------------------------------------------------------
# Global compile posture
# ---------------------------------------------------------------------------
if (MSVC)
    # Language-scoped so MASM (as_callfunc_x64_msvc_asm.asm in the
    # FetchContent-provided AngelScript runtime) does not receive C/C++
    # compiler switches that ml64 rejects (/permissive-, /utf-8, /MP,
    # /GR-, /guard:cf, /Zc:*).  Every C or C++ TU still sees the full
    # posture; MASM sees only the /nologo /Fo… flags MSBuild injects.
    foreach (_sao_msvc_lang IN ITEMS C CXX)
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/W4>"                 # warning level 4
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/permissive->"        # strict conformance
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/utf-8>"              # source + exec charset = UTF-8
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/MP>"                 # parallel compilation
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/GR->"                # disable RTTI globally (opt-in per target)
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/guard:cf>"           # control-flow guard
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/Zc:__cplusplus>"     # correct __cplusplus macro
            "$<$<COMPILE_LANGUAGE:${_sao_msvc_lang}>:/Zc:preprocessor>"    # standards-conforming preprocessor
        )
    endforeach()
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
    # CMake generators (Ninja, VS2022) both honour it.  Language-scoped so
    # ml64/MASM does not choke on it — see the compile-options block above.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++20>")
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
#                                         A later compatibility cleanup will
#                                         merge these; keeping both is a
#                                         skeleton-only bridge.
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

# nlohmann_json is a header-only runtime dependency of the platform/core
# settings envelope.  Prefer a vcpkg / system installation; when the
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

# Iteration identifiers are delivery history, not feature names.  Configure
# rejects them in persistent artifacts and build metadata while preserving the
# published ABI aliases and the `w2s` world-to-screen domain abbreviation.
set(_SAO_ITERATION_NAME_PATTERN
    "(^|[^a-z0-9])(wave[0-9]+[a-z]?|w[0-9]+[a-z]?|phase[0-9]+[a-z]?)([^a-z0-9]|$)")
string(CONCAT _SAO_PUBLISHED_ITERATION_ABI_IDENTIFIER_PATTERN
    "^sao_("
    "(shell_crypter_wave8|(security_)?anti_debug_wave8|"
    "security_anti_dump_wave8|security_shellcode_wave9|"
    "rt_io_wave(12h|13e|13f))_[a-z0-9_]+"
    "|engine_event_(wave5_callback_t|"
    "bus_(create|subscribe|publish|publish_ex)_wave5))$")

function(sao_value_has_iteration_name value out_has_iteration)
    string(TOLOWER "${value}" _sao_candidate)
    if (_sao_candidate MATCHES "${_SAO_ITERATION_NAME_PATTERN}")
        set(${out_has_iteration} TRUE PARENT_SCOPE)
    else()
        set(${out_has_iteration} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sao_is_published_iteration_abi value out_is_published_abi)
    string(TOLOWER "${value}" _sao_value)
    string(REGEX REPLACE "[^a-z0-9_]+" ";" _sao_identifiers "${_sao_value}")
    set(_sao_has_iteration FALSE)
    set(_sao_is_published TRUE)
    foreach (_sao_identifier IN LISTS _sao_identifiers)
        sao_value_has_iteration_name("${_sao_identifier}" _sao_identifier_has_iteration)
        if (NOT _sao_identifier_has_iteration)
            continue()
        endif()
        set(_sao_has_iteration TRUE)
        if (NOT _sao_identifier MATCHES
            "${_SAO_PUBLISHED_ITERATION_ABI_IDENTIFIER_PATTERN}")
            set(_sao_is_published FALSE)
            break()
        endif()
    endforeach()
    if (NOT _sao_has_iteration)
        set(_sao_is_published FALSE)
    endif()
    set(${out_is_published_abi} ${_sao_is_published} PARENT_SCOPE)
endfunction()

function(sao_assert_semantic_name value context allow_published_abi)
    sao_value_has_iteration_name("${value}" _sao_has_iteration)
    if (NOT _sao_has_iteration)
        return()
    endif()
    if (allow_published_abi)
        sao_is_published_iteration_abi("${value}" _sao_is_published_abi)
        if (_sao_is_published_abi)
            return()
        endif()
    endif()
    message(FATAL_ERROR
        "Iteration-named ${context} '${value}' is forbidden; use a semantic feature name")
endfunction()

function(sao_is_allowed_iteration_path relative_path out_is_allowed)
    string(REPLACE "\\" "/" _sao_path "${relative_path}")
    string(TOLOWER "${_sao_path}" _sao_path)
    set(_sao_is_allowed FALSE)
    if (_sao_path MATCHES
        "^platform/gpu_hunt/(include/sao/gpu_hunt/w2s\\.h|src/w2s\\.cpp|tests/test_w2s\\.cpp)$")
        set(_sao_is_allowed TRUE)
    endif()
    set(${out_is_allowed} ${_sao_is_allowed} PARENT_SCOPE)
endfunction()

function(sao_assert_no_iteration_artifact_paths root)
    file(GLOB _sao_top_level_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${root}"
        "${root}/*")
    set(_sao_entries "")
    foreach (_sao_top_level_entry IN LISTS _sao_top_level_entries)
        string(TOLOWER "${_sao_top_level_entry}" _sao_top_level_entry_lower)
        if (_sao_top_level_entry_lower MATCHES
            "^(build|testing|cmakefiles|__pycache__|bin|obj|docs|\\.git|\\.vs|\\.cache)$" OR
            _sao_top_level_entry_lower MATCHES
            "^(plan\\.md|status\\.md|changelog\\.md)$")
            continue()
        endif()
        list(APPEND _sao_entries "${_sao_top_level_entry}")
        if (IS_DIRECTORY "${root}/${_sao_top_level_entry}")
            file(GLOB_RECURSE _sao_nested_entries
                LIST_DIRECTORIES TRUE
                RELATIVE "${root}"
                "${root}/${_sao_top_level_entry}/*")
            list(APPEND _sao_entries ${_sao_nested_entries})
        endif()
    endforeach()
    foreach (_sao_entry IN LISTS _sao_entries)
        string(REPLACE "\\" "/" _sao_entry "${_sao_entry}")
        string(TOLOWER "${_sao_entry}" _sao_entry_lower)
        if (_sao_entry_lower MATCHES
            "(^|/)(build|testing|cmakefiles|__pycache__|bin|obj|\\.git|\\.vs|\\.cache)(/|$)" OR
            _sao_entry_lower MATCHES "^(docs/|plan\\.md$|status\\.md$|changelog\\.md$)")
            continue()
        endif()
        sao_value_has_iteration_name("${_sao_entry}" _sao_has_iteration)
        if (NOT _sao_has_iteration)
            continue()
        endif()
        sao_is_allowed_iteration_path("${_sao_entry}" _sao_is_allowed)
        if (NOT _sao_is_allowed)
            message(FATAL_ERROR
                "Iteration-named persistent artifact '${_sao_entry}' is forbidden; "
                "rename the file or directory by feature")
        endif()
    endforeach()
endfunction()

function(sao_assert_no_iteration_cmake_source root)
    file(GLOB _sao_top_level_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${root}"
        "${root}/*")
    set(_sao_cmake_files "")
    foreach (_sao_top_level_entry IN LISTS _sao_top_level_entries)
        string(TOLOWER "${_sao_top_level_entry}" _sao_top_level_entry_lower)
        if (NOT IS_DIRECTORY "${root}/${_sao_top_level_entry}" OR
            _sao_top_level_entry_lower MATCHES
            "^(build|docs|testing|cmakefiles|__pycache__|bin|obj|\\.git|\\.vs|\\.cache)$")
            continue()
        endif()
        file(GLOB_RECURSE _sao_nested_cmake_files
            LIST_DIRECTORIES FALSE
            RELATIVE "${root}"
            "${root}/${_sao_top_level_entry}/CMakeLists.txt"
            "${root}/${_sao_top_level_entry}/*.cmake")
        list(APPEND _sao_cmake_files ${_sao_nested_cmake_files})
    endforeach()
    foreach (_sao_relative_file IN LISTS _sao_cmake_files)
        string(REPLACE "\\" "/" _sao_relative_file "${_sao_relative_file}")
        string(TOLOWER "${_sao_relative_file}" _sao_relative_file_lower)
        if (_sao_relative_file_lower MATCHES "(^|/)(build|docs|testing)/")
            continue()
        endif()
        file(STRINGS "${root}/${_sao_relative_file}" _sao_cmake_lines ENCODING UTF-8)
        set(_sao_line_number 0)
        set(_sao_metadata_command "")
        set(_sao_set_property_is_compile_definitions FALSE)
        foreach (_sao_line IN LISTS _sao_cmake_lines)
            math(EXPR _sao_line_number "${_sao_line_number} + 1")
            string(REGEX REPLACE "#.*$" "" _sao_code "${_sao_line}")
            string(STRIP "${_sao_code}" _sao_code)
            if (_sao_code STREQUAL "")
                continue()
            endif()
            if (_sao_relative_file_lower STREQUAL "platform/gpu_hunt/cmakelists.txt")
                string(REPLACE "src/w2s.cpp" "" _sao_code "${_sao_code}")
            elseif (_sao_relative_file_lower STREQUAL
                    "platform/gpu_hunt/tests/cmakelists.txt")
                string(REPLACE "test_w2s.cpp" "" _sao_code "${_sao_code}")
            endif()

            if (_sao_code MATCHES
                "^[ \t]*(target_compile_options|add_compile_options)\\(")
                set(_sao_metadata_command "compile_options")
                set(_sao_set_property_is_compile_definitions FALSE)
            elseif (_sao_code MATCHES
                "^[ \t]*(target_compile_definitions|add_compile_definitions)\\(")
                set(_sao_metadata_command "compile_definitions")
                set(_sao_set_property_is_compile_definitions FALSE)
            elseif (_sao_code MATCHES "^[ \t]*set_property\\(")
                set(_sao_metadata_command "set_property")
                set(_sao_set_property_is_compile_definitions FALSE)
            endif()
            if (_sao_metadata_command STREQUAL "set_property" AND
                _sao_code MATCHES
                "(^|[ \t])(INTERFACE_)?COMPILE_DEFINITIONS([ \t)]|$)")
                set(_sao_set_property_is_compile_definitions TRUE)
            endif()
            if (_sao_metadata_command STREQUAL "compile_options")
                # `/W4`, `-W3`, and similar compiler warning switches are not
                # iteration identifiers.
                string(REGEX REPLACE "([/-])W[0-9]+" "" _sao_code "${_sao_code}")
                string(REGEX REPLACE "([/-])w[0-9]+" "" _sao_code "${_sao_code}")
            endif()

            set(_sao_allow_published_abi FALSE)
            if (_sao_metadata_command STREQUAL "compile_definitions" OR
                _sao_set_property_is_compile_definitions)
                set(_sao_allow_published_abi TRUE)
            endif()
            sao_assert_semantic_name(
                "${_sao_code}"
                "CMake metadata at ${_sao_relative_file}:${_sao_line_number}"
                ${_sao_allow_published_abi})
            if (NOT _sao_metadata_command STREQUAL "" AND
                _sao_code MATCHES "\\)$")
                set(_sao_metadata_command "")
                set(_sao_set_property_is_compile_definitions FALSE)
            endif()
        endforeach()
    endforeach()
endfunction()

function(sao_allowed_iteration_test_tag_limit relative_path tag out_limit)
    string(REPLACE "\\" "/" _sao_path "${relative_path}")
    string(TOLOWER "${_sao_path}" _sao_path)
    string(TOLOWER "${tag}" _sao_tag)
    set(_sao_limit 0)
    if (_sao_path STREQUAL "platform/gpu_hunt/tests/test_w2s.cpp" AND
            _sao_tag STREQUAL "w2s")
        set(_sao_limit 5)
    endif()
    set(${out_limit} ${_sao_limit} PARENT_SCOPE)
endfunction()

function(sao_assert_no_iteration_test_tags root)
    file(GLOB _sao_top_level_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${root}"
        "${root}/*")
    set(_sao_test_sources "")
    foreach (_sao_top_level_entry IN LISTS _sao_top_level_entries)
        string(TOLOWER "${_sao_top_level_entry}" _sao_top_level_entry_lower)
        if (NOT IS_DIRECTORY "${root}/${_sao_top_level_entry}" OR
            _sao_top_level_entry_lower MATCHES
            "^(build|docs|testing|cmakefiles|__pycache__|bin|obj|\\.git|\\.vs|\\.cache)$")
            continue()
        endif()
        file(GLOB_RECURSE _sao_nested_test_sources
            LIST_DIRECTORIES FALSE
            RELATIVE "${root}"
            "${root}/${_sao_top_level_entry}/*.cc"
            "${root}/${_sao_top_level_entry}/*.cpp"
            "${root}/${_sao_top_level_entry}/*.cxx")
        list(APPEND _sao_test_sources ${_sao_nested_test_sources})
    endforeach()
    foreach (_sao_test_source IN LISTS _sao_test_sources)
        string(REPLACE "\\" "/" _sao_test_source "${_sao_test_source}")
        string(TOLOWER "${_sao_test_source}" _sao_test_source_lower)
        if (_sao_test_source_lower MATCHES "(^|/)(build|docs|testing)/" OR
            NOT _sao_test_source_lower MATCHES "(^|/)tests?/")
            continue()
        endif()
        file(READ "${root}/${_sao_test_source}" _sao_test_source_text)
        string(TOLOWER "${_sao_test_source_text}" _sao_test_source_text)
        string(REGEX MATCHALL
            "\\[(wave[0-9]+[a-z]?|w[0-9]+[a-z]?|phase[0-9]+[a-z]?)\\]"
            _sao_iteration_tags
            "${_sao_test_source_text}")
        set(_sao_seen_tags "")
        foreach (_sao_tag_match IN LISTS _sao_iteration_tags)
            string(REGEX REPLACE "^\\[|\\]$" "" _sao_tag "${_sao_tag_match}")
            string(TOLOWER "${_sao_tag}" _sao_tag)
            list(APPEND _sao_seen_tags "${_sao_tag}")
            sao_allowed_iteration_test_tag_limit(
                "${_sao_test_source}" "${_sao_tag}" _sao_allowed_limit)
            if (_sao_allowed_limit EQUAL 0)
                message(FATAL_ERROR
                    "Iteration-named Catch2 tag '[${_sao_tag}]' in "
                    "${_sao_test_source} is forbidden; use a semantic tag")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _sao_seen_tags)
        foreach (_sao_seen_tag IN LISTS _sao_seen_tags)
            set(_sao_tag_count 0)
            foreach (_sao_tag_match IN LISTS _sao_iteration_tags)
                string(REGEX REPLACE "^\\[|\\]$" "" _sao_tag "${_sao_tag_match}")
                string(TOLOWER "${_sao_tag}" _sao_tag)
                if (_sao_tag STREQUAL _sao_seen_tag)
                    math(EXPR _sao_tag_count "${_sao_tag_count} + 1")
                endif()
            endforeach()
            sao_allowed_iteration_test_tag_limit(
                "${_sao_test_source}" "${_sao_seen_tag}" _sao_allowed_limit)
            if (_sao_tag_count GREATER _sao_allowed_limit)
                message(FATAL_ERROR
                    "New iteration-named Catch2 tag '[${_sao_seen_tag}]' in "
                    "${_sao_test_source} exceeds the allowed domain baseline")
            endif()
        endforeach()
    endforeach()
endfunction()

function(sao_assert_no_iteration_metadata directory)
    get_property(_sao_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_sao_target IN LISTS _sao_targets)
        sao_assert_semantic_name("${_sao_target}" "CMake target" FALSE)
        foreach (_sao_definition_property IN ITEMS
                COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(
                _sao_definitions "${_sao_target}" "${_sao_definition_property}")
            if (_sao_definitions AND NOT _sao_definitions MATCHES "-NOTFOUND$")
                foreach (_sao_definition IN LISTS _sao_definitions)
                    sao_assert_semantic_name(
                        "${_sao_definition}"
                        "compile definition on target ${_sao_target}"
                        TRUE)
                endforeach()
            endif()
        endforeach()
    endforeach()

    get_property(_sao_directory_definitions
        DIRECTORY "${directory}" PROPERTY COMPILE_DEFINITIONS)
    foreach (_sao_definition IN LISTS _sao_directory_definitions)
        sao_assert_semantic_name(
            "${_sao_definition}" "directory compile definition" TRUE)
    endforeach()

    get_property(_sao_tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach (_sao_test IN LISTS _sao_tests)
        sao_assert_semantic_name("${_sao_test}" "CTest test" FALSE)
        get_property(_sao_labels
            TEST "${_sao_test}" DIRECTORY "${directory}" PROPERTY LABELS)
        foreach (_sao_label IN LISTS _sao_labels)
            sao_assert_semantic_name(
                "${_sao_label}" "CTest label on ${_sao_test}" FALSE)
        endforeach()
    endforeach()

    get_property(_sao_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach (_sao_subdirectory IN LISTS _sao_subdirectories)
        file(RELATIVE_PATH _sao_relative_subdirectory
            "${CMAKE_SOURCE_DIR}" "${_sao_subdirectory}")
        string(REPLACE "\\" "/" _sao_relative_subdirectory
            "${_sao_relative_subdirectory}")
        string(TOLOWER "${_sao_relative_subdirectory}"
            _sao_relative_subdirectory_lower)
        if (_sao_relative_subdirectory_lower MATCHES "^\\.\\." OR
            _sao_relative_subdirectory_lower MATCHES "^(build|docs|testing)/")
            continue()
        endif()
        sao_assert_no_iteration_metadata("${_sao_subdirectory}")
    endforeach()
endfunction()

# Keep the documented detector entry point; AGENTS.md lists this audit helper
# as an explicit exception because its name describes the rule it enforces.
function(sao_assert_no_wave_impl_libraries directory)
    sao_assert_no_iteration_metadata("${directory}")
endfunction()

sao_assert_no_iteration_artifact_paths("${CMAKE_SOURCE_DIR}")
sao_assert_no_iteration_cmake_source("${CMAKE_SOURCE_DIR}")
sao_assert_no_iteration_test_tags("${CMAKE_SOURCE_DIR}")
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
