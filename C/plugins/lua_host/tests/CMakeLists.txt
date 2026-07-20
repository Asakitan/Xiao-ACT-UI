# plugins/lua_host/tests — smoke + interpreter Catch2 单测

add_executable(test_plugins_lua_host test_lua.cpp)
target_link_libraries(test_plugins_lua_host PRIVATE sao_plugins_lua_host)
target_compile_features(test_plugins_lua_host PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_lua_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_lua_host_smoke COMMAND test_plugins_lua_host)

find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_lua_host_interpreter
        test_lua_interpreter.cpp)
    target_link_libraries(test_plugins_lua_host_interpreter PRIVATE
        sao_plugins_lua_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_lua_host_interpreter PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_lua_host_interpreter PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_lua_host_interpreter PRIVATE /GR)
    endif()
    add_test(NAME plugins_lua_host_interpreter
             COMMAND test_plugins_lua_host_interpreter)

    if (TARGET Lua::Lua OR TARGET lua::lua OR Lua_FOUND OR LUA_FOUND)
        add_executable(test_plugins_lua_host_hardening
            test_lua_hardening.cpp)
        target_link_libraries(test_plugins_lua_host_hardening PRIVATE
            sao_plugins_lua_host
            Catch2::Catch2WithMain
        )
        if (TARGET Lua::Lua)
            target_link_libraries(test_plugins_lua_host_hardening PRIVATE Lua::Lua)
        elseif (TARGET lua::lua)
            target_link_libraries(test_plugins_lua_host_hardening PRIVATE lua::lua)
        elseif (LUA_INCLUDE_DIR)
            target_include_directories(test_plugins_lua_host_hardening PRIVATE
                ${LUA_INCLUDE_DIR})
        elseif (LUA_INCLUDE_DIRS)
            target_include_directories(test_plugins_lua_host_hardening PRIVATE
                ${LUA_INCLUDE_DIRS})
        endif()
        target_compile_features(test_plugins_lua_host_hardening PRIVATE cxx_std_20)
        target_compile_definitions(test_plugins_lua_host_hardening PRIVATE
            SAO_PLUGINS_BUILDING_DLL)
        if (MSVC)
            target_compile_options(test_plugins_lua_host_hardening PRIVATE /GR)
        endif()
        add_test(NAME plugins_lua_host_hardening
                 COMMAND test_plugins_lua_host_hardening)
        set_tests_properties(plugins_lua_host_hardening
            PROPERTIES LABELS "plugins;lua;hardening;concurrency;sandbox")
    else()
        message(STATUS
            "plugins/lua_host: Lua runtime unavailable, skipping hardening tests")
    endif()

    add_executable(test_plugins_lua_loader_adapter
        test_lua_loader_adapter.cpp)
    target_link_libraries(test_plugins_lua_loader_adapter PRIVATE
        sao_plugins_lua_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_lua_loader_adapter PRIVATE cxx_std_20)
    target_compile_definitions(test_plugins_lua_loader_adapter PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_lua_loader_adapter PRIVATE /GR)
    endif()
    add_test(NAME plugins_lua_loader_adapter
             COMMAND test_plugins_lua_loader_adapter)
    set_tests_properties(plugins_lua_loader_adapter
        PROPERTIES LABELS "plugins;lua;adapter;lifecycle;sandbox")

    # ── Lua sandbox 10 case ──────────────────────────────────────────
    add_executable(test_plugins_lua_host_sandbox
        test_lua_sandbox.cpp)
    target_link_libraries(test_plugins_lua_host_sandbox PRIVATE
        sao_plugins_lua_host
        sao_script_sandbox_common_lib
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_lua_host_sandbox PRIVATE cxx_std_20)
    target_compile_definitions(test_plugins_lua_host_sandbox PRIVATE
        # 静态 lib + header 用 dllimport → 手动加此定义清 __imp_ 前缀
        SAO_PLUGINS_BUILDING_DLL)
    if (MSVC)
        target_compile_options(test_plugins_lua_host_sandbox PRIVATE /GR)
    endif()
    add_test(NAME plugins_lua_host_sandbox
             COMMAND test_plugins_lua_host_sandbox)
    set_tests_properties(plugins_lua_host_sandbox
        PROPERTIES LABELS "plugins;lua;sandbox")
else()
    message(STATUS "plugins/lua_host: Catch2 未找到, 跳过 interpreter / sandbox 单测")
endif()
