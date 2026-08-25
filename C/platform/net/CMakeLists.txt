# platform/net — packet capture + HTTPS/WSS clients.
#
# SHARED so plugin hosts can dynamically link against just this layer if
# they don't need core.

add_library(sao_platform_net SHARED
    src/npcap_capture.cpp
    src/packet_pipeline.cpp
    src/tcp_reassembly.cpp
    src/http_client.cpp
    src/ws_client.cpp
    src/tls.cpp
    src/url.cpp
    src/winhttp_internal.cpp
    # Protobuf varint and zstd primitives.
    src/proto_zstd.cpp
)
add_library(sao::net ALIAS sao_platform_net)

target_compile_definitions(sao_platform_net PRIVATE SAO_NET_BUILDING_DLL)


target_include_directories(sao_platform_net
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# SAO_ENC_STR() header-only compile-time string encryption; no link dep.
target_include_directories(sao_platform_net PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>)

target_compile_features(sao_platform_net PUBLIC cxx_std_20)

target_link_libraries(sao_platform_net
    PUBLIC sao::core
)

if (WIN32)
    target_link_libraries(sao_platform_net
        PRIVATE
            winhttp
            bcrypt
            crypt32
            ws2_32
            iphlpapi
            # wpcap.dll is loaded lazily via GetProcAddress in
            # src/npcap_capture.cpp — DO NOT link wpcap.lib here.  Builds
            # without the Npcap SDK must still succeed; runtime probing
            # (sao_net_npcap_available) is the source of truth for
            # "Npcap installed on this machine".
    )
endif()

# Optional zstd binding.  Gated on find_package(zstd CONFIG) so hosts
# without the vcpkg port still configure/build; the symbols degrade to
# SAO_STATUS_ERR_CAPABILITY_MISSING when the compile-time macro
# SAO_NET_ZSTD_MISSING is defined (see src/proto_zstd.cpp).  Runtime
# probing via sao_net_zstd_available() is the source of truth for
# "zstd usable in this build".
#
# Gate polarity is INVERSE: SAO_NET_ZSTD_MISSING is DEFINED only when the
# port was not found.  When zstd was found the macro stays undefined and
# the real compress/decompress paths are compiled in.  Do NOT invert this
# to SAO_NET_HAS_ZSTD — proto_zstd.cpp reads the inverse form.
find_package(zstd CONFIG QUIET)
if (TARGET zstd::libzstd_shared OR TARGET zstd::libzstd_static OR TARGET zstd::libzstd)
    if (TARGET zstd::libzstd_shared)
        target_link_libraries(sao_platform_net PRIVATE zstd::libzstd_shared)
    elseif (TARGET zstd::libzstd_static)
        target_link_libraries(sao_platform_net PRIVATE zstd::libzstd_static)
    else()
        target_link_libraries(sao_platform_net PRIVATE zstd::libzstd)
    endif()
    message(STATUS "SAO net: zstd found — proto_zstd bindings enabled "
                   "(SAO_NET_ZSTD_MISSING undefined)")
else()
    target_compile_definitions(sao_platform_net PRIVATE SAO_NET_ZSTD_MISSING)
    message(STATUS "SAO net: zstd NOT found — proto_zstd stubs to "
                   "CAPABILITY_MISSING (SAO_NET_ZSTD_MISSING defined)")
endif()

