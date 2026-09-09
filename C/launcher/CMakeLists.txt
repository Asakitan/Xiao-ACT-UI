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
    src/entity_builtin_action_internal.h
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
    # Rollout system.
    include/sao/launcher/rollout.h
    include/sao/launcher/provider_config.h
    include/sao/launcher/auto_update.h
    include/sao/launcher/user_menu.h
    include/sao/launcher/user_guide_webview.h
    # Boot-residency restart UX (W6.9 state machine + executor).
    include/sao/launcher/boot_residency.h
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
    # Rollout system.
    src/rollout_manager.cpp
    src/rollout_dispatcher.cpp
    src/provider_config.cpp
    src/launcher_lifecycle.cpp
    src/user_menu.cpp
    # WebView2-hosted user guide (dynamic loader, ShellExecute fallback).
    src/user_guide_webview.cpp
    # Phase 12 (Python parity closure): hotkey manager.
    src/hotkey_manager.cpp
    # Phase 14: install_npcap + native comdlg32 file picker fallback.
    src/install_npcap.cpp
    # Boot-residency restart UX (W6.9 state machine + executor).
    src/boot_residency.cpp
)

# ---------------------------------------------------------------------------
# WebView2 (user guide host).  Mirrors the AI Editor probe: NuGet layout
# %USERPROFILE%/.nuget/packages/microsoft.web.webview2/<ver>/build/native.
# When absent the guide falls back to ShellExecute (default browser).
# ---------------------------------------------------------------------------
set(_SAO_GUIDE_WEBVIEW2_CANDIDATE_DIRS
    "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2"
    "$ENV{ProgramFiles}/Microsoft.Web.WebView2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/webview2")
set(_SAO_GUIDE_WEBVIEW2_ROOT "")
foreach (_candidate IN LISTS _SAO_GUIDE_WEBVIEW2_CANDIDATE_DIRS)
    file(GLOB _children LIST_DIRECTORIES true "${_candidate}/*")
    list(SORT _children ORDER DESCENDING)
    foreach (_child IN LISTS _children)
        if (IS_DIRECTORY "${_child}" AND
            EXISTS "${_child}/build/native/include/WebView2.h")
            set(_SAO_GUIDE_WEBVIEW2_ROOT "${_child}/build/native")
            break()
        endif()
    endforeach()
    if (_SAO_GUIDE_WEBVIEW2_ROOT)
        break()
    endif()
endforeach()
if (_SAO_GUIDE_WEBVIEW2_ROOT)
    file(TO_CMAKE_PATH "${_SAO_GUIDE_WEBVIEW2_ROOT}" _SAO_GUIDE_WEBVIEW2_ROOT)
    set(_SAO_GUIDE_WEBVIEW2_DLL "${_SAO_GUIDE_WEBVIEW2_ROOT}/x64/WebView2Loader.dll")
    if (NOT EXISTS "${_SAO_GUIDE_WEBVIEW2_DLL}")
        set(_SAO_GUIDE_WEBVIEW2_ROOT "")
    endif()
endif()
if (_SAO_GUIDE_WEBVIEW2_ROOT)
    message(STATUS "sao_launcher: WebView2 SDK at ${_SAO_GUIDE_WEBVIEW2_ROOT}")
else()
    message(STATUS "sao_launcher: WebView2 SDK not located; user guide falls back to default browser")
endif()
set(SAO_LAUNCHER_GUIDE_WEBVIEW2_ENABLED FALSE CACHE INTERNAL
    "Whether the native launcher guide host stages WebView2Loader" FORCE)
if (_SAO_GUIDE_WEBVIEW2_ROOT)
    set(SAO_LAUNCHER_GUIDE_WEBVIEW2_ENABLED TRUE CACHE INTERNAL
        "Whether the native launcher guide host stages WebView2Loader" FORCE)
endif()

set(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION OFF)
if (TARGET sao::ui AND TARGET sao::rt_io AND TARGET sao::sdk AND
    TARGET sao::server::freetier::workshop_client)
    list(APPEND SAO_LAUNCHER_HEADERS
        src/plugin_manager_panel_internal.h
        src/process_selector_panel_internal.h
        src/workshop_panel_internal.h)
    list(APPEND SAO_LAUNCHER_SOURCES
        src/plugin_manager_panel_internal.cpp
        src/process_selector_panel_internal.cpp
        src/workshop_panel_internal.cpp)
    if (TARGET sao::license::sdk AND TARGET sao::license::client)
        list(APPEND SAO_LAUNCHER_HEADERS src/license_panel_internal.h)
        list(APPEND SAO_LAUNCHER_SOURCES src/license_panel_internal.cpp)
    endif()
    set(SAO_LAUNCHER_SHARED_PANEL_COMPOSITION ON)
endif()

if (TARGET sao::ui)
    list(APPEND SAO_LAUNCHER_SOURCES
        src/entity_builtin_action_internal.cpp
        src/hotkey_config_panel.cpp
        src/settings_config_panel.cpp
        src/settings_profiles.cpp)
else()
    list(APPEND SAO_LAUNCHER_SOURCES
        src/hotkey_config_panel_stub.cpp
        src/settings_config_panel_stub.cpp)

    add_library(sao_launcher_ui_off_stubs STATIC EXCLUDE_FROM_ALL
        src/hotkey_config_panel_stub.cpp
        src/settings_config_panel_stub.cpp)
    target_compile_features(sao_launcher_ui_off_stubs PRIVATE cxx_std_20)
    target_include_directories(sao_launcher_ui_off_stubs PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/platform/core/include
        ${CMAKE_SOURCE_DIR}/platform/ui/include)

    add_library(sao_launcher_ui_off_link_smoke SHARED EXCLUDE_FROM_ALL
        src/args.cpp
        src/single_instance.cpp
        src/user_menu.cpp
        src/user_guide_webview.cpp
        src/working_dir.cpp)
    target_compile_features(sao_launcher_ui_off_link_smoke PRIVATE cxx_std_20)
    target_compile_definitions(sao_launcher_ui_off_link_smoke PRIVATE
        SAO_LAUNCHER_UI_OFF_LINK_SMOKE=1)
    target_include_directories(sao_launcher_ui_off_link_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/platform/core/include
        ${CMAKE_SOURCE_DIR}/platform/ui/include
        ${CMAKE_SOURCE_DIR}/security/obfuscation/include)
    target_link_libraries(sao_launcher_ui_off_link_smoke PRIVATE
        sao_launcher_ui_off_stubs
        user32
        shell32)
    if (_SAO_GUIDE_WEBVIEW2_ROOT)
        target_include_directories(sao_launcher_ui_off_link_smoke PRIVATE
            "${_SAO_GUIDE_WEBVIEW2_ROOT}/include")
        target_compile_definitions(sao_launcher_ui_off_link_smoke PRIVATE
            SAO_LAUNCHER_HAS_WEBVIEW2=1)
        target_link_libraries(sao_launcher_ui_off_link_smoke PRIVATE ole32)
    endif()
endif()

if (TARGET sao::plugins::loader AND TARGET sao::rt_io AND TARGET sao::ui)
    list(APPEND SAO_LAUNCHER_HEADERS
        src/entity_provider_catalog_internal.h
        src/entity_provider_publication_internal.h)
    list(APPEND SAO_LAUNCHER_SOURCES
        src/entity_provider_publication_internal.cpp)
endif()

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
        sao::license::sdk sao::license::client sao_core winhttp crypt32 bcrypt)
endif()

if (TARGET sao::plugins::loader)
    add_library(sao_launcher_plugins_provider STATIC src/plugins_provider.cpp)
    target_include_directories(sao_launcher_plugins_provider PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sao_launcher_plugins_provider PRIVATE
        sao::plugins::loader)
    if (TARGET sao::sdk AND TARGET sao::ui)
        target_link_libraries(sao_launcher_plugins_provider PRIVATE
            sao::sdk sao::ui)
        target_compile_definitions(sao_launcher_plugins_provider PUBLIC
            SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK=1)
    endif()

    if (TARGET sao::plugins::emma_host)
        target_link_libraries(sao_launcher_plugins_provider PRIVATE
            sao::plugins::emma_host)
        target_compile_definitions(sao_launcher_plugins_provider PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_EMMA=1)
    endif()
    if (TARGET sao::plugins::angel_host)
        target_link_libraries(sao_launcher_plugins_provider PRIVATE
            sao::plugins::angel_host)
        target_compile_definitions(sao_launcher_plugins_provider PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_ANGEL=1)
    endif()
    if (TARGET sao::plugins::lua_host)
        target_link_libraries(sao_launcher_plugins_provider PRIVATE
            sao::plugins::lua_host)
        target_compile_definitions(sao_launcher_plugins_provider PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_LUA=1)
    endif()
    if (TARGET sao::plugins::csharp_host)
        target_link_libraries(sao_launcher_plugins_provider PRIVATE
            sao::plugins::csharp_host)
        target_compile_definitions(sao_launcher_plugins_provider PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_CSHARP=1)
    endif()
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
if(WIN32 AND TARGET sao::ui AND TARGET sao::sdk AND TARGET sao::plugin_ai_editor AND
   TARGET sao::rt_io AND TARGET sao::server::freetier::workshop_client)
    # The preview is a production-compositor developer shell.  Compile the
    # launcher owners into it so their real factories can run without starting
    # the full launcher pipeline; an AI backend remains an explicit CLI opt-in.
    add_executable(sao_ui_preview WIN32 EXCLUDE_FROM_ALL
        preview/production_ui_host.cpp
        src/settings_codec.cpp
        src/settings_json_internal.cpp
        src/settings_owner.cpp
        src/settings_theme_internal.cpp
        src/settings_profiles.cpp
        src/settings_config_panel.cpp
        src/hotkey_manager.cpp
        src/hotkey_config_panel.cpp
        src/user_guide_webview.cpp
        src/user_menu.cpp
        src/args.cpp
        src/single_instance.cpp
        src/working_dir.cpp
        src/plugin_manager_panel_internal.cpp
        src/process_selector_panel_internal.cpp
        src/workshop_panel_internal.cpp
    )
    target_compile_features(sao_ui_preview PRIVATE cxx_std_20)
    target_compile_definitions(sao_ui_preview PRIVATE
        UNICODE
        _UNICODE
        SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER=1
        SAO_LAUNCHER_SHARED_PANEL_COMPOSITION=1
    )
    target_include_directories(sao_ui_preview PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/platform/core/include
        ${CMAKE_SOURCE_DIR}/platform/sdk/include
    )
    target_link_libraries(sao_ui_preview PRIVATE
        nlohmann_json::nlohmann_json
        sao::ui
        sao::sdk
        sao::plugin_ai_editor
        sao::rt_io
        sao::server::freetier::workshop_client
        user32
        gdi32
        ole32
        shell32
        advapi32
        crypt32
        bcrypt
        shlwapi
    )
    if(TARGET sao::license::sdk AND TARGET sao::license::client)
        target_sources(sao_ui_preview PRIVATE src/license_panel_internal.cpp)
        target_link_libraries(sao_ui_preview PRIVATE sao::license::sdk sao::license::client)
        target_compile_definitions(sao_ui_preview PRIVATE SAO_LAUNCHER_LICENSE_PANEL=1)
    endif()
    if(TARGET SaoAiEditor)
        add_dependencies(sao_ui_preview SaoAiEditor)
    endif()
    if(MSVC)
        target_compile_options(sao_ui_preview PRIVATE /utf-8)
    endif()
endif()
if (TARGET sao::server::freetier::updater AND TARGET SaoAutoUpdateHelper)
    target_sources(SaoAuto PRIVATE src/auto_update.cpp)
    target_link_libraries(SaoAuto PRIVATE sao::server::freetier::updater)
    target_compile_definitions(SaoAuto PRIVATE SAO_LAUNCHER_HAS_AUTO_UPDATE=1)
    add_dependencies(SaoAuto SaoAutoUpdateHelper)
    add_custom_command(TARGET SaoAuto POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:SaoAutoUpdateHelper>"
            "$<TARGET_FILE_DIR:SaoAuto>/SaoAutoUpdateHelper.exe"
        COMMENT "Copying native launcher update helper"
        VERBATIM
    )
else()
    message(STATUS
        "launcher: native updater/helper unavailable; auto update stays disabled")
endif()
target_compile_definitions(SaoAuto PRIVATE
    "$<$<CONFIG:Debug>:SAO_LAUNCHER_ACTUAL_DEBUG=1>")

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
    # SAO_ENC_STR() header-only compile-time string encryption; no link dep.
    ${CMAKE_SOURCE_DIR}/security/obfuscation/include
)
if (_SAO_GUIDE_WEBVIEW2_ROOT)
    target_include_directories(SaoAuto PRIVATE
        "${_SAO_GUIDE_WEBVIEW2_ROOT}/include")
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_HAS_WEBVIEW2=1)
    if(TARGET sao_ui_preview)
        target_include_directories(sao_ui_preview PRIVATE
            "${_SAO_GUIDE_WEBVIEW2_ROOT}/include")
        target_compile_definitions(sao_ui_preview PRIVATE SAO_LAUNCHER_HAS_WEBVIEW2=1)
    endif()
endif()
target_link_libraries(SaoAuto PRIVATE nlohmann_json::nlohmann_json dwmapi)
if (TARGET sao::core)
    target_link_libraries(SaoAuto PRIVATE sao::core)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_CORE_LOG_PROVIDER=1)
endif()
if (TARGET sao::ui)
    target_link_libraries(SaoAuto PRIVATE sao::ui)
endif()
# Anti-screencap facade: the launcher consumes enable_anti_screencap and
# registers process windows with the threat coordinator at startup.
# Declared after security/ is configured, mirroring platform/ui.
if (TARGET sao::security::anti_screencap)
    target_link_libraries(SaoAuto PRIVATE sao::security::anti_screencap)
endif()
if (SAO_LAUNCHER_SHARED_PANEL_COMPOSITION)
    target_link_libraries(SaoAuto PRIVATE
        sao::ui
        sao::rt_io
        sao::server::freetier::workshop_client)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_SHARED_PANEL_COMPOSITION=1)
    if (TARGET sao::license::sdk AND TARGET sao::license::client)
        target_link_libraries(SaoAuto PRIVATE sao::license::sdk sao::license::client)
        target_compile_definitions(SaoAuto PRIVATE SAO_LAUNCHER_LICENSE_PANEL=1)
    else()
        message(STATUS
            "launcher: license sdk/client unavailable; license activation panel will not register")
    endif()
else()
    message(STATUS
        "launcher: shared workshop/process/plugin panels unavailable; retaining fail-closed Entity actions")
endif()

if (TARGET sao::plugin_ai_editor)
    target_link_libraries(SaoAuto PRIVATE sao::plugin_ai_editor)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_HAS_AI_EDITOR_ABI=1)
    if (TARGET sao_ai_editor)
        add_dependencies(SaoAuto sao_ai_editor)
    endif()
    message(STATUS
        "launcher: native AI Editor uses dedicated sao::plugin_ai_editor ABI")
else()
    message(STATUS
        "launcher: ai_editor ABI unavailable; OPEN_AI_EDITOR remains fail-closed")
endif()

set(SAO_LAUNCHER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../python/icon.ico")
if (EXISTS "${SAO_LAUNCHER_ICON}")
    file(TO_CMAKE_PATH "${SAO_LAUNCHER_ICON}" SAO_LAUNCHER_ICON_RC_PATH)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_ICON_PATH=\"${SAO_LAUNCHER_ICON_RC_PATH}\")
else()
    message(STATUS "launcher: no application icon found; omitting icon resource")
endif()

# Link the whole platform.  The other subsystems each expose an
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
# enable it only when every concrete runtime target it calls is available.
if (TARGET sao::rt_io AND TARGET sao::ui AND TARGET sao::sdk)
    target_link_libraries(SaoAuto PRIVATE sao::rt_io sao::ui sao::sdk)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER=1)
else()
    message(STATUS
        "launcher: rt_io, ui, and/or sdk target unavailable; retaining fail-closed platform composition")
endif()

if (TARGET sao::plugins::loader AND
    TARGET sao_launcher_plugins_provider AND
    TARGET sao::rt_io AND TARGET sao::ui)
    target_link_libraries(SaoAuto PRIVATE sao::plugins::loader)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_ENTITY_PROVIDER_COMPOSITION=1
        SAO_PLUGINS_BUILDING_DLL)
else()
    message(STATUS
        "launcher: native Entity provider publication unavailable; retaining fail-closed dynamic actions")
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

# anti-dump provider: PE header scrub + snapshot watch are wired into the
# security composition init when the static target exists.
if (TARGET sao::security::anti_dump)
    target_link_libraries(SaoAuto PRIVATE sao::security::anti_dump)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_ANTI_DUMP_PROVIDER=1)
endif()

# The user-evasion library exposes passive ntdll/gadget diagnostics alongside
# active primitives. Launcher composition consumes only the passive evidence
# APIs and keeps findings advisory unless strict_user_evasion is enabled.
if (TARGET sao::security::user_evasion)
    target_link_libraries(SaoAuto PRIVATE sao::security::user_evasion)
    target_compile_definitions(SaoAuto PRIVATE
        SAO_LAUNCHER_USER_EVASION_PROVIDER=1
        SAO_SECURITY_USER_EVASION_BUILDING_DLL)
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
    bcrypt         # HMAC-SHA-256 for deterministic rollout bucketing
    crypt32        # current-user DPAPI for secure settings envelopes
)

# rollout slice — rollout_manager depends on freetier telemetry_client
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
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/index.html")
set(SAO_LAUNCHER_DOCS_STYLE
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/style.css")
set(SAO_LAUNCHER_DOCS_THEME
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/theme.js")
set(SAO_LAUNCHER_DOCS_SCROLLSPY
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/scrollspy.js")
set(SAO_LAUNCHER_DOCS_PARTICLES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/particles.js")
set(SAO_LAUNCHER_DOCS_SFX
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/sfx.js")
set(SAO_LAUNCHER_DOCS_SC_CURSOR
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/sc_cursor.js")
set(SAO_LAUNCHER_DOCS_LINKSTART
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/linkstart.js")
set(SAO_LAUNCHER_DOCS_GPU_RIPPLE
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/gpu_ripple.js")
set(SAO_LAUNCHER_DOCS_DETAIL_POPUP
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/detail_popup.js")
set(SAO_LAUNCHER_DOCS_SOUNDS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/html/assets/sounds")
set(SAO_LAUNCHER_DOCS_SOUND_NAMES
    "Dismiss.SAO.Launcher.wav"
    "Dismiss.SAO.Message.wav"
    "Feedback.SAO.Click.wav"
    "Popup.SAO.Launcher.wav"
    "Popup.SAO.Menu.wav"
    "Popup.SAO.Panel.wav")
set(SAO_LAUNCHER_DOCS_SOUND_FILES "")
foreach (_sao_launcher_guide_sound IN LISTS SAO_LAUNCHER_DOCS_SOUND_NAMES)
    set(_sao_launcher_guide_sound_path
        "${SAO_LAUNCHER_DOCS_SOUNDS_DIR}/${_sao_launcher_guide_sound}")
    if (NOT EXISTS "${_sao_launcher_guide_sound_path}" OR
        IS_DIRECTORY "${_sao_launcher_guide_sound_path}")
        message(FATAL_ERROR
            "launcher: required guide sound is missing: ${_sao_launcher_guide_sound_path}")
    endif()
    list(APPEND SAO_LAUNCHER_DOCS_SOUND_FILES
        "${_sao_launcher_guide_sound_path}")
endforeach()
if (NOT EXISTS "${SAO_LAUNCHER_DOCS_INDEX}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_STYLE}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_THEME}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_SCROLLSPY}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_PARTICLES}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_SFX}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_SC_CURSOR}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_LINKSTART}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_GPU_RIPPLE}" OR
    NOT EXISTS "${SAO_LAUNCHER_DOCS_DETAIL_POPUP}")
    message(FATAL_ERROR
        "launcher: required packaged user guide files are missing")
endif()

add_custom_command(TARGET SaoAuto POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_INDEX}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/index.html"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_STYLE}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/style.css"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_THEME}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/theme.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_SCROLLSPY}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/scrollspy.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_PARTICLES}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/particles.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_SFX}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/sfx.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_SC_CURSOR}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/sc_cursor.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_LINKSTART}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/linkstart.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_GPU_RIPPLE}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/gpu_ripple.js"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SAO_LAUNCHER_DOCS_DETAIL_POPUP}"
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/detail_popup.js"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/sounds"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${SAO_LAUNCHER_DOCS_SOUND_FILES}
        "$<TARGET_FILE_DIR:SaoAuto>/docs/html/assets/sounds"
    COMMENT "Copying packaged launcher user guide"
    VERBATIM
)

set(SAO_LAUNCHER_PROVIDER_CONFIG_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/config/SaoAuto.provider.json")
add_custom_command(TARGET SaoAuto POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DSAO_PROVIDER_CONFIG_SOURCE=${SAO_LAUNCHER_PROVIDER_CONFIG_SOURCE}"
        "-DSAO_PROVIDER_CONFIG_DEST=$<TARGET_FILE_DIR:SaoAuto>/SaoAuto.provider.json"
        "-DSAO_PROVIDER_CONFIG_CONFIG=$<CONFIG>"
        -P "${CMAKE_SOURCE_DIR}/cmake/stage_provider_config.cmake"
    COMMENT "Staging launcher provider configuration"
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

# Launcher tests and integration-test registration were removed on 2026-08-23.

install(TARGETS SaoAuto
    RUNTIME DESTINATION bin
    COMPONENT ship
)
if (_SAO_GUIDE_WEBVIEW2_ROOT)
    add_custom_command(TARGET SaoAuto POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_SAO_GUIDE_WEBVIEW2_DLL}"
            "$<TARGET_FILE_DIR:SaoAuto>/WebView2Loader.dll"
        COMMENT "Copying WebView2Loader.dll for user guide host"
        VERBATIM
    )
    install(FILES "${_SAO_GUIDE_WEBVIEW2_DLL}"
        DESTINATION bin
        COMPONENT ship
    )
endif()

install(FILES "${SAO_LAUNCHER_DOCS_INDEX}"
    DESTINATION bin/docs/html
    COMPONENT ship
)
install(FILES "${SAO_LAUNCHER_DOCS_STYLE}"
    DESTINATION bin/docs/html/assets
    COMPONENT ship
)
install(FILES
    "${SAO_LAUNCHER_DOCS_THEME}"
    "${SAO_LAUNCHER_DOCS_SCROLLSPY}"
    "${SAO_LAUNCHER_DOCS_PARTICLES}"
    "${SAO_LAUNCHER_DOCS_GPU_RIPPLE}"
    "${SAO_LAUNCHER_DOCS_SFX}"
    "${SAO_LAUNCHER_DOCS_SC_CURSOR}"
    "${SAO_LAUNCHER_DOCS_LINKSTART}"
    "${SAO_LAUNCHER_DOCS_DETAIL_POPUP}"
    DESTINATION bin/docs/html/assets
    COMPONENT ship
)
install(FILES ${SAO_LAUNCHER_DOCS_SOUND_FILES}
    DESTINATION bin/docs/html/assets/sounds
    COMPONENT ship
)
