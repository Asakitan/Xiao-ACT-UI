# AI Editor real subprocess and named-pipe integration tests.

find_package(Catch2 3 CONFIG REQUIRED)

add_executable(sao_plugin_ai_editor_child_fixture
    ai_editor_child_fixture.cpp
)
target_compile_features(sao_plugin_ai_editor_child_fixture PRIVATE cxx_std_20)

add_executable(sao_plugin_ai_editor_wave7_tests
    ai_editor_process_test.cpp
)

target_link_libraries(sao_plugin_ai_editor_wave7_tests PRIVATE
    sao::plugin_ai_editor
    Catch2::Catch2WithMain
)

target_compile_features(sao_plugin_ai_editor_wave7_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_plugin_ai_editor_wave7_tests PRIVATE
    SAO_AI_EDITOR_CHILD_FIXTURE="$<TARGET_FILE:sao_plugin_ai_editor_child_fixture>"
)
add_dependencies(sao_plugin_ai_editor_wave7_tests
    sao_plugin_ai_editor_child_fixture
)

include(CTest)
include(Catch)
catch_discover_tests(sao_plugin_ai_editor_wave7_tests
    TEST_PREFIX "plugins_ai_editor."
    PROPERTIES LABELS "plugins;ai_editor"
)
