# plugins/angel_host — 真 AngelScript SDK 嵌入
if (NOT SAO_PLUGINS_ENABLE_ANGEL)
    return()
endif()

add_library(sao_plugins_angel_host STATIC
    src/as_host.cpp
    src/as_module_bridge.cpp
    src/as_call.cpp
    src/as_sandbox.cpp
    src/as_stdlib.cpp
    src/as_error.cpp
    # Wave 8 / Agent d Phase 8: plugin lifecycle (真加载真跑)
    src/as_plugin_lifecycle.cpp
)
add_library(sao::plugins::angel_host ALIAS sao_plugins_angel_host)

target_include_directories(sao_plugins_angel_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../loader/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk_binding/include>
)

target_compile_features(sao_plugins_angel_host PUBLIC cxx_std_17)
target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_PLUGINS_BUILDING_DLL)

target_link_libraries(sao_plugins_angel_host PUBLIC
    sao_plugins_loader
    sao_plugins_sdk_binding
)

# AngelScript SDK — 优先 vcpkg unofficial-angelscript, 无 vcpkg 时用 FetchContent
# 从官方 anjo76/angelscript v2.38.0 源码构建静态库 (zlib 许可, 单库).
find_package(unofficial-angelscript CONFIG QUIET)
if (unofficial-angelscript_FOUND)
    target_link_libraries(sao_plugins_angel_host PRIVATE
        unofficial::angelscript::angelscript
    )
    target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_HAS_ANGELSCRIPT=1)
    message(STATUS "plugins/angel_host: 用 vcpkg unofficial-angelscript (SAO_HAS_ANGELSCRIPT=1)")
else()
    # Wave 8 / Agent d: FetchContent fallback — 无 vcpkg 也能真编真跑
    include(FetchContent)
    set(ANGELSCRIPT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS_ANGELSCRIPT_SAVED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    # AngelScript v2.38 x64 MSVC 走 asm callfunc, 需要 ml64 + 无 CXX flag 污染.
    # 顶层 CMakeLists 的 add_compile_options 会全局污染 ml64, 无法排除.
    # 解法: 用 AS_MAX_PORTABILITY 宏定义, AS 内部走纯 C++ generic callfunc,
    # 不再需要 asm 文件.  这轻微降性能但对 hello_angel smoke 完全够用.
    # 我们**不** 设置 CMAKE_CL_64 → AS CMakeLists.txt if(MSVC AND CMAKE_CL_64)
    # 分支不进 → 不加 asm 文件.
    add_compile_definitions(AS_MAX_PORTABILITY=1)
    FetchContent_Declare(
        angelscript_source
        GIT_REPOSITORY https://github.com/anjo76/angelscript.git
        GIT_TAG        v2.38.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  sdk/angelscript/projects/cmake
    )
    FetchContent_MakeAvailable(angelscript_source)
    if (TARGET angelscript)
        # AS_MAX_PORTABILITY + MSVC x64 组合下 asm 文件不必要 (走纯 C++ generic
        # callfunc), 但 AS CMakeLists.txt 无视 AS_MAX_PORTABILITY 仍然把
        # as_callfunc_x64_msvc_asm.asm 加进 sources.  用顶层 add_compile_options
        # 污染 ml64 就 build 不过.  这里从 target 里手动移除 asm 源.
        get_target_property(_as_sources angelscript SOURCES)
        list(FILTER _as_sources EXCLUDE REGEX ".*as_callfunc_x64_msvc_asm\\.asm$")
        set_property(TARGET angelscript PROPERTY SOURCES ${_as_sources})
        # 同样, 顶层用 /GR-, 但 AS 需要 typeid — 打开 /GR 让 AS 静态库正常.
        target_compile_options(angelscript PRIVATE /GR /EHsc /w)
        target_link_libraries(sao_plugins_angel_host PRIVATE angelscript)
        target_include_directories(sao_plugins_angel_host PRIVATE
            ${angelscript_source_SOURCE_DIR}/sdk/angelscript/include)
        target_compile_definitions(sao_plugins_angel_host PRIVATE SAO_HAS_ANGELSCRIPT=1)
        message(STATUS "plugins/angel_host: 用 FetchContent AngelScript v2.38.0 (SAO_HAS_ANGELSCRIPT=1)")
    else()
        message(STATUS "plugins/angel_host: AngelScript FetchContent 失败 —— gated stub")
    endif()
    # 恢复上层 BUILD_SHARED_LIBS 默认 (避免污染兄弟子目录)
    if (DEFINED BUILD_SHARED_LIBS_ANGELSCRIPT_SAVED)
        set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_ANGELSCRIPT_SAVED} CACHE BOOL "" FORCE)
    endif()
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
