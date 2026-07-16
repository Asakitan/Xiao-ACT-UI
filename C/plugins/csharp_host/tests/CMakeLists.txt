add_executable(test_plugins_csharp_host test_cs.cpp)
target_link_libraries(test_plugins_csharp_host PRIVATE sao_plugins_csharp_host)
target_compile_features(test_plugins_csharp_host PRIVATE cxx_std_17)
# 静态库消费方需要清 dllimport 前缀 (对齐 wave4/wave8 test).
target_compile_definitions(test_plugins_csharp_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_csharp_host_smoke COMMAND test_plugins_csharp_host)
set_tests_properties(plugins_csharp_host_smoke PROPERTIES LABELS "plugins;csharp")

# Wave 4: hostfxr + Roslyn stub 首切片单测
add_executable(test_plugins_csharp_host_wave4 test_cs_host_wave4.cpp)
target_link_libraries(test_plugins_csharp_host_wave4 PRIVATE sao_plugins_csharp_host)
target_compile_features(test_plugins_csharp_host_wave4 PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_csharp_host_wave4 PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_csharp_host_wave4
         COMMAND test_plugins_csharp_host_wave4)
set_tests_properties(plugins_csharp_host_wave4 PROPERTIES LABELS "plugins;csharp")

# Wave 8 / Agent d Phase 8: hello_csharp 真加载 + OnLoad/OnTick/OnUnload
find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_csharp_host_wave8 test_hello_csharp_wave8.cpp)
    target_link_libraries(test_plugins_csharp_host_wave8 PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_compile_features(test_plugins_csharp_host_wave8 PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_host_wave8 PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        SAO_HELLO_CSHARP_JSON="${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp/plugin.json"
        SAO_HELLO_CSHARP_DLL="${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp/prebuilt/HelloPlugin.dll")
    if (MSVC)
        target_compile_options(test_plugins_csharp_host_wave8 PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_host_wave8 COMMAND test_plugins_csharp_host_wave8)
    set_tests_properties(plugins_csharp_host_wave8
        PROPERTIES LABELS "plugins;csharp;wave8;requires_dotnet_sdk")
endif()
