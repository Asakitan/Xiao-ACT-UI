# AI Editor real subprocess and named-pipe integration tests.

find_package(Catch2 3 CONFIG REQUIRED)

add_executable(sao_plugin_ai_editor_child_fixture
    ai_editor_child_fixture.cpp
)
target_compile_features(sao_plugin_ai_editor_child_fixture PRIVATE cxx_std_20)

add_executable(sao_plugin_ai_editor_wave7_tests
    ai_editor_native_test.cpp
    ai_editor_process_test.cpp
)

target_link_libraries(sao_plugin_ai_editor_wave7_tests PRIVATE
    sao::plugin_ai_editor
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    ws2_32
)

target_compile_features(sao_plugin_ai_editor_wave7_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_plugin_ai_editor_wave7_tests PRIVATE
    SAO_AI_EDITOR_CHILD_FIXTURE="$<TARGET_FILE:sao_plugin_ai_editor_child_fixture>"
    SAO_AI_EDITOR_MCP_SERVER_EXECUTABLE="$<TARGET_FILE:sao_ai_editor>"
)
add_dependencies(sao_plugin_ai_editor_wave7_tests
    sao_plugin_ai_editor_child_fixture
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
)
add_dependencies(sao_plugin_ai_editor_production_child_tests
    sao_ai_editor
)

include(CTest)
include(Catch)
catch_discover_tests(sao_plugin_ai_editor_wave7_tests
    TEST_PREFIX "plugins_ai_editor."
    PROPERTIES LABELS "plugins;ai_editor"
)
catch_discover_tests(sao_plugin_ai_editor_production_child_tests
    TEST_PREFIX "plugins_ai_editor.production_child."
    PROPERTIES LABELS "plugins;ai_editor;integration;production_child"
)
