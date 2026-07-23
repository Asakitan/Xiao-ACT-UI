# AI Editor real subprocess and named-pipe integration tests.

if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_executable(sao_plugin_ai_editor_child_fixture
    ai_editor_child_fixture.cpp
)
target_compile_features(sao_plugin_ai_editor_child_fixture PRIVATE cxx_std_20)

# Fake stdio MCP server used to exercise the notification forwarder end-to-end.
add_executable(sao_plugin_ai_editor_mcp_notification_fixture
    ai_editor_mcp_notification_fixture.cpp
)
target_compile_features(sao_plugin_ai_editor_mcp_notification_fixture PRIVATE
    cxx_std_20
)

add_executable(sao_plugin_ai_editor_tests
    ai_editor_native_test.cpp
    ai_editor_process_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/chat_provider_router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_secret_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_utils.cpp
    # tool_result_filter is compiled straight into the test target so the
    # built-in filter factories + is_protected_from_compression helper are
    # visible to the compression-filter test cases.  The plugin DLL also
    # owns a copy for its own dispatch pipeline; the two live in separate
    # translation units and never clash at link time.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tool_result_filter.cpp
)

target_link_libraries(sao_plugin_ai_editor_tests PRIVATE
    sao::plugin_ai_editor
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    crypt32
    ws2_32
)
target_include_directories(sao_plugin_ai_editor_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/ai_editor/src
)
if (SAO_BUILD_WEBVIEW_BRIDGE)
    target_compile_definitions(sao_plugin_ai_editor_tests PRIVATE
        SAO_AI_EDITOR_HAS_WEBVIEW=1)
endif()

target_compile_features(sao_plugin_ai_editor_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_plugin_ai_editor_tests PRIVATE
    SAO_AI_EDITOR_CHILD_FIXTURE="$<TARGET_FILE:sao_plugin_ai_editor_child_fixture>"
    SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE="$<TARGET_FILE:sao_ai_editor>"
    SAO_AI_EDITOR_MCP_NOTIFICATION_FIXTURE="$<TARGET_FILE:sao_plugin_ai_editor_mcp_notification_fixture>"
)
add_dependencies(sao_plugin_ai_editor_tests
    sao_plugin_ai_editor_child_fixture
    sao_plugin_ai_editor_mcp_notification_fixture
    sao_ai_editor
)

add_executable(sao_plugin_ai_editor_production_child_tests
    ai_editor_production_child_test.cpp
)
target_compile_features(sao_plugin_ai_editor_production_child_tests PRIVATE
    cxx_std_20
)
target_link_libraries(sao_plugin_ai_editor_production_child_tests PRIVATE
    sao::plugin_ai_editor
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    user32
)
target_compile_definitions(sao_plugin_ai_editor_production_child_tests PRIVATE
    SAO_AI_EDITOR_PRODUCTION_EXECUTABLE="$<TARGET_FILE:sao_ai_editor>"
    SAO_AI_EDITOR_HAS_WEBVIEW=$<BOOL:${SAO_BUILD_WEBVIEW_BRIDGE}>
    SAO_AI_EDITOR_MAIN_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../src/sao_ai_editor_main.cpp"
    SAO_AI_EDITOR_CMAKE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt"
    SAO_AI_EDITOR_WEBVIEW_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../src/webview_bridge.cpp"
)
add_dependencies(sao_plugin_ai_editor_production_child_tests
    sao_ai_editor
)

add_executable(sao_plugin_ai_editor_gpu_hunt_tests
    gpu_hunt_sdk_adapter_test.cpp
    test_stealth_window.cpp
    test_mmf_frame_writer.cpp
    test_input_event_ring.cpp
    test_self_render.cpp
    test_window_capture_mmf.cpp
    test_ai_editor_main_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ai_editor_main_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ai_editor_settings_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_central_panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_sdk_adapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_tool.cpp
)
target_compile_features(sao_plugin_ai_editor_gpu_hunt_tests PRIVATE
    cxx_std_20
)
target_compile_definitions(sao_plugin_ai_editor_gpu_hunt_tests PRIVATE
    SAO_AI_EDITOR_BUILDING_DLL
    SAO_AI_EDITOR_TESTING
)
target_include_directories(sao_plugin_ai_editor_gpu_hunt_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(sao_plugin_ai_editor_gpu_hunt_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    sao::gpu_hunt
    sao::rt_io
    sao::sdk
    sao::ui
    comctl32
    gdi32
    user32
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_panel.cpp")
    add_executable(sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests
        gpu_hunt_legacy_compatibility_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_central_panel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_panel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_sdk_adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_tool.cpp
    )
    target_compile_features(
        sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests PRIVATE
        cxx_std_20
    )
    target_compile_definitions(
        sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests PRIVATE
        SAO_AI_EDITOR_BUILDING_DLL
        SAO_AI_EDITOR_TESTING
        SAO_AI_EDITOR_LEGACY_GPU_HUNT_COMPAT_TEST=1
    )
    target_include_directories(
        sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    target_link_libraries(
        sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests PRIVATE
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
        sao::gpu_hunt
        sao::rt_io
        sao::sdk
        sao::ui
        comctl32
        gdi32
        user32
    )
endif()

include(CTest)
include(Catch)
catch_discover_tests(sao_plugin_ai_editor_tests
    TEST_PREFIX "plugins_ai_editor."
    PROPERTIES LABELS "plugins;ai_editor"
)
catch_discover_tests(sao_plugin_ai_editor_production_child_tests
    TEST_PREFIX "plugins_ai_editor.production_child."
    PROPERTIES LABELS "plugins;ai_editor;integration;production_child"
)
catch_discover_tests(sao_plugin_ai_editor_gpu_hunt_tests
    TEST_PREFIX "plugins_ai_editor.gpu_hunt."
    PROPERTIES LABELS "plugins;ai_editor;gpu_hunt"
)
if (TARGET sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests)
    catch_discover_tests(sao_plugin_ai_editor_gpu_hunt_legacy_compatibility_tests
        TEST_PREFIX "plugins_ai_editor.gpu_hunt.compatibility."
        PROPERTIES LABELS "plugins;ai_editor;gpu_hunt;compatibility;standalone_hwnd"
    )
endif()

add_executable(sao_plugin_ai_editor_settings_contract_tests
    test_ai_editor_settings_contract.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ai_editor_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_secret_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/scope_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sha256_helper.cpp
)
target_compile_features(sao_plugin_ai_editor_settings_contract_tests PRIVATE
    cxx_std_20
)
target_include_directories(sao_plugin_ai_editor_settings_contract_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(sao_plugin_ai_editor_settings_contract_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    bcrypt
    crypt32
)
catch_discover_tests(sao_plugin_ai_editor_settings_contract_tests
    TEST_PREFIX "plugins_ai_editor.settings."
    PROPERTIES LABELS "plugins;ai_editor;settings;contract"
)
