find_package(Catch2 3 REQUIRED)

set(sao_plugins_test_sources test_plugins.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_legacy_logging.cpp")
    list(APPEND sao_plugins_test_sources test_legacy_logging.cpp)
endif()

# test_plugins.cpp always exercises the loader facade. When the optional full
# v1 aggregate exists it also covers the aggregate ABI and Lua runtime; without
# both legacy SDKs, link the facade objects directly to keep compatibility
# validation available in the normal plugins-only build.
if (TARGET sao_plugins_legacy)
    add_executable(sao_plugins_tests ${sao_plugins_test_sources})
    target_link_libraries(sao_plugins_tests PRIVATE sao_plugins_legacy Catch2::Catch2WithMain)
    # sao_plugins_legacy is SHARED and exports SAO_PLUGINS_API, so the
    # consumer must see the plain (undecorated) prototype through
    # __declspec(dllimport) — no BUILDING_DLL define here.
else()
    add_executable(sao_plugins_tests
        ${sao_plugins_test_sources}
        ../src/sao_plugins.cpp
        $<TARGET_OBJECTS:sao_plugins_legacy_loader_facade_objects>)
    target_link_libraries(sao_plugins_tests PRIVATE
        sao_plugins_loader
        Catch2::Catch2WithMain)
    target_compile_definitions(sao_plugins_tests PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        SAO_TEST_LEGACY_RUNTIME_FACADE_ONLY)
endif()

target_compile_features(sao_plugins_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_plugins_tests PRIVATE
    SAO_TEST_NATIVE_PLUGIN_PATH=L"$<TARGET_FILE:sao_loader_native_fixture>")
add_dependencies(sao_plugins_tests sao_loader_native_fixture)
add_test(NAME sao_plugins_tests COMMAND sao_plugins_tests)
