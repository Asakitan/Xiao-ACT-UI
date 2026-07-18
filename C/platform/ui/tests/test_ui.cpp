#include <catch2/catch_test_macros.hpp>

#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dcomp_bridge.h"
#include "sao/ui/z_order.h"

#include <cstddef>

TEST_CASE("ui ABI and interop layouts are exact", "[ui][abi][interop]") {
    REQUIRE(SAO_UI_ABI_VERSION_MAJOR == 1u);
    REQUIRE(SAO_UI_ABI_VERSION_MINOR == 5u);
    REQUIRE(SAO_UI_ABI_VERSION == 0x00010005u);
    REQUIRE(sao_ui_abi_version() == SAO_UI_ABI_VERSION);

    REQUIRE(SAO_UI_SOPF_MMF_MAGIC == 0x46504F53u);
    REQUIRE(SAO_UI_SOPF_MMF_VERSION_V1 == 1u);
    REQUIRE(SAO_UI_SOPF_MMF_VERSION_V2 == 2u);
    REQUIRE(SAO_UI_SOPF_MMF_HEADER_BYTES == 64u);
    REQUIRE(SAO_UI_SOPF_MMF_V1_MIN_SLOT_COUNT == 1u);
    REQUIRE(SAO_UI_SOPF_MMF_V2_MIN_SLOT_COUNT == 2u);
    REQUIRE(SAO_UI_SOPF_MMF_MAX_SLOT_COUNT == 8u);
    REQUIRE(SAO_UI_SOPF_MMF_SLOT_GENERATION_BYTES == 8u);
    REQUIRE(SAO_UI_SOPF_MMF_MAX_MAPPING_BYTES == 512ull * 1024ull * 1024ull);
    REQUIRE(sizeof(SaoUiSopfMmfHeaderV1) == 64u);
    REQUIRE(sizeof(SaoUiSopfMmfHeaderV2) == 64u);
    REQUIRE(sizeof(SaoUiSopfMmfSlotFooterV2) == 8u);
    REQUIRE(offsetof(SaoUiSopfMmfHeaderV1, published_generation) == 24u);
    REQUIRE(offsetof(SaoUiSopfMmfHeaderV1, published_slot) == 32u);
    REQUIRE(offsetof(SaoUiSopfMmfHeaderV2, published_generation) == 24u);
    REQUIRE(offsetof(SaoUiSopfMmfHeaderV2, latest_completed_slot) == 32u);
    REQUIRE(offsetof(SaoUiSopfMmfHeaderV2, header_bytes) == 36u);
    REQUIRE(SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_KEY == 0ull);
    REQUIRE(SAO_UI_SHARED_TEXTURE_KEYED_MUTEX_TIMEOUT_MS == 8u);
}

TEST_CASE("z_order_status zeroes the struct on an invalid handle", "[ui][z_order]") {
    SaoZOrderStatus status;
    // Fill with garbage and confirm invalid-handle returns still do not leak
    // stale status bytes to callers.
    for (auto& b : reinterpret_cast<uint8_t (&)[sizeof(status)]>(status)) b = 0xEE;
    const auto rc = sao_ui_z_order_status(nullptr, &status);
    REQUIRE(rc == SAO_STATUS_ERR_HANDLE_INVALID);
    REQUIRE(status.policy == 0);
    REQUIRE(status.is_topmost == false);
}
