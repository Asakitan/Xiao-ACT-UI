# plugins/loader — 通用插件加载器
# 对齐 Python: act_platform/plugins.py 里 PluginManager + 相关辅助。

add_library(sao_plugins_loader STATIC
    src/entity_provider.cpp
    src/plugin_manifest.cpp
    src/plugin_registry.cpp
    src/plugin_scanner.cpp
    src/plugin_install.cpp
    src/plugin_deps.cpp
    src/plugin_lifecycle.cpp
    src/plugin_isolation.cpp
    src/plugin_context.cpp
    # Phase 14 (Python parity closure): per-event record/skip policy,
    # encrypted native-loader path, script sandbox path check.
    src/selective_parsing.cpp
    src/native_loader_encrypted.cpp
    src/script_path_sandbox.cpp
)
add_library(sao::plugins::loader ALIAS sao_plugins_loader)

target_include_directories(sao_plugins_loader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
)

target_compile_features(sao_plugins_loader PUBLIC cxx_std_20)
target_compile_definitions(sao_plugins_loader PRIVATE SAO_PLUGINS_BUILDING_DLL)
if (MSVC)
    target_compile_definitions(sao_plugins_loader PRIVATE
        _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()

find_package(nlohmann_json 3 CONFIG REQUIRED)
target_link_libraries(sao_plugins_loader
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE shell32
    PRIVATE $<TARGET_NAME_IF_EXISTS:sao_core>
    # Phase 14 native_loader_encrypted uses AES-GCM decrypt from security/crypto
    # and the reflective in-memory loader from security/loader (no disk).
    PRIVATE $<TARGET_NAME_IF_EXISTS:sao_security_crypto>
    PRIVATE $<TARGET_NAME_IF_EXISTS:sao_security_loader>)

