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

# Lua 5.4 resolution order:
#   1. find_package(Lua 5.4) / find_package(lua 5.4 CONFIG) (vcpkg / system)
#   2. FetchContent from lua.org when SAO_PLUGINS_LUA_AUTO_FETCH=ON
# Auto-fetch is opt-in so shipping / air-gapped builds stay reproducible.
option(SAO_PLUGINS_LUA_REQUIRE_RUNTIME
       "Fail configure when the Lua 5.4 runtime is unavailable" OFF)
option(SAO_PLUGINS_LUA_AUTO_FETCH
       "Download upstream Lua 5.4.8 source tarball on demand (FetchContent) when no runtime is found." OFF)
set(SAO_LUA_FETCH_URL
    "https://www.lua.org/ftp/lua-5.4.8.tar.gz"
    CACHE STRING "URL from which the Lua source tarball is fetched when auto-fetch is enabled")
set(SAO_LUA_FETCH_SHA256
    ""
    CACHE STRING "SHA256 of the fetched Lua source tarball (empty = skip integrity check)")

find_package(Lua 5.4 QUIET)
if (NOT Lua_FOUND)
    find_package(lua 5.4 QUIET CONFIG)
endif()
set(_sao_lua_resolved OFF)
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

if (NOT _sao_lua_resolved AND SAO_PLUGINS_LUA_AUTO_FETCH)
    include(FetchContent)
    set(_sao_lua_fetch_args
        lua_source
        URL "${SAO_LUA_FETCH_URL}")
    if (SAO_LUA_FETCH_SHA256)
        list(APPEND _sao_lua_fetch_args URL_HASH "SHA256=${SAO_LUA_FETCH_SHA256}")
    endif()
    FetchContent_Declare(${_sao_lua_fetch_args})
    FetchContent_MakeAvailable(lua_source)
    # Upstream Lua ships a plain Makefile.  Compose a small static library
    # target here so we can link it uniformly.  Excludes lua.c (interpreter
    # main) and luac.c (compiler main); we only need the library API.
    file(GLOB _sao_lua_sources CONFIGURE_DEPENDS
        "${lua_source_SOURCE_DIR}/src/*.c")
    list(FILTER _sao_lua_sources EXCLUDE REGEX "/(lua|luac)\\.c$")
    if (_sao_lua_sources)
        add_library(sao_lua_embedded STATIC ${_sao_lua_sources})
        target_include_directories(sao_lua_embedded PUBLIC
            "${lua_source_SOURCE_DIR}/src")
        target_compile_definitions(sao_lua_embedded PUBLIC LUA_COMPAT_5_3)
        set_property(TARGET sao_lua_embedded PROPERTY C_STANDARD 99)
        if (MSVC)
            # Upstream Lua intentionally relies on narrowing C idioms that
            # are noisy under this project's /W4 policy.  Keep warnings
            # enabled for the host adapter while treating fetched C as vendor code.
            target_compile_options(sao_lua_embedded PRIVATE
                /wd4244 /wd4310 /wd4324 /wd4701 /wd4702)
        endif()
        target_link_libraries(sao_plugins_lua_host PRIVATE sao_lua_embedded)
        target_compile_definitions(sao_plugins_lua_host PRIVATE SAO_HAS_LUA=1)
        message(STATUS "plugins/lua_host: fetched Lua 5.4 source into ${lua_source_SOURCE_DIR} (SAO_HAS_LUA=1)")
        set(_sao_lua_resolved ON)
    else()
        message(WARNING "plugins/lua_host: FetchContent succeeded but no *.c under ${lua_source_SOURCE_DIR}/src")
    endif()
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

