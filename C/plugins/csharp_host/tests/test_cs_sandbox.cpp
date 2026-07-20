#include <catch2/catch_test_macros.hpp>

#include "sao/plugins/csharp_host/cs_sandbox.h"

#include <cstddef>
#include <cstdint>

using namespace sao::plugins::csharp_host;

namespace {

cs_domain_handle_t sandbox_test_domain() {
    return reinterpret_cast<cs_domain_handle_t>(static_cast<uintptr_t>(0x1001));
}

} // namespace

TEST_CASE("cs_sandbox rejects unavailable hostfxr", "[plugins][csharp][sandbox][contract]") {
    REQUIRE(sao_plugins_cshost_sandbox_set_hostfxr_mock(2) == SAO_OK);
    cs_sandbox_config config{};
    REQUIRE(sao_plugins_cshost_sandbox_arm(sandbox_test_domain(), &config) ==
            SAO_ERR_NOT_INITIALIZED);
    REQUIRE(sao_plugins_cshost_sandbox_set_hostfxr_mock(0) == SAO_OK);
}

TEST_CASE("cs_sandbox fails closed", "[plugins][csharp][sandbox][contract]") {
    REQUIRE(sao_plugins_cshost_sandbox_set_hostfxr_mock(1) == SAO_OK);
    cs_sandbox_config config{};
    REQUIRE(sao_plugins_cshost_sandbox_arm(nullptr, &config) == SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_cshost_sandbox_arm(sandbox_test_domain(), nullptr) ==
            SAO_ERR_INVALID_ARGUMENT);
    REQUIRE(sao_plugins_cshost_sandbox_arm(sandbox_test_domain(), &config) ==
            SAO_ERR_NOT_IMPLEMENTED);

    uint64_t alc_id = 99;
    size_t deny_count = 99;
    REQUIRE(sao_plugins_cshost_sandbox_get_policy_snapshot(sandbox_test_domain(), &alc_id,
                                                           &deny_count) == SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE(alc_id == 0);
    REQUIRE(deny_count == 0);

    bool denied = true;
    REQUIRE(sao_plugins_cshost_sandbox_is_api_denied(sandbox_test_domain(), "System.IO.File",
                                                     &denied) == SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE_FALSE(denied);
    REQUIRE(sao_plugins_cshost_sandbox_release(sandbox_test_domain()) == SAO_ERR_NOT_IMPLEMENTED);
    REQUIRE(sao_plugins_cshost_sandbox_clear_all() == 0);
    REQUIRE(sao_plugins_cshost_sandbox_set_hostfxr_mock(0) == SAO_OK);
}

