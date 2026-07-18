# plugins/script_sandbox_common — 脚本沙箱共享 STATIC lib.
#
# 独立于 lua_host / angel_host 存在, 使 sandbox test binary 可复用 utility
# 而不必链接对应插件的 lua / AS 实现.  规范要求"独立 STATIC lib", 本模块对应.
#
# 不依赖 SAO_PLUGINS_ENABLE_LUA / SAO_PLUGINS_ENABLE_ANGEL — 只在
# SAO_BUILD_TESTS ON 时才编入 (由 plugins/CMakeLists.txt 显式 add_subdirectory).

add_library(sao_script_sandbox_common_lib STATIC
    src/script_sandbox_common.cpp
)
add_library(sao::plugins::script_sandbox_common
    ALIAS sao_script_sandbox_common_lib)

target_include_directories(sao_script_sandbox_common_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    # 供消费方 (Lua / AS sandbox) 拿到 SAO_ENC_STR 头
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>
)

target_compile_features(sao_script_sandbox_common_lib PUBLIC cxx_std_20)

# 与 wave10 SAO_ENC_STR 一致的严格 warning: 不打断构建, 但保持整洁.
if (MSVC)
    target_compile_options(sao_script_sandbox_common_lib PRIVATE /W3 /permissive-)
endif()

message(STATUS "plugins/script_sandbox_common: STATIC lib 就绪 (tag=0x53534258)")
