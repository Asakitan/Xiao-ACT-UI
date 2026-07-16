# plugins/lua_host/tests — smoke + Wave 3 Catch2 单测

add_executable(test_plugins_lua_host test_lua.cpp)
target_link_libraries(test_plugins_lua_host PRIVATE sao_plugins_lua_host)
target_compile_features(test_plugins_lua_host PRIVATE cxx_std_17)
add_test(NAME plugins_lua_host_smoke COMMAND test_plugins_lua_host)

find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_lua_host_wave3 test_lua_wave3.cpp)
    target_link_libraries(test_plugins_lua_host_wave3 PRIVATE
        sao_plugins_lua_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_lua_host_wave3 PRIVATE cxx_std_17)
    if (MSVC)
        target_compile_options(test_plugins_lua_host_wave3 PRIVATE /GR)
    endif()
    add_test(NAME plugins_lua_host_wave3 COMMAND test_plugins_lua_host_wave3)
else()
    message(STATUS "plugins/lua_host: Catch2 未找到, 跳过 wave3 单测")
endif()
