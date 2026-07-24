# platform/runtime_installer -- optional runtime download + verify library.
#
# Ships as a SHARED library so the launcher and plugin hosts can dlopen it
# without pulling core in via a static archive.  The module is intentionally
# self-contained: it depends only on sao::core, the enc_str header-only
# shim, and Win32 (WinHTTP + BCrypt + Shell32/Ole32 for known-folder
# resolution and GUID formatting).

add_library(sao_runtime_installer SHARED
    src/runtime_installer.cpp
    src/manifest.cpp
    src/download_engine.cpp
    src/integrity.cpp
)
add_library(sao::runtime_installer ALIAS sao_runtime_installer)

target_compile_definitions(sao_runtime_installer PRIVATE
    SAO_RUNTIME_INSTALLER_BUILDING_DLL
    # The test hooks (transport + root override) are always compiled into
    # the DLL so tests can dlopen the same binary the launcher ships.  The
    # public header only exposes their declarations when consumers opt in
    # via SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS, so shipped code cannot
    # accidentally reach them.  This mirrors the pattern used by
    # sao_platform_net's late-bound wpcap probe.
    SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)

target_include_directories(sao_runtime_installer
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# SAO_ENC_STR() header-only compile-time string encryption; no link dep.
target_include_directories(sao_runtime_installer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>)

target_compile_features(sao_runtime_installer PUBLIC cxx_std_20)

target_link_libraries(sao_runtime_installer
    PUBLIC  sao::core)
# NOTE: BLAKE3 verification against sao_security_crypto is currently
# deferred -- the security subdirectory is added AFTER platform in the
# top-level CMakeLists so the target is not yet visible.  SHA-256 (BCrypt)
# is the mandatory integrity primitive; the manifest may still carry a
# blake3_hex that is copied into the on-disk marker for future promotion
# once BLAKE3 lands under platform/core.

if (WIN32)
    target_link_libraries(sao_runtime_installer
        PRIVATE
            winhttp
            bcrypt
            crypt32
            shell32
            ole32
    )
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
