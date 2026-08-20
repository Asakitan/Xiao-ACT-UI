# plugins/angel_host/tests — AngelScript host Catch2 单测

find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_angel_host_as_host test_as_host.cpp)
    target_link_libraries(test_plugins_angel_host_as_host PRIVATE
        sao_plugins_angel_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_as_host PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_angel_host_as_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_angel_host_as_host PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_as_host COMMAND test_plugins_angel_host_as_host)

    add_executable(test_plugins_angel_host_bridge_stdlib
        test_as_bridge_stdlib.cpp)
    target_link_libraries(test_plugins_angel_host_bridge_stdlib PRIVATE
        sao_plugins_angel_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_bridge_stdlib PRIVATE
        cxx_std_17)
    target_compile_definitions(test_plugins_angel_host_bridge_stdlib PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (_sao_angel_target)
        target_link_libraries(test_plugins_angel_host_bridge_stdlib PRIVATE
            ${_sao_angel_target})
        target_compile_definitions(test_plugins_angel_host_bridge_stdlib PRIVATE
            SAO_TEST_HAS_ANGELSCRIPT=1)
    endif()
    if (_sao_angel_addons_enabled)
        target_compile_definitions(test_plugins_angel_host_bridge_stdlib PRIVATE
            SAO_TEST_HAS_ANGELSCRIPT_ADDONS=1)
    endif()
    if (SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME)
        target_compile_definitions(test_plugins_angel_host_bridge_stdlib PRIVATE
            SAO_TEST_ANGEL_RUNTIME_REQUIRED=1)
    endif()
    if (SAO_PLUGINS_ANGEL_FORCE_FALLBACK)
        target_compile_definitions(test_plugins_angel_host_bridge_stdlib PRIVATE
            SAO_TEST_ANGEL_FORCE_FALLBACK=1)
    endif()
    if (SAO_PLUGINS_ANGEL_MAX_PORTABILITY)
        target_compile_definitions(test_plugins_angel_host_bridge_stdlib PRIVATE
            AS_MAX_PORTABILITY=1)
    endif()
    if (MSVC)
        target_compile_options(test_plugins_angel_host_bridge_stdlib PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_bridge_stdlib
             COMMAND test_plugins_angel_host_bridge_stdlib)
    set_tests_properties(plugins_angel_host_bridge_stdlib
        PROPERTIES LABELS "plugins;angel;bridge;stdlib")

    # hello_angel 真加载 + on_load/tick/unload
    add_executable(test_plugins_angel_host_hello test_hello_angel.cpp)
    target_link_libraries(test_plugins_angel_host_hello PRIVATE
        sao_plugins_angel_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_hello PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_angel_host_hello PRIVATE
        SAO_PLUGINS_BUILDING_DLL  # 静态库 + 头 dllimport → 加此定义清 __imp_ 前缀
        SAO_HELLO_ANGEL_JSON="${CMAKE_SOURCE_DIR}/plugins/examples/hello_angel/plugin.json")
    if (MSVC)
        target_compile_options(test_plugins_angel_host_hello PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_hello COMMAND test_plugins_angel_host_hello)
    set_tests_properties(plugins_angel_host_hello
        PROPERTIES LABELS "plugins;angel;lifecycle")

    # gpu_hunt SDK binding on AngelScript engine.
    add_executable(test_plugins_angel_host_gpu_hunt_bind test_gpu_hunt_bind.cpp)
    target_link_libraries(test_plugins_angel_host_gpu_hunt_bind PRIVATE
        sao_plugins_angel_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_gpu_hunt_bind PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_angel_host_gpu_hunt_bind PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_angel_host_gpu_hunt_bind PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_gpu_hunt_bind
             COMMAND test_plugins_angel_host_gpu_hunt_bind)
    set_tests_properties(plugins_angel_host_gpu_hunt_bind
        PROPERTIES LABELS "plugins;angel;gpu_hunt")

    add_executable(test_plugins_angel_host_loader_adapter
        test_as_loader_adapter.cpp)
    target_link_libraries(test_plugins_angel_host_loader_adapter PRIVATE
        sao_plugins_angel_host
        sao::sdk
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_loader_adapter PRIVATE
        cxx_std_20)
    target_compile_definitions(test_plugins_angel_host_loader_adapter PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (_sao_angel_target)
        target_link_libraries(test_plugins_angel_host_loader_adapter PRIVATE
            ${_sao_angel_target})
        target_compile_definitions(test_plugins_angel_host_loader_adapter PRIVATE
            SAO_TEST_HAS_ANGELSCRIPT=1)
    endif()
    if (MSVC)
        target_compile_options(test_plugins_angel_host_loader_adapter PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_loader_adapter
             COMMAND test_plugins_angel_host_loader_adapter)
    set_tests_properties(plugins_angel_host_loader_adapter
        PROPERTIES LABELS "plugins;angel;loader_adapter")

    # ── AngelScript sandbox 8 case ──────────────
    add_executable(test_plugins_angel_host_sandbox
        test_as_sandbox.cpp)
    target_link_libraries(test_plugins_angel_host_sandbox PRIVATE
        sao_plugins_angel_host
        sao_script_sandbox_common_lib
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_sandbox PRIVATE cxx_std_20)
    target_compile_definitions(test_plugins_angel_host_sandbox PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_angel_host_sandbox PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_sandbox
             COMMAND test_plugins_angel_host_sandbox)
    set_tests_properties(plugins_angel_host_sandbox
        PROPERTIES LABELS "plugins;angel;sandbox")
else()
    message(STATUS "plugins/angel_host: Catch2 未找到, 跳过 host/lifecycle/sandbox 单测")
endif()

foreach(_sao_test IN ITEMS
    plugins_angel_host_as_host
    plugins_angel_host_bridge_stdlib
    plugins_angel_host_gpu_hunt_bind
    plugins_angel_host_loader_adapter)
    if (TEST ${_sao_test})
        set_property(TEST ${_sao_test} PROPERTY TIMEOUT 15)
    endif()
endforeach()
foreach(_sao_test IN ITEMS
    plugins_angel_host_hello
    plugins_angel_host_sandbox)
    if (TEST ${_sao_test})
        set_property(TEST ${_sao_test} PROPERTY TIMEOUT 30)
    endif()
endforeach()
