# plugins/lua_host — Lua 5.4 真 C API 嵌入
if (NOT SAO_PLUGINS_ENABLE_LUA)
    return()
endif()

add_library(sao_plugins_lua_host STATIC
    src/lua_host.cpp
    src/lua_host_runtime.cpp
    src/lua_state_internal.cpp
    src/lua_loader_adapter.cpp
    src/lua_module_bridge.cpp
    src/lua_call.cpp
    src/lua_sandbox.cpp
    src/lua_sandbox_runtime.cpp
    src/lua_stdlib.cpp
    src/lua_error.cpp
)
add_library(sao::plugins::lua_host ALIAS sao_plugins_lua_host)

target_include_directories(sao_plugins_lua_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
)

# 沙箱敏感字符串使用 SAO_ENC_STR (header-only, 无需链接)
target_include_directories(sao_plugins_lua_host PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>
)

target_compile_features(sao_plugins_lua_host PUBLIC cxx_std_20)
target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
if (MSVC)
    target_compile_definitions(sao_plugins_lua_host PRIVATE
        _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()

target_link_libraries(sao_plugins_lua_host PUBLIC
    sao_plugins_loader
)

# vcpkg / 系统 Lua 5.4。禁止在配置阶段联网下载运行时。
option(SAO_PLUGINS_LUA_REQUIRE_RUNTIME
       "Fail configure when the Lua 5.4 runtime is unavailable" OFF)
find_package(Lua 5.4 QUIET)
if (NOT Lua_FOUND)
    find_package(lua 5.4 QUIET CONFIG)
endif()
if (TARGET Lua::Lua)
    target_link_libraries(sao_plugins_lua_host PRIVATE Lua::Lua)
    target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
    message(STATUS "plugins/lua_host: using Lua::Lua (SAO_HAS_LUA=1)")
elseif (TARGET lua::lua)
    target_link_libraries(sao_plugins_lua_host PRIVATE lua::lua)
    target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
    message(STATUS "plugins/lua_host: using lua::lua (SAO_HAS_LUA=1)")
elseif (Lua_FOUND OR LUA_FOUND)
    if (LUA_INCLUDE_DIR)
        target_include_directories(sao_plugins_lua_host PRIVATE ${LUA_INCLUDE_DIR})
    elseif (LUA_INCLUDE_DIRS)
        target_include_directories(sao_plugins_lua_host PRIVATE ${LUA_INCLUDE_DIRS})
    endif()
    if (LUA_LIBRARIES)
        target_link_libraries(sao_plugins_lua_host PRIVATE ${LUA_LIBRARIES})
    endif()
    target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
    message(STATUS "plugins/lua_host: using system/vcpkg Lua 5.4 (SAO_HAS_LUA=1)")
elseif (SAO_PLUGINS_LUA_REQUIRE_RUNTIME)
    message(FATAL_ERROR
        "plugins/lua_host: Lua 5.4 is required but no system/vcpkg package was found")
else()
    message(STATUS
        "plugins/lua_host: Lua 5.4 unavailable; runtime APIs are fail-closed stubs")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
