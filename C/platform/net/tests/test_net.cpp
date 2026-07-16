#include <catch2/catch_test_macros.hpp>

#include "sao/net/npcap_capture.h"

TEST_CASE("net ABI version is non-zero", "[net][abi]") {
    REQUIRE(sao_net_abi_version() == SAO_NET_ABI_VERSION);
}

TEST_CASE("enum_interfaces follows Npcap provider availability", "[net][enum]") {
    size_t count = 42;
    const auto status = sao_net_capture_enum_interfaces(nullptr, 0, &count);
    bool available = false;
    REQUIRE(sao_net_npcap_available(&available) == SAO_STATUS_OK);
    if (available) {
        REQUIRE(status == SAO_STATUS_OK);
    } else {
        REQUIRE(status == SAO_STATUS_ERR_NOT_FOUND);
        REQUIRE(count == 0);
    }
}
