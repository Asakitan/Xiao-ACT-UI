#include <catch2/catch_test_macros.hpp>

#include "sao/ui/abi.h"
#include "sao/ui/compositor.h"
#include "sao/ui/dcomp_bridge.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/panel_sdk.h"
#include "sao/ui/streaming_flow.h"
#include "sao/ui/z_order.h"

#include <cstddef>
#include <thread>

TEST_CASE("ui ABI and interop layouts are exact", "[ui][abi][interop]") {
    REQUIRE(SAO_UI_ABI_VERSION_MAJOR == 1u);
    // Minor 8 adds a leading uint32_t struct_size guard to SaoPanelDescriptor,
    // SaoCompositorConfig, and SaoLayerConfig. Existing exports unchanged.
    REQUIRE(SAO_UI_ABI_VERSION_MINOR == 8u);
    REQUIRE(SAO_UI_ABI_VERSION == 0x00010008u);
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

TEST_CASE("ui ABI struct_size guards lock v1 byte sizes", "[ui][abi][struct_size]") {
    // Header-side static_assert already enforces these; a runtime test just
    // documents the values and STATIC_REQUIREs them so a future struct edit
    // that changes packing without bumping the constant fails at test build
    // time even when the corresponding header is not included by any other
    // TU in the compilation set.
    STATIC_REQUIRE(SAO_UI_PANEL_DESCRIPTOR_V1_SIZE == 136u);
    STATIC_REQUIRE(SAO_UI_LAYER_CONFIG_V1_SIZE == 48u);
    STATIC_REQUIRE(SAO_UI_COMPOSITOR_CONFIG_V1_SIZE == 12u);
    STATIC_REQUIRE(SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE == 56u);

    STATIC_REQUIRE(sizeof(SaoPanelDescriptor) == SAO_UI_PANEL_DESCRIPTOR_V1_SIZE);
    STATIC_REQUIRE(sizeof(SaoLayerConfig) == SAO_UI_LAYER_CONFIG_V1_SIZE);
    STATIC_REQUIRE(sizeof(SaoCompositorConfig) == SAO_UI_COMPOSITOR_CONFIG_V1_SIZE);
    STATIC_REQUIRE(sizeof(SaoUiEntityRootItem) == SAO_UI_ENTITY_ROOT_ITEM_V1_SIZE);

    STATIC_REQUIRE(offsetof(SaoPanelDescriptor, struct_size) == 0u);
    STATIC_REQUIRE(offsetof(SaoLayerConfig, struct_size) == 0u);
    STATIC_REQUIRE(offsetof(SaoCompositorConfig, struct_size) == 0u);
    STATIC_REQUIRE(offsetof(SaoUiEntityRootItem, struct_size) == 0u);
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

TEST_CASE("streaming mode lock enforces release ownership and lifecycle",
          "[ui][streaming_flow][mode_lock]") {
    REQUIRE(sao_streaming_flow_reset_for_tests() == SAO_STATUS_OK);

    SECTION("normal release") {
        REQUIRE(sao_streaming_flow_mode_lock_acquire(0.0) == SAO_STATUS_OK);
        CHECK(sao_streaming_flow_mode_lock_release() == SAO_STATUS_OK);
    }

    SECTION("wrong thread release") {
        REQUIRE(sao_streaming_flow_mode_lock_acquire(0.0) == SAO_STATUS_OK);
        sao_status_t wrong_thread_status = SAO_STATUS_OK;
        std::thread wrong_thread([&] {
            wrong_thread_status = sao_streaming_flow_mode_lock_release();
        });
        wrong_thread.join();
        CHECK(wrong_thread_status == SAO_STATUS_ERR_ACCESS_DENIED);
        CHECK(sao_streaming_flow_mode_lock_release() == SAO_STATUS_OK);
    }

    SECTION("double release") {
        REQUIRE(sao_streaming_flow_mode_lock_acquire(0.0) == SAO_STATUS_OK);
        REQUIRE(sao_streaming_flow_mode_lock_release() == SAO_STATUS_OK);
        CHECK(sao_streaming_flow_mode_lock_release() == SAO_STATUS_ERR_NOT_INITIALIZED);
    }
}
