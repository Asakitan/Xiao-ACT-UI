# SAO Auto — launcher (SaoAuto.exe)
#
# The launcher is a small Win32 EXE that owns:
#   1. crash handler installation (minidump on unhandled SEH)
#   2. single-instance mutex
#   3. BASE_DIR resolution (equivalent to Python's config.BASE_DIR)
#   4. init pipeline sequence:
#         license verify        -> license/sdk
#         shell integrity check -> shell/stub
#         platform init         -> platform/core
#         plugin discovery      -> plugins/loader
#         UI online             -> platform/ui
#   5. graceful shutdown / cleanup
#
# It links against the full platform + security + shell + license + plugins
# meta-targets and is the only artefact end users double-click.  All other
# EXEs in the tree are CLI tools or plugin hosts.

set(SAO_LAUNCHER_HEADERS
    src/entity_action_routes_internal.h
    src/settings_codec_internal.h
    src/settings_json_internal.h
    src/settings_owner_internal.h
    src/settings_theme_internal.h
    src/tool_launch_internal.h
    src/tool_launch_package_internal.h
    include/sao/launcher/app.h
    include/sao/launcher/args.h
    include/sao/launcher/crash_handler.h
    include/sao/launcher/single_instance.h
    include/sao/launcher/working_dir.h
    include/sao/launcher/init_pipeline.h
    include/sao/launcher/shutdown.h
    include/sao/launcher/dual_run.h
    # Wave 10 / Agent a — Phase 12 rollout system.
    include/sao/launcher/rollout.h
    include/sao/launcher/provider_config.h
    include/sao/launcher/user_menu.h
)

set(SAO_LAUNCHER_SOURCES
    src/settings_codec.cpp
    src/settings_json_internal.cpp
    src/settings_owner.cpp
    src/settings_theme_internal.cpp
    src/tool_launch_internal.cpp
    src/tool_launch_package_internal.cpp
    src/main.cpp
    src/app.cpp
    src/args.cpp
    src/crash_handler.cpp
    src/single_instance.cpp
    src/working_dir.cpp
    src/init_pipeline.cpp
    src/shutdown.cpp
    src/dual_run.cpp
    # Wave 10 / Agent a — Phase 12 rollout system.
    src/rollout_manager.cpp
    src/rollout_dispatcher.cpp
    src/provider_config.cpp
    src/launcher_lifecycle.cpp
    src/user_menu.cpp
)

if (TARGET sao::shell::stub)
    add_library(sao_launcher_shell_provider STATIC src/shell_provider.cpp)
    target_include_directories(sao_launcher_shell_provider PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sao_launcher_shell_provider PRIVATE
        sao::shell::stub sao_core)
endif()

if (TARGET sao::license::sdk AND TARGET sao::license::client)
    add_library(sao_launcher_license_provider STATIC src/license_provider.cpp)
    target_include_directories(sao_launcher_license_provider PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sao_launcher_license_provider PRIVATE
        sao::license::sdk sao::license::client sao_core winhttp)
endif()

if (TARGET sao::plugins::loader)
    add_library(sao_launcher_plugins_provider STATIC src/plugins_provider.cpp)
    target_include_directories(sao_launcher_plugins_provider PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sao_launcher_plugins_provider PRIVATE
        sao::plugins::loader)
    target_compile_definitions(sao_launcher_plugins_provider PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
endif()

# Version resource + application manifest.  Both go into the final PE image.
set(SAO_LAUNCHER_RESOURCES
    resources/app.rc
)

add_executable(SaoAuto WIN32
    ${SAO_LAUNCHER_HEADERS}
    ${SAO_LAUNCHER_SOURCES}
    ${SAO_LAUNCHER_RESOURCES}
)

# The output filename is deliberately fixed to SaoAuto.exe — the packaging
# tools (tools/pack_cli) expect this exact name when building the release
# tree.  Do not rename.
set_target_properties(SaoAuto PROPERTIES
    OUTPUT_NAME "SaoAuto"
    SUFFIX ".exe"
    VS_DPI_AWARE "PerMonitor"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/diagnostics/pdb"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/diagnostics/compile-pdb"
)

target_include_directories(SaoAuto
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/platform/core/include
)
target_link_libraries(SaoAuto PRIVATE nlohmann_json::nlohmann_json dwmapi)
if (TARGET sao::core)
    target_link_libraries(SaoAuto PRIVATE sao::core)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_CORE_LOG_PROVIDER=1)
endif()

set(SAO_LAUNCHER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../python/icon.ico")
if (EXISTS "${SAO_LAUNCHER_ICON}")
    file(TO_CMAKE_PATH "${SAO_LAUNCHER_ICON}" SAO_LAUNCHER_ICON_RC_PATH)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_ICON_PATH=\"${SAO_LAUNCHER_ICON_RC_PATH}\")
else()
    message(STATUS "launcher: no application icon found; omitting icon resource")
endif()

# Link the whole platform.  The other agents' subsystems each expose an
# INTERFACE meta-target (sao::platform, sao::security, sao::shell,
# sao::license, sao::plugins) which pulls in all their sub-libs transitively.
#
# If a subsystem toggle is OFF at configure time, the meta-target won't
# exist; guard with TARGET so the launcher can still build a stripped-down
# version for iterative work.
target_link_libraries(SaoAuto PRIVATE)

# A subsystem aggregate describes link dependencies, not necessarily the
# launcher's concrete ABI provider.  Only a dedicated bridge target may
# suppress the fail-closed fallback in init_pipeline.cpp.
function(_sao_link_meta target define provider_target)
    if (TARGET ${target})
        target_link_libraries(SaoAuto PRIVATE ${target})
    else()
        message(STATUS "launcher: ${target} not available; retaining fail-closed ${define} stubs")
    endif()

    if (TARGET ${provider_target})
        target_link_libraries(SaoAuto PRIVATE ${provider_target})
        target_compile_definitions(SaoAuto PRIVATE ${define}=1)
        message(STATUS
            "launcher: ${target} uses dedicated ${provider_target}")
    else()
        message(STATUS "launcher: ${target} has no ${provider_target}; retaining fail-closed ${define} stubs")
    endif()
endfunction()

_sao_link_meta(sao::shell    SAO_LINKED_SHELL    sao_launcher_shell_provider)
_sao_link_meta(sao::license  SAO_LINKED_LICENSE  sao_launcher_license_provider)
_sao_link_meta(sao::plugins  SAO_LINKED_PLUGINS  sao_launcher_plugins_provider)

# The platform aggregate is not itself a lifecycle ABI provider.  The
# launcher owns the minimal composition provider in init_pipeline.cpp and may
# enable it only when both concrete runtime targets it calls are available.
if (TARGET sao::rt_io AND TARGET sao::ui)
    target_link_libraries(SaoAuto PRIVATE sao::rt_io sao::ui)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER=1)
else()
    message(STATUS
        "launcher: rt_io and/or ui target unavailable; retaining fail-closed platform composition")
endif()

# There is no aggregate security lifecycle ABI.  Use the concrete, stable
# anti-debug provider when it exists; the launcher composition wrapper calls
# it during security initialization and fails closed otherwise.
if (TARGET sao::security::anti_debug)
    target_link_libraries(SaoAuto PRIVATE sao::security::anti_debug)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_SECURITY_COMPOSITION_PROVIDER=1)
else()
    message(STATUS
        "launcher: anti_debug target unavailable; retaining fail-closed security composition")
endif()

# Standard Win32 libs.  Keep this list tight — everything else should come
# through the subsystem meta-targets, not the launcher directly.
target_link_libraries(SaoAuto PRIVATE
    user32
    gdi32
    shell32
    ole32
    advapi32
    dbghelp        # for minidump generation in crash_handler
    version
    shlwapi
    bcrypt         # HMAC-SHA-256 for rollout bucketing (Wave 10 Phase 12)
    crypt32        # current-user DPAPI for secure settings envelopes
)

# Wave 10 / Agent a — rollout_manager depends on freetier telemetry_client
# for its record_success / record_failure telemetry publishes.  When the
# server subsystem is disabled rollout_manager compiles a no-op emit path.
if (TARGET sao::server::freetier::telemetry_client)
    target_link_libraries(SaoAuto PRIVATE sao::server::freetier::telemetry_client)
    target_compile_definitions(SaoAuto PRIVATE SAO_LAUNCHER_HAS_TELEMETRY_CLIENT=1)
else()
    message(STATUS
        "launcher: telemetry_client not available; rollout telemetry will no-op")
endif()

target_compile_definitions(SaoAuto PRIVATE
    SAO_LAUNCHER_VERSION="${CMAKE_PROJECT_VERSION}"
)

set(SAO_LAUNCHER_DOCS_INDEX
    "${CMAKE_CURRENT_SOURCE_DIR}/../../python/docs/html/index.html")
set(SAO_LAUNCHER_DOCS_STYLE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../python/docs/html/assets/style.css")
if (NOT EXISTS "${SAO_LAUNCHER_DOCS_INDEX}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_STYLE}")
    message(FATAL_ERROR
        "launcher: required packaged user guide files are missing")
endif()

add_custom_command(TARGET SaoAuto POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_INDEX}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/index.html"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_STYLE}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/style.css"
    COMMENT "Copying packaged launcher user guide"
    VERBATIM
)

if (SAO_HARDENED_BUILD)
    target_compile_definitions(SaoAuto PRIVATE SAO_LAUNCHER_HARDENED=1)
    # Strip the CRT startup banner and turn off exception handlers where we
    # don't need them.  Doesn't affect functionality — only reduces the
    # attack surface of the launcher itself.
    if (MSVC)
        target_link_options(SaoAuto PRIVATE /ENTRY:wWinMainCRTStartup)
    endif()
endif()

# Manifest is embedded via app.rc, not via /MANIFEST option, so we can
# ship a fully DPI-aware / requestedExecutionLevel="asInvoker" build without
# depending on mt.exe's post-link step.
if (MSVC)
    target_link_options(SaoAuto PRIVATE /MANIFEST:NO)
endif()

# Tests for the launcher itself.  Cover argv parsing, single-instance mutex,
# BASE_DIR resolution.  Anything that needs a real GUI stays out of unit
# tests and moves into tests/integration/.
if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(TARGETS SaoAuto
    RUNTIME DESTINATION bin
    COMPONENT ship
)
install(FILES "${SAO_LAUNCHER_DOCS_INDEX}"
    DESTINATION bin/docs/html
    COMPONENT ship
)
install(FILES "${SAO_LAUNCHER_DOCS_STYLE}"
    DESTINATION bin/docs/html/assets
    COMPONENT ship
)
