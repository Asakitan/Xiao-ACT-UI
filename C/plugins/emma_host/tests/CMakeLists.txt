# plugins/emma_host/tests — interpreter Catch2 单测

# Tree-walk interpreter 功能验证 (Catch2)
find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_emma_host_interpreter
        test_emma_interpreter.cpp)
    target_link_libraries(test_plugins_emma_host_interpreter PRIVATE
        sao_plugins_emma_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_emma_host_interpreter PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_emma_host_interpreter PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    # Catch2 需要 RTTI (顶层 /GR- 关掉了, 这里 opt-in)
    if (MSVC)
        target_compile_options(test_plugins_emma_host_interpreter PRIVATE /GR)
    endif()
    add_test(NAME plugins_emma_host_interpreter
        COMMAND test_plugins_emma_host_interpreter)

    add_executable(test_plugins_emma_loader_adapter
        test_emma_loader_adapter.cpp)
    target_link_libraries(test_plugins_emma_loader_adapter PRIVATE
        sao_plugins_emma_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_emma_loader_adapter PRIVATE cxx_std_20)
    target_compile_definitions(test_plugins_emma_loader_adapter PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_emma_loader_adapter PRIVATE /GR)
    endif()
    add_test(NAME plugins_emma_loader_adapter
        COMMAND test_plugins_emma_loader_adapter)
    set_tests_properties(plugins_emma_loader_adapter
        PROPERTIES LABELS "plugins;emma;adapter;lifecycle")

    add_executable(test_plugins_emma_context_stdlib
        test_emma_context_stdlib.cpp)
    target_link_libraries(test_plugins_emma_context_stdlib PRIVATE
        sao_plugins_emma_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_emma_context_stdlib PRIVATE cxx_std_20)
    target_compile_definitions(test_plugins_emma_context_stdlib PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_emma_context_stdlib PRIVATE /GR)
    endif()
    add_test(NAME plugins_emma_context_stdlib
        COMMAND test_plugins_emma_context_stdlib)
else()
    message(STATUS "plugins/emma_host: Catch2 未找到, 跳过 interpreter 单测")
endif()

foreach(_sao_test IN ITEMS
    plugins_emma_host_interpreter
    plugins_emma_loader_adapter)
    if (TEST ${_sao_test})
        set_property(TEST ${_sao_test} PROPERTY TIMEOUT 15)
    endif()
endforeach()
if (TEST plugins_emma_context_stdlib)
    set_property(TEST plugins_emma_context_stdlib PROPERTY TIMEOUT 30)
endif()
