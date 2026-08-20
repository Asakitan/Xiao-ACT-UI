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
)
add_dependencies(sao_plugin_ai_editor_production_child_tests
    sao_ai_editor
)

add_executable(sao_plugin_ai_editor_capture_tests
    test_stealth_window.cpp
    test_mmf_frame_writer.cpp
    test_input_event_ring.cpp
    test_self_render.cpp
    test_window_capture_mmf.cpp
)
target_compile_features(sao_plugin_ai_editor_capture_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_plugin_ai_editor_capture_tests PRIVATE
    SAO_AI_EDITOR_BUILDING_DLL
    SAO_AI_EDITOR_TESTING
)
target_include_directories(sao_plugin_ai_editor_capture_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(sao_plugin_ai_editor_capture_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    sao::legacy_core
    sao::security::anti_screencap
    sao::rt_io
    sao::ui
    comctl32
    gdi32
    user32
)

set(_SAO_AI_EDITOR_GPU_HUNT_TEST_SOURCES
    gpu_hunt_sdk_adapter_test.cpp
    ../src/gpu_hunt_sdk_adapter.cpp
    ../src/gpu_hunt_tool.cpp
)
set(_SAO_AI_EDITOR_GPU_HUNT_TESTS_AVAILABLE TRUE)
foreach (_sao_ai_editor_gpu_hunt_test_source IN LISTS _SAO_AI_EDITOR_GPU_HUNT_TEST_SOURCES)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sao_ai_editor_gpu_hunt_test_source}")
        set(_SAO_AI_EDITOR_GPU_HUNT_TESTS_AVAILABLE FALSE)
    endif()
endforeach()
if (_SAO_AI_EDITOR_GPU_HUNT_TESTS_AVAILABLE)
    add_executable(sao_plugin_ai_editor_gpu_hunt_tests
        gpu_hunt_sdk_adapter_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_sdk_adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu_hunt_tool.cpp
    )
    target_compile_features(sao_plugin_ai_editor_gpu_hunt_tests PRIVATE cxx_std_20)
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
        sao::legacy_core
        sao::security::anti_screencap
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
add_test(NAME plugins_ai_editor
         COMMAND sao_plugin_ai_editor_tests --reporter compact)
set_tests_properties(plugins_ai_editor PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "plugins;ai_editor"
    TIMEOUT 30)

add_test(NAME plugins_ai_editor.production_child
         COMMAND sao_plugin_ai_editor_production_child_tests
                 --reporter compact)
set_tests_properties(plugins_ai_editor.production_child PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "plugins;ai_editor;integration;production_child"
    TIMEOUT 15)
add_test(NAME plugins_ai_editor.capture
         COMMAND sao_plugin_ai_editor_capture_tests --reporter compact)
set_tests_properties(plugins_ai_editor.capture PROPERTIES
    LABELS "plugins;ai_editor;capture"
    TIMEOUT 15)
if (_SAO_AI_EDITOR_GPU_HUNT_TESTS_AVAILABLE)
    add_test(NAME plugins_ai_editor.gpu_hunt
             COMMAND sao_plugin_ai_editor_gpu_hunt_tests --reporter compact)
    set_tests_properties(plugins_ai_editor.gpu_hunt PROPERTIES
        LABELS "plugins;ai_editor;gpu_hunt"
        TIMEOUT 15)
endif()
add_executable(sao_plugin_ai_editor_kernel_map_tests
    kernel_map_bridge_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/kernel_map_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/kernel_map_commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/kernel_map_tools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_tool_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/scope_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_secret_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tool_result_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sha256_helper.cpp
)
target_compile_features(sao_plugin_ai_editor_kernel_map_tests PRIVATE
    cxx_std_20
)
target_compile_definitions(sao_plugin_ai_editor_kernel_map_tests PRIVATE
    SAO_AI_EDITOR_TESTING
)
target_include_directories(sao_plugin_ai_editor_kernel_map_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(sao_plugin_ai_editor_kernel_map_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    sao::rt_io
    bcrypt
    crypt32
)
add_test(NAME plugins_ai_editor.kernel_map COMMAND sao_plugin_ai_editor_kernel_map_tests)
set_tests_properties(plugins_ai_editor.kernel_map PROPERTIES
    LABELS "plugins;ai_editor;kernel_map"
    TIMEOUT 15)
