# Shared helper that stamps DOTNET_ROOT / PATH / SAO_CSHARP_HOSTFXR_DIR on
# every test so a build-tree-fetched runtime is discoverable without needing
# a system-wide .NET install.  The values are propagated verbatim from the
# csharp_host CMakeLists resolution result and default to empty strings when
# no runtime was resolved (in which case the tests fall back to their
# existing PATH-based LoadLibrary("hostfxr") probe).
set(_sao_csharp_test_env "")
if (SAO_CSHARP_DOTNET_ROOT)
    list(APPEND _sao_csharp_test_env
        "DOTNET_ROOT=${SAO_CSHARP_DOTNET_ROOT}")
endif()
if (SAO_CSHARP_HOSTFXR_DIR)
    list(APPEND _sao_csharp_test_env
        "SAO_CSHARP_HOSTFXR_DIR=${SAO_CSHARP_HOSTFXR_DIR}")
    if (WIN32)
        # ctest merges list items with ';' which is also the PATH separator
        # on Windows, so we splice the fetched host directory onto whatever
        # PATH the ctest harness inherits.  The extra literal ';' delimiter
        # is required because CMake would otherwise merge the two segments.
        list(APPEND _sao_csharp_test_env
            "PATH=${SAO_CSHARP_HOSTFXR_DIR};$ENV{PATH}")
    endif()
endif()

function(_sao_csharp_apply_test_env test_name)
    if (_sao_csharp_test_env)
        set_property(TEST ${test_name} APPEND PROPERTY
            ENVIRONMENT "${_sao_csharp_test_env}")
    endif()
endfunction()

add_executable(test_plugins_csharp_host test_cs.cpp)
target_link_libraries(test_plugins_csharp_host PRIVATE sao_plugins_csharp_host)
target_compile_features(test_plugins_csharp_host PRIVATE cxx_std_17)
# 静态库消费方需要清 dllimport 前缀 (对齐 host tests).
target_compile_definitions(test_plugins_csharp_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_csharp_host_smoke COMMAND test_plugins_csharp_host)
set_tests_properties(plugins_csharp_host_smoke PROPERTIES LABELS "plugins;csharp")
_sao_csharp_apply_test_env(plugins_csharp_host_smoke)

# hostfxr 可用性与保留编译接口单测
add_executable(test_plugins_csharp_host_cs_host test_cs_host.cpp)
target_link_libraries(test_plugins_csharp_host_cs_host PRIVATE sao_plugins_csharp_host)
target_compile_features(test_plugins_csharp_host_cs_host PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_csharp_host_cs_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_csharp_host_cs_host
         COMMAND test_plugins_csharp_host_cs_host)
set_tests_properties(plugins_csharp_host_cs_host PROPERTIES LABELS "plugins;csharp")
_sao_csharp_apply_test_env(plugins_csharp_host_cs_host)

# hello_csharp 真加载 + OnLoad/OnTick/OnUnload
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
    _sao_csharp_apply_test_env(plugins_csharp_host_contracts)

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
    _sao_csharp_apply_test_env(plugins_csharp_sdk_bridge)

    add_executable(test_plugins_csharp_host_hello test_hello_csharp.cpp)
    target_link_libraries(test_plugins_csharp_host_hello PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_compile_features(test_plugins_csharp_host_hello PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_host_hello PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        SAO_HELLO_CSHARP_JSON="${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp/plugin.json"
        SAO_HELLO_CSHARP_DLL="${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp/prebuilt/HelloPlugin.dll")
    if (MSVC)
        target_compile_options(test_plugins_csharp_host_hello PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_host_hello COMMAND test_plugins_csharp_host_hello)
    set_tests_properties(plugins_csharp_host_hello
        PROPERTIES LABELS "plugins;csharp;lifecycle;requires_dotnet_sdk")
    _sao_csharp_apply_test_env(plugins_csharp_host_hello)

    # cs_sandbox 真实装 (ALC policy 记账 + hostfxr 降级).
    # 无 .NET SDK 也能跑 (mock hostfxr 探测).
    add_executable(test_plugins_csharp_host_sandbox test_cs_sandbox.cpp)
    target_link_libraries(test_plugins_csharp_host_sandbox PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_compile_features(test_plugins_csharp_host_sandbox PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_host_sandbox PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_csharp_host_sandbox PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_host_sandbox
             COMMAND test_plugins_csharp_host_sandbox)
    set_tests_properties(plugins_csharp_host_sandbox
        PROPERTIES LABELS "plugins;csharp;sandbox")
    _sao_csharp_apply_test_env(plugins_csharp_host_sandbox)

    add_executable(test_plugins_csharp_loader_adapter
        test_cs_loader_adapter.cpp)
    target_link_libraries(test_plugins_csharp_loader_adapter PRIVATE
        sao_plugins_csharp_host
        Catch2::Catch2WithMain)
    target_compile_features(test_plugins_csharp_loader_adapter PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_csharp_loader_adapter PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        SAO_HELLO_CSHARP_DIR="${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp")
    find_program(SAO_CSHARP_DOTNET_EXECUTABLE NAMES dotnet)
    if (SAO_CSHARP_DOTNET_EXECUTABLE)
        set(_sao_managed_entity_fixture_root
            "${CMAKE_CURRENT_BINARY_DIR}/managed_entity_fixture")
        set(_sao_managed_entity_fixture_a
            "${_sao_managed_entity_fixture_root}/entity_a")
        set(_sao_managed_entity_fixture_b
            "${_sao_managed_entity_fixture_root}/entity_b")
        set(_sao_managed_entity_project
            "${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp/HelloPlugin.csproj")
        set(_sao_managed_entity_source
            "${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp/HelloPlugin.cs")
        set(_sao_managed_entity_a_dll
            "${_sao_managed_entity_fixture_a}/HelloPluginEntityA.dll")
        set(_sao_managed_entity_b_dll
            "${_sao_managed_entity_fixture_b}/HelloPluginEntityB.dll")
        add_custom_command(
            OUTPUT
                "${_sao_managed_entity_a_dll}"
                "${_sao_managed_entity_fixture_a}/HelloPluginEntityA.runtimeconfig.json"
                "${_sao_managed_entity_b_dll}"
                "${_sao_managed_entity_fixture_b}/HelloPluginEntityB.runtimeconfig.json"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_sao_managed_entity_fixture_a}"
                    "${_sao_managed_entity_fixture_b}"
            COMMAND "${SAO_CSHARP_DOTNET_EXECUTABLE}" build
                    "${_sao_managed_entity_project}"
                    --configuration Release
                    "-p:AssemblyName=HelloPluginEntityA"
                    "-p:OutputPath=${_sao_managed_entity_fixture_a}/"
                    "-p:BaseIntermediateOutputPath=${_sao_managed_entity_fixture_root}/obj/entity_a/"
                    "-p:DefineConstants=SAO_MANAGED_ENTITY_FIXTURE"
            COMMAND "${SAO_CSHARP_DOTNET_EXECUTABLE}" build
                    "${_sao_managed_entity_project}"
                    --configuration Release
                    "-p:AssemblyName=HelloPluginEntityB"
                    "-p:OutputPath=${_sao_managed_entity_fixture_b}/"
                    "-p:BaseIntermediateOutputPath=${_sao_managed_entity_fixture_root}/obj/entity_b/"
                    "-p:DefineConstants=SAO_MANAGED_ENTITY_FIXTURE"
            DEPENDS
                "${_sao_managed_entity_project}"
                "${_sao_managed_entity_source}"
            WORKING_DIRECTORY
                "${CMAKE_SOURCE_DIR}/plugins/examples/hello_csharp"
            VERBATIM)
        add_custom_target(sao_csharp_managed_entity_fixture
            DEPENDS "${_sao_managed_entity_a_dll}" "${_sao_managed_entity_b_dll}")
        add_dependencies(test_plugins_csharp_loader_adapter
            sao_csharp_managed_entity_fixture)
        target_compile_definitions(test_plugins_csharp_loader_adapter PRIVATE
            SAO_MANAGED_ENTITY_A_DIR="${_sao_managed_entity_fixture_a}"
            SAO_MANAGED_ENTITY_B_DIR="${_sao_managed_entity_fixture_b}")
    endif()
    if (MSVC)
        target_compile_options(test_plugins_csharp_loader_adapter PRIVATE /GR /EHsc)
    endif()
    add_test(NAME plugins_csharp_loader_adapter
             COMMAND test_plugins_csharp_loader_adapter)
    set_tests_properties(plugins_csharp_loader_adapter PROPERTIES
        LABELS "plugins;csharp;generic;requires_dotnet_fixture"
        RESOURCE_LOCK "sao_launcher_process_resources")
    _sao_csharp_apply_test_env(plugins_csharp_loader_adapter)

    add_test(NAME plugins_csharp_managed_entity_v2
             COMMAND test_plugins_csharp_loader_adapter
                     "csharp managed Entity v2 provider preserves lifecycle token stride and generations")
    set_tests_properties(plugins_csharp_managed_entity_v2 PROPERTIES
        LABELS "plugins;csharp;generic;entity;v2;focused;requires_dotnet_fixture"
        RESOURCE_LOCK "sao_launcher_process_resources")
    _sao_csharp_apply_test_env(plugins_csharp_managed_entity_v2)
endif()
