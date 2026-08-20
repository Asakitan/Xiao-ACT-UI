# plugins/ai_editor/ — AI Editor subprocess launcher and IPC plugin.
#
# AI Editor is a subprocess by design. SaoAiEditor is the production native
# Win32 child; the shared library owns the launcher, IPC, and runtime ABI.
#
option(SAO_BUILD_AI_EDITOR "Build the AI Editor subprocess launcher/IPC plugin" ON)
option(SAO_BUILD_WEBVIEW_BRIDGE "Build the WebView2 window bridge" ON)

if (NOT SAO_BUILD_AI_EDITOR)
    return()
endif()

# Discover the Microsoft WebView2 SDK layout dropped by NuGet
# (Microsoft.Web.WebView2.<version>/build/native/{include,x64}).  Falls back
# to environment vars if the NuGet cache is not present.
set(_SAO_WEBVIEW2_CANDIDATE_DIRS
    "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2"
    "$ENV{ProgramFiles}/Microsoft.Web.WebView2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/webview2"
)
set(_SAO_WEBVIEW2_ROOT "")
foreach (_candidate IN LISTS _SAO_WEBVIEW2_CANDIDATE_DIRS)
    if (EXISTS "${_candidate}")
        file(GLOB _versioned_children LIST_DIRECTORIES true "${_candidate}/*")
        list(SORT _versioned_children ORDER DESCENDING)
        foreach (_child IN LISTS _versioned_children)
            if (EXISTS "${_child}/build/native/include/WebView2.h")
                set(_SAO_WEBVIEW2_ROOT "${_child}/build/native")
                break()
            endif()
        endforeach()
    endif()
    if (_SAO_WEBVIEW2_ROOT)
        break()
    endif()
endforeach()
if (_SAO_WEBVIEW2_ROOT)
    file(TO_CMAKE_PATH "${_SAO_WEBVIEW2_ROOT}" _SAO_WEBVIEW2_ROOT)
endif()
if (NOT _SAO_WEBVIEW2_ROOT AND SAO_BUILD_WEBVIEW_BRIDGE)
    message(STATUS "sao_ai_editor: WebView2 SDK not located; webview bridge disabled")
    set(SAO_BUILD_WEBVIEW_BRIDGE OFF CACHE BOOL "" FORCE)
endif()
if (SAO_BUILD_WEBVIEW_BRIDGE)
    message(STATUS "sao_ai_editor: WebView2 SDK at ${_SAO_WEBVIEW2_ROOT}")
    set(_SAO_WEBVIEW2_INCLUDE "${_SAO_WEBVIEW2_ROOT}/include")
    set(_SAO_WEBVIEW2_LIB    "${_SAO_WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib")
    set(_SAO_WEBVIEW2_DLL    "${_SAO_WEBVIEW2_ROOT}/x64/WebView2Loader.dll")
    if (NOT EXISTS "${_SAO_WEBVIEW2_LIB}" OR
        NOT EXISTS "${_SAO_WEBVIEW2_DLL}")
        message(STATUS
            "sao_ai_editor: WebView2LoaderStatic.lib and WebView2Loader.dll "
            "are both required (${_SAO_WEBVIEW2_LIB}, ${_SAO_WEBVIEW2_DLL}); "
            "disabling webview bridge")
        set(SAO_BUILD_WEBVIEW_BRIDGE OFF CACHE BOOL "" FORCE)
    endif()
endif()

# gpu_hunt bridge/panel/tool/adapter translation units are local-only
# (see top-level .gitignore rule **/gpu_hunt_*.cpp).  A clean checkout is
# expected to succeed; each source is added only when the file is actually
# present.  Missing sources also skip the sao::gpu_hunt link below.
set(_SAO_AI_EDITOR_GPU_HUNT_SOURCES
    src/gpu_hunt_bridge.cpp
    src/gpu_hunt_central_panel.cpp
    src/gpu_hunt_sdk_adapter.cpp
    src/gpu_hunt_tool.cpp
)
set(_SAO_AI_EDITOR_GPU_HUNT_PRESENT_SOURCES "")
set(_SAO_AI_EDITOR_GPU_HUNT_MISSING_SOURCES "")
foreach (_sao_ai_editor_gpu_hunt_src IN LISTS _SAO_AI_EDITOR_GPU_HUNT_SOURCES)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sao_ai_editor_gpu_hunt_src}")
        list(APPEND _SAO_AI_EDITOR_GPU_HUNT_PRESENT_SOURCES
            "${_sao_ai_editor_gpu_hunt_src}")
    else()
        list(APPEND _SAO_AI_EDITOR_GPU_HUNT_MISSING_SOURCES
            "${_sao_ai_editor_gpu_hunt_src}")
    endif()
endforeach()
if (_SAO_AI_EDITOR_GPU_HUNT_MISSING_SOURCES)
    message(STATUS
        "sao_plugin_ai_editor: gpu_hunt sources not present in this checkout: "
        "${_SAO_AI_EDITOR_GPU_HUNT_MISSING_SOURCES}")
endif()

add_library(sao_plugin_ai_editor SHARED
    src/ai_editor_launcher_process.cpp
    src/ai_editor_ipc_win32.cpp
    src/agent_registry.cpp
    src/ai_editor_main_panel.cpp
    src/ai_editor_native_runtime.cpp
    src/ai_editor_settings.cpp
    src/ai_editor_settings_panel.cpp
    src/auth_device_flow.cpp
    src/chat_provider_router.cpp
    src/conversation_store.cpp
    src/extension_host.cpp
    ${_SAO_AI_EDITOR_GPU_HUNT_PRESENT_SOURCES}
    src/kernel_map_bridge.cpp
    src/kernel_map_commands.cpp
    src/kernel_map_panel_provider.cpp
    src/kernel_map_tools.cpp
    src/mcp_management_panel_provider.cpp
    src/mcp_client.cpp
    src/mcp_codec.cpp
    src/mcp_server.cpp
    src/native_secret_store.cpp
    src/native_tool_registry.cpp
    src/native_utils.cpp
    src/node_runtime.cpp
    src/openai_codec.cpp
    src/prompt_registry.cpp
    src/scope_store.cpp
    src/sha256_helper.cpp
    src/tool_execution_monitor.cpp
    src/tool_result_cache.cpp
    src/tool_result_filter.cpp
    src/webview_panel_registry.cpp
    src/winhttp_chat.cpp
    src/workflow_engine.cpp
    # Phase 10 (Python parity closure): live SDK dumper for 4 game engines,
    # chat-callable via native_tool_registry.
    src/sdk_dumper/sdk_dumper_base.cpp
    src/sdk_dumper/sdk_dumper_il2cpp.cpp
    src/sdk_dumper/sdk_dumper_mono.cpp
    src/sdk_dumper/sdk_dumper_unreal.cpp
    src/sdk_dumper/sdk_dumper_source.cpp
    # Phase 11 (Python parity closure): VSCode API host-side shim +
    # Streamable HTTP MCP transport (companion to existing stdio server).
    src/vscode_api_shim.cpp
    src/mcp_server_http.cpp
)
if (SAO_BUILD_WEBVIEW_BRIDGE)
    target_sources(sao_plugin_ai_editor PRIVATE src/webview_bridge.cpp)
    target_include_directories(sao_plugin_ai_editor PRIVATE
        "${_SAO_WEBVIEW2_INCLUDE}")
    target_link_libraries(sao_plugin_ai_editor PRIVATE
        "${_SAO_WEBVIEW2_LIB}" ole32 oleaut32 advapi32 version)
    target_compile_definitions(sao_plugin_ai_editor PRIVATE
        SAO_AI_EDITOR_HAS_WEBVIEW=1)
    add_custom_command(TARGET sao_plugin_ai_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SAO_WEBVIEW2_DLL}"
            "$<TARGET_FILE_DIR:sao_plugin_ai_editor>/WebView2Loader.dll"
        VERBATIM)
endif()
add_library(sao::plugin_ai_editor ALIAS sao_plugin_ai_editor)

target_include_directories(sao_plugin_ai_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(sao_plugin_ai_editor PRIVATE SAO_AI_EDITOR_BUILDING_DLL)
if (SAO_BUILD_TESTS)
    target_compile_definitions(sao_plugin_ai_editor PRIVATE SAO_AI_EDITOR_TESTING)
endif()
target_compile_features(sao_plugin_ai_editor PUBLIC cxx_std_20)
target_link_libraries(sao_plugin_ai_editor PUBLIC
    sao::ui
)
target_link_libraries(sao_plugin_ai_editor PRIVATE
    nlohmann_json::nlohmann_json
    sao::legacy_core
    sao::security::anti_screencap
    sao::rt_io
    sao::sdk
    # Phase 10 (SDK dumper) — engine adapter access via mem_probe.
    sao::mem_probe
    bcrypt
    crypt32
    comdlg32
    shell32
    winhttp
)
if (TARGET sao_security_anti_screencap)
    add_dependencies(sao_plugin_ai_editor sao_security_anti_screencap)
endif()
# Link sao::gpu_hunt only when at least one gpu_hunt-consuming source is
# present; on a clean checkout without gpu_hunt local-only sources the
# alias resolves to a stub INTERFACE library (see platform/CMakeLists.txt).
# Linking the stub is harmless but there is no consumer, so skip it to
# keep the dependency graph honest.
if (_SAO_AI_EDITOR_GPU_HUNT_PRESENT_SOURCES)
    target_link_libraries(sao_plugin_ai_editor PRIVATE sao::gpu_hunt)
endif()

add_executable(sao_ai_editor WIN32
    src/sao_ai_editor_main.cpp
)
set_target_properties(sao_ai_editor PROPERTIES OUTPUT_NAME "SaoAiEditor")
target_compile_features(sao_ai_editor PRIVATE cxx_std_20)
if (SAO_BUILD_WEBVIEW_BRIDGE)
    target_compile_definitions(sao_ai_editor PRIVATE
        SAO_AI_EDITOR_HAS_WEBVIEW=1)
endif()
target_link_libraries(sao_ai_editor PRIVATE
    sao::plugin_ai_editor
    sao::legacy_core
    sao::security::anti_screencap
    comctl32
    shell32
    user32
)
if (TARGET sao_security_anti_screencap)
    add_dependencies(sao_ai_editor sao_security_anti_screencap)
endif()
if (TARGET SaoKernelMapMcpServer)
    add_dependencies(sao_ai_editor SaoKernelMapMcpServer)
endif()

install(TARGETS sao_ai_editor sao_plugin_ai_editor
    RUNTIME DESTINATION "bin"
    COMPONENT ship
)

if (SAO_BUILD_WEBVIEW_BRIDGE)
    install(FILES "${_SAO_WEBVIEW2_DLL}"
        DESTINATION "bin"
        RENAME "WebView2Loader.dll"
        COMPONENT ship)
endif()

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/extension_host_shim.js"
    DESTINATION "bin/assets/ai_editor"
    COMPONENT ship
)

# Market presets: bundled agent/workflow definitions surfaced by
# AgentRegistry / WorkflowRegistry as scope="market" (builtin=false so
# users can override or delete them via the normal save/delete flow).
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/agents/"
    DESTINATION "bin/assets/ai_editor/agents"
    COMPONENT ship
    FILES_MATCHING PATTERN "*.json"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/workflows/"
    DESTINATION "bin/assets/ai_editor/workflows"
    COMPONENT ship
    FILES_MATCHING PATTERN "*.json"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/prompts/"
    DESTINATION "bin/assets/ai_editor/prompts"
    COMPONENT ship
    FILES_MATCHING PATTERN "*.json"
)

# Kernel Map operator dashboard assets — HTML/JS/CSS bundle loaded by
# the KernelMapPanelProvider at runtime.  Ships alongside the other
# ai_editor assets so options.system_root + "/assets/ai_editor/..."
# resolves cleanly.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/kernel_map_panel/"
    DESTINATION "bin/assets/ai_editor/kernel_map_panel"
    COMPONENT ship
    FILES_MATCHING
        PATTERN "*.html"
        PATTERN "*.js"
        PATTERN "*.css"
)

    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/mcp_management_panel/"
        DESTINATION "bin/assets/ai_editor/mcp_management_panel"
        COMPONENT ship
        FILES_MATCHING
        PATTERN "*.html"
        PATTERN "*.js"
        PATTERN "*.css"
    )

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
