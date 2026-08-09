if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_executable(sao_platform_net_tests
    test_net.cpp
)

target_link_libraries(sao_platform_net_tests PRIVATE
    sao::net
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_net_tests PRIVATE cxx_std_20)

# Consumers of the DLL headers need to see the import macro flip; the
# tests link against the DLL so they set SAO_NET_USING_DLL.
target_compile_definitions(sao_platform_net_tests PRIVATE SAO_NET_USING_DLL)

add_executable(sao_platform_net_network_abi_tests
    network_abi_test.cpp
)
target_link_libraries(sao_platform_net_network_abi_tests PRIVATE
    sao::net
    Catch2::Catch2WithMain
    ws2_32
    bcrypt
    crypt32
)
target_compile_features(sao_platform_net_network_abi_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_platform_net_network_abi_tests PRIVATE SAO_NET_USING_DLL)

# ─── Npcap and TCP reassembly tests ────────────────────────────────
# Npcap wrapper (wpcap.dll late-bound, tests SKIP gracefully when the
# machine has no Npcap installed).
add_executable(sao_platform_net_npcap_tests
    test_npcap.cpp
)
target_link_libraries(sao_platform_net_npcap_tests PRIVATE
    sao::net
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_net_npcap_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_platform_net_npcap_tests PRIVATE SAO_NET_USING_DLL)

# Game-agnostic TCP reassembler (no external deps — always runs).
add_executable(sao_platform_net_reassembly_tests
    test_reassembly.cpp
)
target_link_libraries(sao_platform_net_reassembly_tests PRIVATE
    sao::net
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_net_reassembly_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_platform_net_reassembly_tests PRIVATE SAO_NET_USING_DLL)

# ── engine detector/IL2CPP slice — zstd + protobuf varint ───────────────────
# zstd tests SKIP gracefully when the library is not compiled in
# (SAO_NET_ZSTD_MISSING).  Varint tests are dependency-free.
add_executable(sao_platform_net_zstd_tests
    test_zstd.cpp
)
target_link_libraries(sao_platform_net_zstd_tests PRIVATE
    sao::net
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_net_zstd_tests PRIVATE cxx_std_20)
target_compile_definitions(sao_platform_net_zstd_tests PRIVATE SAO_NET_USING_DLL)

include(CTest)
include(Catch)
add_test(NAME sao_platform_net_tests COMMAND sao_platform_net_tests)
add_test(NAME net_abi COMMAND sao_platform_net_network_abi_tests)
set_tests_properties(net_abi PROPERTIES LABELS "net;network_abi")
# Npcap SDK is licensed and optional.  When wpcap.dll / Packet.dll are not
# installed the Catch2 cases call SKIP(); Catch2 SKIP() exits with 4 and
# without SKIP_RETURN_CODE ctest reports the skip as a hard failure.  See
# the zstd target below and platform/rt_io driver tests for prior art.
add_test(NAME sao_platform_net_npcap_tests COMMAND sao_platform_net_npcap_tests)
set_tests_properties(sao_platform_net_npcap_tests PROPERTIES
    SKIP_RETURN_CODE 4)
add_test(NAME sao_platform_net_reassembly_tests COMMAND sao_platform_net_reassembly_tests)
# Catch2 SKIP() exits with 4; without SKIP_RETURN_CODE ctest reports the
# skip as a hard failure.  See platform/rt_io driver tests for prior art.
add_test(NAME sao_platform_net_zstd_tests COMMAND sao_platform_net_zstd_tests)
set_tests_properties(sao_platform_net_zstd_tests PROPERTIES
    SKIP_RETURN_CODE 4)
