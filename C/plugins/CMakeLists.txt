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
option(SAO_PLUGINS_ENABLE_PYTHON  "构建 Python 插件宿主 (需要 vcpkg python3)" ON)
option(SAO_PLUGINS_ENABLE_LUA     "构建 Lua 插件宿主 (vcpkg lua)"             OFF)
option(SAO_PLUGINS_ENABLE_ANGEL   "构建 AngelScript 插件宿主 (vcpkg unofficial-angelscript)" OFF)
option(SAO_PLUGINS_ENABLE_EMMA    "构建 Emma 插件宿主 (零依赖)"                ON)
option(SAO_PLUGINS_ENABLE_CSHARP  "构建 C# 插件宿主 (需要 .NET 8+ runtime)"    OFF)
option(SAO_PLUGINS_LEGACY_FACADE_ONLY
    "只构建 legacy facade、loader 及其本地测试"
    OFF)

# ── v2 分层模块 ────────────────────────────────────────────────────────
add_subdirectory(loader)
set(SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_loader)

if (NOT SAO_PLUGINS_LEGACY_FACADE_ONLY)
    add_subdirectory(sdk_binding)
    add_subdirectory(compat)
    list(APPEND SAO_PLUGINS_COMPONENT_TARGETS
        sao_plugins_sdk_binding
        sao_plugins_compat)
endif()

add_library(sao_plugins_legacy_loader_facade_objects OBJECT
    src/loader.cpp
    src/logging.cpp)
target_include_directories(sao_plugins_legacy_loader_facade_objects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/include)
target_compile_features(sao_plugins_legacy_loader_facade_objects PRIVATE cxx_std_20)
target_compile_definitions(sao_plugins_legacy_loader_facade_objects PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)

# ── Wave 7 / Agent d — Phase 10 AI Editor subprocess plugin skeleton ──
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
        list(APPEND SAO_PLUGINS_COMPONENT_TARGETS sao_plugins_python_host)
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
endif()

# ── v1 兼容外层 (旧 sao_plugins.dll) ──────────────────────────────────
# Legacy v1 requires both optional SDKs.  Keep it available for SDK-equipped
# builds without making an empty skeleton checkout depend on either one.
option(SAO_PLUGINS_BUILD_LEGACY_V1 "Build the v1 sao_plugins.dll compatibility layer" OFF)
if (SAO_PLUGINS_BUILD_LEGACY_V1)
    find_package(Lua QUIET)
    find_package(unofficial-angelscript CONFIG QUIET)
    if (Lua_FOUND AND unofficial-angelscript_FOUND)
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
        target_include_directories(sao_plugins_legacy PRIVATE ${LUA_INCLUDE_DIR})
        target_compile_definitions(sao_plugins_legacy PRIVATE SAO_PLUGINS_BUILDING_DLL)
        target_link_libraries(sao_plugins_legacy PRIVATE
            sao_plugins_loader
            ${LUA_LIBRARIES}
            unofficial::angelscript::angelscript
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

# ── 脚本沙箱共享 STATIC lib ─────────────────────────────────────────
# 独立 STATIC lib sao_script_sandbox_common_lib, 为 Lua / AS sandbox test
# 提供共享 utility.  只在 SAO_BUILD_TESTS 时构建 — 无 test 时 lib 不参与出货.
if (SAO_BUILD_TESTS AND NOT SAO_PLUGINS_LEGACY_FACADE_ONLY)
    add_subdirectory(script_sandbox_common)
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# examples/ 不进 build (只作为运行时骨架)
