# plugins/ai_editor/ — AI Editor subprocess launcher and IPC plugin.
#
# AI Editor is a **subprocess** by design (mirrors the Python-side
# `subprocess.Popen(AiEditor.exe, ...)` pattern), so the C++ side only
# owns the launcher + IPC channel + status ABI, not the WebView2 host
# or LLM/MCP/Node/Roslyn stack.  That deep implementation lives inside
# the child `AiEditor.exe` and lands in Wave 10+.
#
option(SAO_BUILD_AI_EDITOR "Build the AI Editor subprocess launcher/IPC plugin" ON)

if (NOT SAO_BUILD_AI_EDITOR)
    return()
endif()

add_library(sao_plugin_ai_editor SHARED
    src/ai_editor_launcher_process.cpp
    src/ai_editor_ipc_win32.cpp
)
add_library(sao::plugin_ai_editor ALIAS sao_plugin_ai_editor)

target_include_directories(sao_plugin_ai_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(sao_plugin_ai_editor PRIVATE SAO_AI_EDITOR_BUILDING_DLL)
target_compile_features(sao_plugin_ai_editor PUBLIC cxx_std_20)
target_link_libraries(sao_plugin_ai_editor PRIVATE shell32)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
