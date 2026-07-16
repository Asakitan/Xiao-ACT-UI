# plugins/lua_host — Lua 5.4 真 C API 嵌入
if (NOT SAO_PLUGINS_ENABLE_LUA)
    return()
endif()

add_library(sao_plugins_lua_host STATIC
    src/lua_host.cpp
    src/lua_module_bridge.cpp
    src/lua_call.cpp
    src/lua_sandbox.cpp
    src/lua_stdlib.cpp
    src/lua_error.cpp
)
add_library(sao::plugins::lua_host ALIAS sao_plugins_lua_host)

target_include_directories(sao_plugins_lua_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

target_compile_features(sao_plugins_lua_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_lua_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
)

# vcpkg / 系统 Lua 5.4
# 优先 CONFIG (vcpkg 出), 回落 module 模式 (系统 pkg)
find_package(Lua 5.4 QUIET)
if (NOT Lua_FOUND)
    find_package(lua QUIET)
endif()
if (Lua_FOUND OR LUA_FOUND)
    target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
    if (LUA_INCLUDE_DIR)
        target_include_directories(sao_plugins_lua_host PRIVATE ${LUA_INCLUDE_DIR})
    endif()
    if (LUA_LIBRARIES)
        target_link_libraries(sao_plugins_lua_host PRIVATE ${LUA_LIBRARIES})
    endif()
    message(STATUS "plugins/lua_host: 用真 Lua 5.4 (SAO_HAS_LUA=1)")
else()
    message(STATUS "plugins/lua_host: Lua 未找到 —— gated stub, API 返 SAO_ERR_NOT_IMPLEMENTED")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
