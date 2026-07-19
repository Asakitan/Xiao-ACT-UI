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
    add_executable(test_plugins_csharp_host_contracts test_cs_contracts.cpp)
    target_link_libraries(test_plugins_csharp_host_contracts PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_include_directories(test_plugins_csharp_host_contracts PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_features(test_plugins_csharp_host_contracts PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_host_contracts PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_csharp_host_contracts PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_host_contracts
             COMMAND test_plugins_csharp_host_contracts)
    set_tests_properties(plugins_csharp_host_contracts PROPERTIES
        LABELS "plugins;csharp;contract;no_runtime")

    add_executable(test_plugins_csharp_sdk_bridge test_cs_sdk_bridge.cpp)
    target_link_libraries(test_plugins_csharp_sdk_bridge PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_include_directories(test_plugins_csharp_sdk_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_features(test_plugins_csharp_sdk_bridge PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_sdk_bridge PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_csharp_sdk_bridge PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_sdk_bridge COMMAND test_plugins_csharp_sdk_bridge)
    set_tests_properties(plugins_csharp_sdk_bridge PROPERTIES
        LABELS "plugins;csharp;bridge;no_runtime")

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

    # Wave 18 / Agent a: cs_sandbox 真实装 (ALC policy 记账 + hostfxr 降级).
    # 无 .NET SDK 也能跑 (mock hostfxr 探测).
    add_executable(test_plugins_csharp_host_wave18a test_cs_sandbox_wave18a.cpp)
    target_link_libraries(test_plugins_csharp_host_wave18a PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_compile_features(test_plugins_csharp_host_wave18a PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_host_wave18a PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_csharp_host_wave18a PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_host_wave18a
             COMMAND test_plugins_csharp_host_wave18a)
    set_tests_properties(plugins_csharp_host_wave18a
        PROPERTIES LABELS "plugins;csharp;wave18a;sandbox")

    add_executable(test_plugins_csharp_loader_adapter
        test_cs_loader_adapter.cpp)
    target_link_libraries(test_plugins_csharp_loader_adapter PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_compile_features(test_plugins_csharp_loader_adapter PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_loader_adapter PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        SAO_HELLO_CSHARP_DIR="${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp")
    if (MSVC)
        target_compile_options(test_plugins_csharp_loader_adapter PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_loader_adapter
             COMMAND test_plugins_csharp_loader_adapter)
    set_tests_properties(plugins_csharp_loader_adapter PROPERTIES
        LABELS "plugins;csharp;generic;requires_dotnet_fixture")
endif()
