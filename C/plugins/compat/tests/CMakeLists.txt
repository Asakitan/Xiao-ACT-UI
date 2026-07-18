add_executable(test_plugins_compat test_compat.cpp)
target_link_libraries(test_plugins_compat PRIVATE sao_plugins_compat)
target_compile_features(test_plugins_compat PRIVATE cxx_std_17)
# sao_plugins_compat is STATIC but the public API is dllimport-decorated
# unless SAO_PLUGINS_BUILDING_DLL is defined.  Matches the wave4 pattern.
target_compile_definitions(test_plugins_compat PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_smoke COMMAND test_plugins_compat)
set_tests_properties(plugins_compat_smoke PROPERTIES LABELS "plugins;compat")

# Wave2: 真实老 Python 平台 plugin.json 兼容层单测
add_executable(test_plugins_compat_py_v1_manifest_wave2 test_py_v1_manifest_wave2.cpp)
target_link_libraries(test_plugins_compat_py_v1_manifest_wave2 PRIVATE sao_plugins_compat)
target_compile_features(test_plugins_compat_py_v1_manifest_wave2 PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_compat_py_v1_manifest_wave2 PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_py_v1_manifest_wave2
         COMMAND test_plugins_compat_py_v1_manifest_wave2)
set_tests_properties(plugins_compat_py_v1_manifest_wave2 PROPERTIES LABELS "plugins;manifest")

# Wave 4: runtime shim (ctx alias + libs/vendor probe + format) 单测
add_executable(test_plugins_compat_runtime_wave4 test_compat_runtime_wave4.cpp)
target_link_libraries(test_plugins_compat_runtime_wave4 PRIVATE sao_plugins_compat)
target_link_libraries(test_plugins_compat_runtime_wave4 PRIVATE sao::sdk)
target_compile_features(test_plugins_compat_runtime_wave4 PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_compat_runtime_wave4 PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_runtime_wave4
         COMMAND test_plugins_compat_runtime_wave4)
set_tests_properties(plugins_compat_runtime_wave4 PROPERTIES LABELS "plugins;compat")

add_executable(test_plugins_compat_v1_ctx_provider
    test_py_v1_ctx_shim_provider.cpp)
target_link_libraries(test_plugins_compat_v1_ctx_provider PRIVATE
    sao_plugins_compat
    sao::sdk)
target_compile_features(test_plugins_compat_v1_ctx_provider PRIVATE cxx_std_20)
target_compile_definitions(test_plugins_compat_v1_ctx_provider PRIVATE
    SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_v1_ctx_provider
         COMMAND test_plugins_compat_v1_ctx_provider)
set_tests_properties(plugins_compat_v1_ctx_provider
    PROPERTIES LABELS "plugins;compat;provider;lifecycle")

# Wave 8 / Agent d Phase 8: 磁盘 fixture (docs/fixtures/plugin_manifest/*.json)
# 13 case parity 校验 parser 逐字段对齐 Python 侧规范化结果.
add_executable(test_manifest_fixture_parity_wave8 test_manifest_fixture_parity_wave8.cpp)
target_link_libraries(test_manifest_fixture_parity_wave8 PRIVATE sao_plugins_compat)
target_compile_features(test_manifest_fixture_parity_wave8 PRIVATE cxx_std_17)
target_compile_definitions(test_manifest_fixture_parity_wave8 PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    SAO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/docs/fixtures/plugin_manifest")
add_test(NAME plugins_compat_manifest_fixture_parity_wave8
         COMMAND test_manifest_fixture_parity_wave8)
set_tests_properties(plugins_compat_manifest_fixture_parity_wave8
    PROPERTIES LABELS "plugins;manifest;fixture_parity;wave8")

find_package(Catch2 3 CONFIG REQUIRED)
add_executable(test_plugins_compat_shims_focused
    test_compat_shims_focused.cpp)
target_link_libraries(test_plugins_compat_shims_focused PRIVATE
    sao_plugins_compat
    Catch2::Catch2WithMain)
target_compile_features(test_plugins_compat_shims_focused PRIVATE cxx_std_20)
target_compile_definitions(test_plugins_compat_shims_focused PRIVATE
    SAO_PLUGINS_BUILDING_DLL)
if (MSVC)
    target_compile_options(test_plugins_compat_shims_focused PRIVATE /GR)
endif()
include(Catch)
catch_discover_tests(test_plugins_compat_shims_focused
    TEST_PREFIX "plugins_compat_shims."
    PROPERTIES LABELS "plugins;compat;focused")
