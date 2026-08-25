# platform/scripting — abstract IScriptEngine interface + registry.
#
# STATIC because concrete engine hosts under ../plugins/*_host/ link
# this to fill in the vtable + register at DLL init time.  A shared
# library would double-instance the registry.

add_library(sao_platform_scripting STATIC
    src/abi.cpp
    src/script_error.cpp
    src/script_context.cpp
    src/script_engine.cpp
    src/script_registry.cpp
)
add_library(sao::scripting ALIAS sao_platform_scripting)

target_include_directories(sao_platform_scripting
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(sao_platform_scripting PUBLIC cxx_std_20)

target_link_libraries(sao_platform_scripting
    PUBLIC sao::core
)

