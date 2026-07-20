add_executable(test_plugins_compat test_compat.cpp)
target_link_libraries(test_plugins_compat PRIVATE sao_plugins_compat)
target_compile_features(test_plugins_compat PRIVATE cxx_std_17)
# sao_plugins_compat is STATIC but the public API is dllimport-decorated
# unless SAO_PLUGINS_BUILDING_DLL is defined. Match the static-library consumer pattern.
target_compile_definitions(test_plugins_compat PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_smoke COMMAND test_plugins_compat)
set_tests_properties(plugins_compat_smoke PROPERTIES LABELS "plugins;compat")

# 真实老 Python 平台 plugin.json 兼容层单测
add_executable(test_plugins_compat_py_v1_manifest test_py_v1_manifest.cpp)
target_link_libraries(test_plugins_compat_py_v1_manifest PRIVATE sao_plugins_compat)
target_compile_features(test_plugins_compat_py_v1_manifest PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_compat_py_v1_manifest PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_py_v1_manifest
         COMMAND test_plugins_compat_py_v1_manifest)
set_tests_properties(plugins_compat_py_v1_manifest PROPERTIES LABELS "plugins;manifest")

# runtime shim (ctx alias + libs/vendor probe + format) 单测
add_executable(test_plugins_compat_runtime test_compat_runtime.cpp)
target_link_libraries(test_plugins_compat_runtime PRIVATE sao_plugins_compat)
target_link_libraries(test_plugins_compat_runtime PRIVATE sao::sdk)
target_compile_features(test_plugins_compat_runtime PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_compat_runtime PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_compat_runtime
         COMMAND test_plugins_compat_runtime)
set_tests_properties(plugins_compat_runtime PROPERTIES LABELS "plugins;compat")

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

# 磁盘 fixture (docs/fixtures/plugin_manifest/*.json)
# 13 case parity 校验 parser 逐字段对齐 Python 侧规范化结果.
add_executable(test_manifest_fixture_parity test_manifest_fixture_parity.cpp)
target_link_libraries(test_manifest_fixture_parity PRIVATE sao_plugins_compat)
target_compile_features(test_manifest_fixture_parity PRIVATE cxx_std_17)
target_compile_definitions(test_manifest_fixture_parity PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    SAO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/docs/fixtures/plugin_manifest")
add_test(NAME plugins_compat_manifest_fixture_parity
         COMMAND test_manifest_fixture_parity)
set_tests_properties(plugins_compat_manifest_fixture_parity
    PROPERTIES LABELS "plugins;manifest;fixture_parity;lifecycle")

if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()
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
