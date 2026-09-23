# plugins/CMakeLists.txt — 多引擎插件宿主 (v2 分层布局)
#
# 平台不 import 任何具体插件, 只按 plugin.json 的 language 派发给 5 种 host:
#   - loader/       通用发现 + 生命周期 + registry
#   - sdk_binding/  SDK C ABI ↔ 5 语言的桥
#   - python_host/  嵌入 CPython 3.11+
#   - emma_host/    Emma 自研零依赖解释器
#   - angel_host/   真 AngelScript SDK 嵌入
#   - lua_host/     Lua 5.4 真 C API 嵌入
#   - csharp_host/  .NET hostfxr + Roslyn
#   - compat/       老 Python 平台插件向下兼容层
#
# examples/ 不进 build (只作为运行时被 loader 扫描的骨架)。
# 顶层 sao_plugins.dll (v1) 保留兼容外层, 内部 delegate 到 v2 模块是后续 phase。

# ── SAO_PLUGINS_ABI_VERSION ────────────────────────────────────────────
set(SAO_PLUGINS_VERSION 2)
add_compile_definitions(SAO_PLUGINS_ABI_VERSION=${SAO_PLUGINS_VERSION})

# ── 单模块开关 (可覆盖) ────────────────────────────────────────────────
option(SAO_PLUGINS_ENABLE_PYTHON  "构建 Python 插件宿主 (默认ON；Python3 Development.Embed，缺 runtime 时 fail-closed stub)" ON)
option(SAO_PLUGINS_ENABLE_LUA     "构建 Lua 插件宿主 (默认ON；内嵌 vendor/lua 5.4，缺时退 find_package/stub)" ON)
option(SAO_PLUGINS_ENABLE_ANGEL   "构建 AngelScript 插件宿主 (默认ON；内嵌 vendor/angelscript_2.36.1，缺时退 package/stub)" ON)
option(SAO_PLUGINS_ENABLE_EMMA    "构建 Emma 插件宿主 (零依赖)"                ON)
option(SAO_PLUGINS_ENABLE_CSHARP  "构建 C# 插件宿主 (默认ON；.NET 8+ runtime 检测，缺时 fail-closed)"    ON)
option(SAO_PLUGINS_ENABLE_PYMINI  "构建 pymini 纯 C++ Python 子集解释器 (engine_kind::python 默认 composite adapter)" ON)
option(SAO_PLUGINS_ENABLE_CSMINI  "构建 csmini 纯 C++ C# 子集解释器 (engine_kind::csharp composite adapter)" ON)
option(SAO_PLUGINS_LEGACY_FACADE_ONLY
    "只构建 legacy facade、loader 及其本地测试"
    OFF)

# ── build-time runtime schema (不进入 ship/install) ────────────────────
# Must precede per-host subdirectories so configure-time metadata remains
# isolated per preset without becoming a runtime-installer asset.
add_subdirectory(runtimes)

# ── v2 分层模块 ────────────────────────────────────────────────────────
add_subdirectory(loader)
set(SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_loader)

if (NOT SAO_PLUGINS_LEGACY_FACADE_ONLY)
    add_subdirectory(sdk_binding)
    add_subdirectory(compat)
    add_subdirectory(script_ctx)
    list(APPEND SAO_PLUGINS_COMPONENT_TARGETS
        sao_plugins_sdk_binding
        sao_plugins_compat
        sao_plugins_script_ctx)
endif()

add_library(sao_plugins_legacy_loader_facade_objects OBJECT
    src/loader.cpp
    src/logging.cpp)
target_include_directories(sao_plugins_legacy_loader_facade_objects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/include)
target_link_libraries(sao_plugins_legacy_loader_facade_objects PRIVATE
    sao_plugins_loader)
target_compile_features(sao_plugins_legacy_loader_facade_objects PRIVATE cxx_std_20)
target_compile_definitions(sao_plugins_legacy_loader_facade_objects PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)

# ── AI Editor subprocess plugin ─────────────────────────────────────────────
# AI Editor is an independent subprocess (mirrors Python subprocess.Popen),
# so its C++ side is a launcher + IPC channel + status ABI, not a full
# WebView2/LLM host.  The subdirectory manages its own SAO_BUILD_AI_EDITOR
# gate (default ON); when disabled the target is skipped and no component
# target is appended here.  See plugins/ai_editor/CMakeLists.txt notes.
if (NOT SAO_PLUGINS_LEGACY_FACADE_ONLY)
    add_subdirectory(ai_editor)
    if (TARGET sao_plugin_ai_editor)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugin_ai_editor)
    endif()

    if (SAO_PLUGINS_ENABLE_PYTHON)
        add_subdirectory(python_host)
        add_library(sao_plugins_python_host_standalone INTERFACE)
        target_link_libraries(sao_plugins_python_host_standalone INTERFACE
            sao_plugins_python_host)
        add_library(sao::plugins::python_host_standalone ALIAS
            sao_plugins_python_host_standalone)
    endif()
    if (SAO_PLUGINS_ENABLE_EMMA)
        add_subdirectory(emma_host)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_emma_host)
    endif()
    if (SAO_PLUGINS_ENABLE_ANGEL)
        add_subdirectory(angel_host)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_angel_host)
    endif()
    if (SAO_PLUGINS_ENABLE_LUA)
        add_subdirectory(lua_host)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_lua_host)
    endif()
    if (SAO_PLUGINS_ENABLE_CSHARP)
        add_subdirectory(csharp_host)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_csharp_host)
    endif()
    # csmini 必须在 csharp_host 之后：composite adapter 可选委托 sao_plugins_cshost_*。
    if (SAO_PLUGINS_ENABLE_CSMINI)
        add_subdirectory(csmini)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_csmini)
    endif()
    # pymini 必须在 python_host 之后：composite adapter 可选委托 sao_plugins_pyhost_*。
    if (SAO_PLUGINS_ENABLE_PYMINI)
        add_subdirectory(pymini)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_pymini)
    endif()
endif()

# ── v1 兼容外层 (旧 sao_plugins.dll) ──────────────────────────────────
# The facade prefers the runtimes already vendored in-tree: lua_host's
# sao_lua_vendored static lib and angel_host's vendored `angelscript`
# target.  When the host subdirectories were skipped
# (SAO_PLUGINS_LEGACY_FACADE_ONLY or per-host switches off) the vendored
# copies are provisioned here directly, so a normal checkout builds the
# facade without vcpkg; find_package stays as the fallback only.
option(SAO_PLUGINS_BUILD_LEGACY_V1 "Build the v1 sao_plugins.dll compatibility layer" ON)
if (SAO_PLUGINS_BUILD_LEGACY_V1)
    # ── Lua: reuse sao_lua_vendored from lua_host, else provision the
    # vendored copy (or SAO_LUA_SOURCE_DIR) ourselves, else find_package. ──
    if (NOT TARGET sao_lua_vendored)
        set(_sao_legacy_lua_dir "${SAO_LUA_SOURCE_DIR}")
        if (NOT _sao_legacy_lua_dir)
            set(_sao_legacy_lua_dir
                "${CMAKE_CURRENT_SOURCE_DIR}/lua_host/vendor/lua")
        endif()
        if (EXISTS "${_sao_legacy_lua_dir}/src/lua.h")
            file(GLOB _sao_legacy_lua_sources CONFIGURE_DEPENDS
                "${_sao_legacy_lua_dir}/src/*.c")
            list(REMOVE_ITEM _sao_legacy_lua_sources
                "${_sao_legacy_lua_dir}/src/lua.c"
                "${_sao_legacy_lua_dir}/src/luac.c"
                "${_sao_legacy_lua_dir}/src/onelua.c")
            if (_sao_legacy_lua_sources)
                add_library(sao_lua_vendored STATIC ${_sao_legacy_lua_sources})
                target_include_directories(sao_lua_vendored PUBLIC
                    "${_sao_legacy_lua_dir}/src")
                if (MSVC)
                    target_compile_definitions(sao_lua_vendored PRIVATE
                        _CRT_SECURE_NO_WARNINGS)
                endif()
            endif()
        endif()
    endif()

    set(_sao_legacy_lua_libs "")
    set(_sao_legacy_lua_includes "")
    if (TARGET sao_lua_vendored)
        set(_sao_legacy_lua_libs sao_lua_vendored)
    elseif (TARGET Lua::Lua)
        set(_sao_legacy_lua_libs Lua::Lua)
    elseif (TARGET lua::lua)
        set(_sao_legacy_lua_libs lua::lua)
    else()
        find_package(Lua 5.4 QUIET)
        if (NOT Lua_FOUND)
            find_package(lua 5.4 QUIET CONFIG)
        endif()
        if (TARGET Lua::Lua)
            set(_sao_legacy_lua_libs Lua::Lua)
        elseif (TARGET lua::lua)
            set(_sao_legacy_lua_libs lua::lua)
        elseif (Lua_FOUND OR LUA_FOUND)
            set(_sao_legacy_lua_libs ${LUA_LIBRARIES})
            set(_sao_legacy_lua_includes ${LUA_INCLUDE_DIR})
        endif()
    endif()

    # ── AngelScript: reuse whichever target a host already resolved
    # (vendored `angelscript` first); when nothing exists yet, provision
    # the vendored SDK ourselves before any package fallback. ──
    if (NOT TARGET angelscript AND
        NOT TARGET unofficial::angelscript::angelscript AND
        NOT TARGET Angelscript::angelscript AND
        NOT SAO_PLUGINS_ANGEL_FORCE_FALLBACK)
        set(_sao_legacy_angel_sdk "${SAO_ANGELSCRIPT_SOURCE_DIR}")
        if (NOT _sao_legacy_angel_sdk)
            set(_sao_legacy_angel_sdk
                "${CMAKE_CURRENT_SOURCE_DIR}/angel_host/vendor/angelscript_2.36.1")
        endif()
        if (EXISTS "${_sao_legacy_angel_sdk}/angelscript/projects/cmake/CMakeLists.txt")
            # Same cache-scope squelch as angel_host's vendored add: the SDK's
            # deprecation warnings and its unscoped /MP option (A4018 on ml64).
            get_property(_sao_warn_deprecated_type CACHE CMAKE_WARN_DEPRECATED PROPERTY TYPE)
            if (_sao_warn_deprecated_type)
                get_property(_sao_warn_deprecated_value CACHE CMAKE_WARN_DEPRECATED PROPERTY VALUE)
            endif()
            get_property(_sao_msvc_flags_type CACHE MSVC_COMPILE_FLAGS PROPERTY TYPE)
            if (_sao_msvc_flags_type)
                get_property(_sao_msvc_flags_value CACHE MSVC_COMPILE_FLAGS PROPERTY VALUE)
            endif()
            set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL
                "Suppress deprecation warnings while configuring vendored AngelScript" FORCE)
            if (MSVC)
                set(MSVC_COMPILE_FLAGS "" CACHE STRING
                    "AngelScript MSVC compiler flags" FORCE)
            endif()
            set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
            add_subdirectory(
                "${_sao_legacy_angel_sdk}/angelscript/projects/cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/legacy_v1_angelscript"
                EXCLUDE_FROM_ALL)
            if (_sao_warn_deprecated_type)
                set(CMAKE_WARN_DEPRECATED "${_sao_warn_deprecated_value}" CACHE BOOL
                    "Whether to issue warnings for deprecated CMake functionality" FORCE)
            else()
                unset(CMAKE_WARN_DEPRECATED CACHE)
            endif()
            if (_sao_msvc_flags_type)
                set(MSVC_COMPILE_FLAGS "${_sao_msvc_flags_value}" CACHE STRING
                    "Restored MSVC compiler flags" FORCE)
            else()
                unset(MSVC_COMPILE_FLAGS CACHE)
            endif()
        endif()
    endif()

    set(_sao_legacy_angel_libs "")
    if (TARGET angelscript)
        set(_sao_legacy_angel_libs angelscript)
    elseif (TARGET unofficial::angelscript::angelscript)
        set(_sao_legacy_angel_libs unofficial::angelscript::angelscript)
    elseif (TARGET Angelscript::angelscript)
        set(_sao_legacy_angel_libs Angelscript::angelscript)
    else()
        find_package(unofficial-angelscript CONFIG QUIET)
        if (TARGET unofficial::angelscript::angelscript)
            set(_sao_legacy_angel_libs unofficial::angelscript::angelscript)
        else()
            find_package(Angelscript CONFIG QUIET)
            if (TARGET Angelscript::angelscript)
                set(_sao_legacy_angel_libs Angelscript::angelscript)
            endif()
        endif()
    endif()

    if (_sao_legacy_lua_libs AND _sao_legacy_angel_libs)
        add_library(sao_plugins_legacy SHARED
            src/sao_plugins.cpp
            src/lua_runtime.cpp
            src/angel_runtime.cpp
            $<TARGET_OBJECTS:sao_plugins_legacy_loader_facade_objects>
        )
        add_library(sao::plugins::legacy ALIAS sao_plugins_legacy)
        set_target_properties(sao_plugins_legacy PROPERTIES OUTPUT_NAME "sao_plugins")

        target_include_directories(sao_plugins_legacy PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        )
        if (_sao_legacy_lua_includes)
            target_include_directories(sao_plugins_legacy PRIVATE
                ${_sao_legacy_lua_includes})
        endif()
        target_compile_definitions(sao_plugins_legacy PRIVATE SAO_PLUGINS_BUILDING_DLL)
        target_compile_features(sao_plugins_legacy PRIVATE cxx_std_20)
        target_link_libraries(sao_plugins_legacy PRIVATE
            sao_plugins_loader
            ${_sao_legacy_lua_libs}
            ${_sao_legacy_angel_libs}
        )
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_legacy)
    else()
        message(WARNING "plugins: legacy v1 requested but Lua or AngelScript is unavailable; skipping it")
    endif()
endif()

# First-party v1 examples are optional as well: hide_seek needs OpenCV, while
# the runtime aggregate is complete without either example plugin.
option(SAO_PLUGINS_BUILD_LEGACY_EXAMPLES "Build legacy first-party example plugins" OFF)
if (SAO_PLUGINS_BUILD_LEGACY_EXAMPLES)
    add_subdirectory(midi_piano)
    list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugin_midi_piano)

    find_package(OpenCV CONFIG QUIET)
    if (OpenCV_FOUND)
        add_subdirectory(hide_seek)
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugin_hide_seek)
    else()
        message(WARNING "plugins: OpenCV unavailable; skipping legacy hide_seek example")
    endif()
endif()

add_library(sao_plugins_runtime INTERFACE)
add_library(sao::plugins ALIAS sao_plugins_runtime)
target_link_libraries(sao_plugins_runtime INTERFACE ${SAO_PLUGINS_COMPONENT_TARGETS})

# examples/ 不进 build (只作为运行时骨架)
