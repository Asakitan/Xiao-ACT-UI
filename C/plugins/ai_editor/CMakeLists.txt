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
if (NOT _SAO_WEBVIEW2_ROOT AND SAO_BUILD_WEBVIEW_BRIDGE)
    message(STATUS "sao_ai_editor: WebView2 SDK not located; webview bridge disabled")
    set(SAO_BUILD_WEBVIEW_BRIDGE OFF CACHE BOOL "" FORCE)
endif()
if (SAO_BUILD_WEBVIEW_BRIDGE)
    message(STATUS "sao_ai_editor: WebView2 SDK at ${_SAO_WEBVIEW2_ROOT}")
    set(_SAO_WEBVIEW2_INCLUDE "${_SAO_WEBVIEW2_ROOT}/include")
    set(_SAO_WEBVIEW2_LIB    "${_SAO_WEBVIEW2_ROOT}/x64/WebView2LoaderStatic.lib")
    set(_SAO_WEBVIEW2_DLL    "${_SAO_WEBVIEW2_ROOT}/x64/WebView2Loader.dll")
    if (NOT EXISTS "${_SAO_WEBVIEW2_LIB}")
        message(STATUS "sao_ai_editor: WebView2LoaderStatic.lib missing at ${_SAO_WEBVIEW2_LIB}, disabling webview bridge")
        set(SAO_BUILD_WEBVIEW_BRIDGE OFF CACHE BOOL "" FORCE)
    endif()
endif()

add_library(sao_plugin_ai_editor SHARED
    src/ai_editor_launcher_process.cpp
    src/ai_editor_ipc_win32.cpp
    src/agent_registry.cpp
    src/ai_editor_native_runtime.cpp
    src/auth_device_flow.cpp
    src/chat_provider_router.cpp
    src/conversation_store.cpp
    src/extension_host.cpp
    src/gpu_hunt_bridge.cpp
    src/gpu_hunt_panel.cpp
    src/gpu_hunt_sdk_adapter.cpp
    src/gpu_hunt_tool.cpp
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
)
if (SAO_BUILD_WEBVIEW_BRIDGE)
    target_sources(sao_plugin_ai_editor PRIVATE src/webview_bridge.cpp)
    target_include_directories(sao_plugin_ai_editor PRIVATE
        "${_SAO_WEBVIEW2_INCLUDE}")
    target_link_libraries(sao_plugin_ai_editor PRIVATE
        "${_SAO_WEBVIEW2_LIB}" ole32 oleaut32 advapi32 version)
    target_compile_definitions(sao_plugin_ai_editor PRIVATE
        SAO_AI_EDITOR_HAS_WEBVIEW=1)
endif()
add_library(sao::plugin_ai_editor ALIAS sao_plugin_ai_editor)

target_include_directories(sao_plugin_ai_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(sao_plugin_ai_editor PRIVATE SAO_AI_EDITOR_BUILDING_DLL)
target_compile_features(sao_plugin_ai_editor PUBLIC cxx_std_20)
target_link_libraries(sao_plugin_ai_editor PRIVATE
    nlohmann_json::nlohmann_json
    sao::gpu_hunt
    sao::rt_io
    sao::sdk
    bcrypt
    crypt32
    shell32
    winhttp
)

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
    comctl32
    shell32
    user32
)

install(TARGETS sao_ai_editor sao_plugin_ai_editor
    RUNTIME DESTINATION "."
    COMPONENT ship
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/extension_host_shim.js"
    DESTINATION "assets/ai_editor"
    COMPONENT ship
)

# Market presets: bundled agent/workflow definitions surfaced by
# AgentRegistry / WorkflowRegistry as scope="market" (builtin=false so
# users can override or delete them via the normal save/delete flow).
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/agents/"
    DESTINATION "assets/ai_editor/agents"
    COMPONENT ship
    FILES_MATCHING PATTERN "*.json"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/workflows/"
    DESTINATION "assets/ai_editor/workflows"
    COMPONENT ship
    FILES_MATCHING PATTERN "*.json"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai_editor/prompts/"
    DESTINATION "assets/ai_editor/prompts"
    COMPONENT ship
    FILES_MATCHING PATTERN "*.json"
)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
