# plugins/ai_editor/ — AI Editor subprocess launcher and IPC plugin.
#
# AI Editor is a subprocess by design. SaoAiEditor is the production native
# Win32 child; the shared library owns the launcher, IPC, and runtime ABI.
#
option(SAO_BUILD_AI_EDITOR "Build the AI Editor subprocess launcher/IPC plugin" ON)

if (NOT SAO_BUILD_AI_EDITOR)
    return()
endif()

add_library(sao_plugin_ai_editor SHARED
    src/ai_editor_launcher_process.cpp
    src/ai_editor_ipc_win32.cpp
    src/ai_editor_native_runtime.cpp
    src/auth_device_flow.cpp
    src/chat_provider_router.cpp
    src/conversation_store.cpp
    src/mcp_client.cpp
    src/mcp_codec.cpp
    src/mcp_server.cpp
    src/native_secret_store.cpp
    src/native_tool_registry.cpp
    src/native_utils.cpp
    src/openai_codec.cpp
    src/scope_store.cpp
    src/winhttp_chat.cpp
    src/workflow_engine.cpp
)
add_library(sao::plugin_ai_editor ALIAS sao_plugin_ai_editor)

target_include_directories(sao_plugin_ai_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(sao_plugin_ai_editor PRIVATE SAO_AI_EDITOR_BUILDING_DLL)
target_compile_features(sao_plugin_ai_editor PUBLIC cxx_std_20)
target_link_libraries(sao_plugin_ai_editor PRIVATE
    nlohmann_json::nlohmann_json
    crypt32
    shell32
    winhttp
)

add_executable(sao_ai_editor WIN32
    src/sao_ai_editor_main.cpp
)
set_target_properties(sao_ai_editor PROPERTIES OUTPUT_NAME "SaoAiEditor")
target_compile_features(sao_ai_editor PRIVATE cxx_std_20)
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

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
