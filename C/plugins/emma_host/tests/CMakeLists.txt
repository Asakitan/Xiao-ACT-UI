# plugins/emma_host/tests — smoke + Wave 3 Catch2 单测

# 老 smoke test (轻量, 无 Catch2 依赖)
add_executable(test_plugins_emma_host test_emma.cpp)
target_link_libraries(test_plugins_emma_host PRIVATE sao_plugins_emma_host)
target_compile_features(test_plugins_emma_host PRIVATE cxx_std_17)
# sao_plugins_emma_host is STATIC; the API surface is dllimport-decorated
# unless SAO_PLUGINS_BUILDING_DLL is defined.
target_compile_definitions(test_plugins_emma_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_emma_host_smoke COMMAND test_plugins_emma_host)

# Wave 3 tree-walk 功能验证 (Catch2)
find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_emma_host_wave3 test_emma_wave3.cpp)
    target_link_libraries(test_plugins_emma_host_wave3 PRIVATE
        sao_plugins_emma_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_emma_host_wave3 PRIVATE cxx_std_17)
    # Match the smoke test: sao_plugins_emma_host is STATIC.
    target_compile_definitions(test_plugins_emma_host_wave3 PRIVATE SAO_PLUGINS_BUILDING_DLL)
    # Catch2 需要 RTTI (顶层 /GR- 关掉了, 这里 opt-in)
    if (MSVC)
        target_compile_options(test_plugins_emma_host_wave3 PRIVATE /GR)
    endif()
    add_test(NAME plugins_emma_host_wave3 COMMAND test_plugins_emma_host_wave3)

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
else()
    message(STATUS "plugins/emma_host: Catch2 未找到, 跳过 wave3 单测")
endif()
