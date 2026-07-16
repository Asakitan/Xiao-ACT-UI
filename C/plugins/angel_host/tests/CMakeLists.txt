# plugins/angel_host/tests — smoke + Wave 3 Catch2 单测

add_executable(test_plugins_angel_host test_angel.cpp)
target_link_libraries(test_plugins_angel_host PRIVATE sao_plugins_angel_host)
target_compile_features(test_plugins_angel_host PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_angel_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_angel_host_smoke COMMAND test_plugins_angel_host)
set_tests_properties(plugins_angel_host_smoke PROPERTIES LABELS "plugins;angel")

find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_angel_host_wave3 test_as_wave3.cpp)
    target_link_libraries(test_plugins_angel_host_wave3 PRIVATE
        sao_plugins_angel_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_wave3 PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_angel_host_wave3 PRIVATE SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_angel_host_wave3 PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_wave3 COMMAND test_plugins_angel_host_wave3)

    # Wave 8 / Agent d Phase 8: hello_angel 真加载 + on_load/tick/unload
    add_executable(test_plugins_angel_host_wave8 test_hello_angel_wave8.cpp)
    target_link_libraries(test_plugins_angel_host_wave8 PRIVATE
        sao_plugins_angel_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_angel_host_wave8 PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_angel_host_wave8 PRIVATE
        SAO_PLUGINS_BUILDING_DLL  # 静态库 + 头 dllimport → 加此定义清 __imp_ 前缀
        SAO_HELLO_ANGEL_JSON="${CMAKE_SOURCE_DIR}/plugins/examples/hello_angel/plugin.json")
    if (MSVC)
        target_compile_options(test_plugins_angel_host_wave8 PRIVATE /GR)
    endif()
    add_test(NAME plugins_angel_host_wave8 COMMAND test_plugins_angel_host_wave8)
    set_tests_properties(plugins_angel_host_wave8
        PROPERTIES LABELS "plugins;angel;wave8")
else()
    message(STATUS "plugins/angel_host: Catch2 未找到, 跳过 wave3/wave8 单测")
endif()
