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
    src/lua_sandbox_runtime.cpp
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
    sao::plugins::script_ctx
    sao_plugins_sdk_binding
    sao::sdk
)

# Lua 5.4 is resolved only from vcpkg or another preprovisioned package.
option(SAO_PLUGINS_LUA_REQUIRE_RUNTIME
       "Fail configure when the Lua 5.4 runtime is unavailable" OFF)
option(SAO_PLUGINS_LUA_AUTO_FETCH
       "Legacy compatibility switch; runtime downloads are disabled." OFF)
if (SAO_PLUGINS_LUA_AUTO_FETCH)
    message(WARNING
        "SAO_PLUGINS_LUA_AUTO_FETCH is deprecated and ignored; Lua resolution remains offline.")
    set(SAO_PLUGINS_LUA_AUTO_FETCH OFF)
    set(SAO_PLUGINS_LUA_AUTO_FETCH OFF CACHE BOOL
        "Legacy compatibility switch; runtime downloads are disabled." FORCE)
endif()

# Vendored upstream Lua 5.4 is the first-class resolution path: the C++
# tree owns the base interpreter so lua_host works without any external
# runtime package.  SAO_LUA_SOURCE_DIR can point at another Lua source
# root; find_package below remains the fallback for custom installs.
set(SAO_LUA_SOURCE_DIR "" CACHE PATH
    "Lua 5.4 source root containing src/lua.h (defaults to the vendored copy)")
if (NOT SAO_LUA_SOURCE_DIR AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua/src/lua.h")
    set(SAO_LUA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua")
endif()

set(_sao_lua_resolved OFF)
if (SAO_LUA_SOURCE_DIR AND EXISTS "${SAO_LUA_SOURCE_DIR}/src/lua.h")
    file(GLOB _sao_lua_sources CONFIGURE_DEPENDS "${SAO_LUA_SOURCE_DIR}/src/*.c")
    list(REMOVE_ITEM _sao_lua_sources
        "${SAO_LUA_SOURCE_DIR}/src/lua.c"
        "${SAO_LUA_SOURCE_DIR}/src/luac.c"
        "${SAO_LUA_SOURCE_DIR}/src/onelua.c")
    if (_sao_lua_sources)
        add_library(sao_lua_vendored STATIC ${_sao_lua_sources})
        target_include_directories(sao_lua_vendored PUBLIC "${SAO_LUA_SOURCE_DIR}/src")
        if (MSVC)
            target_compile_definitions(sao_lua_vendored PRIVATE _CRT_SECURE_NO_WARNINGS)
            # Vendored upstream source: suppress its inherent narrowing / padding /
            # /W4 noise instead of patching third-party .c files.
            target_compile_options(sao_lua_vendored PRIVATE
                /wd4244 /wd4310 /wd4324 /wd4701 /wd4702)
        endif()
        target_link_libraries(sao_plugins_lua_host PRIVATE sao_lua_vendored)
        target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
        set(_sao_lua_resolved ON)
        message(STATUS "plugins/lua_host: vendored Lua sources (SAO_HAS_LUA=1)")
    else()
        message(WARNING "plugins/lua_host: SAO_LUA_SOURCE_DIR has no sources; falling back to find_package")
    endif()
endif()

if (NOT _sao_lua_resolved)
    find_package(Lua 5.4 QUIET)
    if (NOT Lua_FOUND)
        find_package(lua 5.4 QUIET CONFIG)
    endif()
endif()

if (TARGET Lua::Lua)
    target_link_libraries(sao_plugins_lua_host PRIVATE Lua::Lua)
    target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
    message(STATUS "plugins/lua_host: using Lua::Lua (SAO_HAS_LUA=1)")
    set(_sao_lua_resolved ON)
elseif (TARGET lua::lua)
    target_link_libraries(sao_plugins_lua_host PRIVATE lua::lua)
    target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
    message(STATUS "plugins/lua_host: using lua::lua (SAO_HAS_LUA=1)")
    set(_sao_lua_resolved ON)
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
    set(_sao_lua_resolved ON)
endif()

if (NOT _sao_lua_resolved)
    if (SAO_PLUGINS_LUA_REQUIRE_RUNTIME)
        message(FATAL_ERROR
            "plugins/lua_host: Lua 5.4 is required but no system/vcpkg/upstream package was found")
    else()
        message(STATUS
            "plugins/lua_host: Lua 5.4 unavailable; runtime APIs are fail-closed stubs")
    endif()
endif()

