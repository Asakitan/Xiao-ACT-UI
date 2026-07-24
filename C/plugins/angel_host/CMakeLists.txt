# plugins/angel_host — 真 AngelScript SDK 嵌入
if (NOT SAO_PLUGINS_ENABLE_ANGEL)
    return()
endif()

add_library(sao_plugins_angel_host STATIC
    src/as_host.cpp
    src/as_module_bridge.cpp
    src/as_call.cpp
    src/as_generic_bindings.cpp
    src/as_loader_adapter.cpp
    src/as_sandbox.cpp
    src/as_stdlib.cpp
    src/as_error.cpp
    # Plugin lifecycle (真加载真跑)
    src/as_plugin_lifecycle.cpp
    # gpu_hunt SDK binding (Vec3 / Mat4 / GpuTracker → AS)
    src/as_gpu_hunt_bind.cpp
)
add_library(sao::plugins::angel_host ALIAS sao_plugins_angel_host)

target_include_directories(sao_plugins_angel_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

# 沙箱敏感字符串使用 SAO_ENC_STR (header-only, 无需链接)
target_include_directories(sao_plugins_angel_host PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>
)

target_compile_features(sao_plugins_angel_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_angel_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
    # gpu_hunt static lib supplies Vec3 / Mat4 / GpuTracker for
    # the standalone binding path.
    sao::gpu_hunt
)

# sao::sdk is linked PRIVATE so its PUBLIC cxx_std_20 requirement does
# not leak into downstream consumers of sao_plugins_angel_host.  The
# gpu-hunt binding source is the only consumer inside this target and
# only needs the header path (SaoSdkContext + SaoSdkGpuHuntTable) at
# compile time.
target_link_libraries(sao_plugins_angel_host PRIVATE
    sao::sdk
)

option(SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME
    "AngelScript SDK 缺失时终止配置，而不是构建 fail-closed stub" OFF)
option(SAO_PLUGINS_ANGEL_FORCE_FALLBACK
    "即使找到 AngelScript SDK 也强制构建 fail-closed stub" OFF)
option(SAO_PLUGINS_ANGEL_MAX_PORTABILITY
    "从本地 SDK 源构建 AS_MAX_PORTABILITY runtime" OFF)
set(SAO_ANGELSCRIPT_SOURCE_DIR "" CACHE PATH
    "AngelScript SDK 根目录（包含 angelscript/ 与 add_on/）")

# Auto-fetch upstream AngelScript SDK if no local runtime is available.
# Enabled by default for the windows-tests preset via cache-var; disable when
# building shipping/hardened targets that must not contact the network.
option(SAO_PLUGINS_ANGEL_AUTO_FETCH
    "Download AngelScript 2.36.1 tarball on demand (FetchContent) when no runtime is found." OFF)
set(SAO_ANGELSCRIPT_FETCH_URL
    "https://www.angelcode.com/angelscript/sdk/files/angelscript_2.36.1.zip"
    CACHE STRING "URL from which the SDK zip is fetched when auto-fetch is enabled")
set(SAO_ANGELSCRIPT_FETCH_SHA256
    ""
    CACHE STRING "SHA256 of the fetched SDK zip (empty = skip integrity check; set for reproducible builds)")

if (SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME AND SAO_PLUGINS_ANGEL_FORCE_FALLBACK)
    message(FATAL_ERROR
        "plugins/angel_host: REQUIRE_RUNTIME 与 FORCE_FALLBACK 不能同时启用")
endif()

# AngelScript SDK resolution order:
#   1. vcpkg unofficial-angelscript / upstream Angelscript CONFIG package
#   2. Operator-supplied SAO_ANGELSCRIPT_SOURCE_DIR (offline vendored source)
#   3. FetchContent from angelcode.com when SAO_PLUGINS_ANGEL_AUTO_FETCH=ON
# Auto-fetch is opt-in so shipping / air-gapped builds stay reproducible.
set(_sao_angel_target "")
if (NOT SAO_PLUGINS_ANGEL_FORCE_FALLBACK)
    find_package(unofficial-angelscript CONFIG QUIET)
    if (unofficial-angelscript_FOUND)
        set(_sao_angel_target unofficial::angelscript::angelscript)
    else()
        find_package(Angelscript CONFIG QUIET)
        if (TARGET Angelscript::angelscript)
            set(_sao_angel_target Angelscript::angelscript)
        endif()
    endif()
    if (NOT _sao_angel_target AND SAO_ANGELSCRIPT_SOURCE_DIR)
        if (NOT EXISTS
            "${SAO_ANGELSCRIPT_SOURCE_DIR}/angelscript/projects/cmake/CMakeLists.txt")
            message(FATAL_ERROR
                "plugins/angel_host: SAO_ANGELSCRIPT_SOURCE_DIR 不是 AngelScript SDK 根目录")
        endif()
        set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        add_subdirectory(
            "${SAO_ANGELSCRIPT_SOURCE_DIR}/angelscript/projects/cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/angelscript-runtime"
            EXCLUDE_FROM_ALL)
        if (TARGET angelscript)
            set(_sao_angel_target angelscript)
        endif()
    endif()
    if (NOT _sao_angel_target AND SAO_PLUGINS_ANGEL_AUTO_FETCH)
        include(FetchContent)
        set(_sao_angel_fetch_args
            angelscript_source
            URL      "${SAO_ANGELSCRIPT_FETCH_URL}")
        if (SAO_ANGELSCRIPT_FETCH_SHA256)
            list(APPEND _sao_angel_fetch_args
                URL_HASH "SHA256=${SAO_ANGELSCRIPT_FETCH_SHA256}")
        endif()
        FetchContent_Declare(${_sao_angel_fetch_args})
        FetchContent_MakeAvailable(angelscript_source)
        if (EXISTS
            "${angelscript_source_SOURCE_DIR}/angelscript/projects/cmake/CMakeLists.txt")
            set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
            add_subdirectory(
                "${angelscript_source_SOURCE_DIR}/angelscript/projects/cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/angelscript-runtime"
                EXCLUDE_FROM_ALL)
            if (TARGET angelscript)
                set(_sao_angel_target angelscript)
                set(SAO_ANGELSCRIPT_SOURCE_DIR
                    "${angelscript_source_SOURCE_DIR}" CACHE PATH "" FORCE)
                message(STATUS "plugins/angel_host: fetched AngelScript source into ${angelscript_source_SOURCE_DIR}")
            endif()
        else()
            message(WARNING
                "plugins/angel_host: fetched archive missing angelscript/projects/cmake/CMakeLists.txt")
        endif()
    endif()
endif()

if (_sao_angel_target)
    if (SAO_PLUGINS_ANGEL_MAX_PORTABILITY)
        if (NOT SAO_ANGELSCRIPT_SOURCE_DIR OR NOT TARGET angelscript OR
            NOT _sao_angel_target STREQUAL "angelscript")
            message(FATAL_ERROR
                "plugins/angel_host: MAX_PORTABILITY 需要 SAO_ANGELSCRIPT_SOURCE_DIR")
        endif()
        target_compile_definitions(angelscript PRIVATE AS_MAX_PORTABILITY)
        if (MSVC)
            get_target_property(_sao_angel_runtime_sources angelscript SOURCES)
            list(FILTER _sao_angel_runtime_sources EXCLUDE REGEX
                "as_callfunc_x64_msvc_asm\\.asm$")
            set_property(TARGET angelscript PROPERTY SOURCES
                "${_sao_angel_runtime_sources}")
        endif()
        target_compile_definitions(sao_plugins_angel_host PRIVATE AS_MAX_PORTABILITY)
    endif()
    target_link_libraries(sao_plugins_angel_host PRIVATE ${_sao_angel_target})
    target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_HAS_ANGELSCRIPT=1)

    # vcpkg 的 addons feature 会把官方 addon 的 .h/.cpp 安装到
    # include/angelscript。直接编译官方源，避免复制实现或维护第二套容器。
    set(SAO_ANGELSCRIPT_ADDON_SOURCE_DIR "" CACHE PATH
        "AngelScript sdk/add_on 目录（package 未携带 addons 时可显式指定）")
    set(_sao_angel_addon_dir "${SAO_ANGELSCRIPT_ADDON_SOURCE_DIR}")
    if (NOT _sao_angel_addon_dir AND SAO_ANGELSCRIPT_SOURCE_DIR)
        set(_sao_angel_addon_dir "${SAO_ANGELSCRIPT_SOURCE_DIR}/add_on")
    endif()
    if (NOT _sao_angel_addon_dir)
        get_target_property(_sao_angel_include_dirs ${_sao_angel_target}
            INTERFACE_INCLUDE_DIRECTORIES)
        foreach (_sao_angel_include_dir IN LISTS _sao_angel_include_dirs)
            if (EXISTS "${_sao_angel_include_dir}/angelscript/scriptarray/scriptarray.cpp")
                set(_sao_angel_addon_dir
                    "${_sao_angel_include_dir}/angelscript")
                break()
            elseif (EXISTS "${_sao_angel_include_dir}/scriptarray/scriptarray.cpp")
                set(_sao_angel_addon_dir "${_sao_angel_include_dir}")
                break()
            endif()
        endforeach()
    endif()

    if (EXISTS "${_sao_angel_addon_dir}/scriptarray/scriptarray.cpp" AND
        EXISTS "${_sao_angel_addon_dir}/scriptdictionary/scriptdictionary.cpp" AND
        EXISTS "${_sao_angel_addon_dir}/scriptmath/scriptmath.cpp" AND
        EXISTS "${_sao_angel_addon_dir}/datetime/datetime.cpp")
        target_sources(sao_plugins_angel_host PRIVATE
            "${_sao_angel_addon_dir}/scriptarray/scriptarray.cpp"
            "${_sao_angel_addon_dir}/scriptdictionary/scriptdictionary.cpp"
            "${_sao_angel_addon_dir}/scriptmath/scriptmath.cpp"
            "${_sao_angel_addon_dir}/datetime/datetime.cpp"
        )
        target_include_directories(sao_plugins_angel_host PRIVATE
            "${_sao_angel_addon_dir}")
        target_compile_definitions(sao_plugins_angel_host PRIVATE
            SAO_HAS_ANGELSCRIPT_ADDONS=1)
        message(STATUS
            "plugins/angel_host: AngelScript runtime + official addons enabled")
        set(_sao_angel_addons_enabled TRUE)
    else()
        if (SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME)
            message(FATAL_ERROR
                "plugins/angel_host: required runtime 缺少 scriptarray、"
                "scriptdictionary、scriptmath 或 datetime addon 源")
        else()
            message(STATUS
                "plugins/angel_host: AngelScript runtime enabled; official addons unavailable")
        endif()
    endif()
elseif (SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME)
    message(FATAL_ERROR
        "plugins/angel_host: SAO_PLUGINS_ANGEL_REQUIRE_RUNTIME=ON, "
        "但未找到 unofficial-angelscript package")
else()
    message(STATUS
        "plugins/angel_host: AngelScript package 未安装；构建 fail-closed stub")
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
