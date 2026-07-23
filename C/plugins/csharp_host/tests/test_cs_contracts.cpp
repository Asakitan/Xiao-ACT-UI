#include <catch2/catch_test_macros.hpp>

#include "cs_component_internal.h"
#include "cs_host_internal.h"

#include "sao/plugins/csharp_host/cs_error.h"
#include "sao/plugins/csharp_host/cs_plugin_lifecycle.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>

using namespace sao::plugins::csharp_host;

namespace {

#if defined(_WIN32)
int g_close_calls = 0;

int32_t __cdecl retrying_close(hostfxr_handle_t) {
    ++g_close_calls;
    return g_close_calls == 1 ? -1 : 0;
}
#endif

} // namespace

TEST_CASE("csharp hostfxr and component delegates use distinct Windows ABIs",
          "[plugins][csharp][contract][abi]") {
#if defined(_WIN32)
    using expected_hostfxr_close = int32_t(__cdecl*)(hostfxr_handle_t);
    using expected_component_entry = int32_t(__stdcall*)(void*, int32_t);
    STATIC_REQUIRE(std::is_same_v<hostfxr_close_fn, expected_hostfxr_close>);
    STATIC_REQUIRE(std::is_same_v<component_entry_point_fn, expected_component_entry>);
#if defined(_M_IX86)
    using wrong_component_entry = int32_t(__cdecl*)(void*, int32_t);
    STATIC_REQUIRE(!std::is_same_v<component_entry_point_fn, wrong_component_entry>);
#endif
#else
    SUCCEED("non-Windows calling conventions collapse to the platform default");
#endif
}

TEST_CASE("managed bridge and OnLoad context have append-only size contracts",
          "[plugins][csharp][contract][context]") {
    STATIC_REQUIRE(std::is_standard_layout_v<cs_sdk_bridge>);
    STATIC_REQUIRE(std::is_standard_layout_v<cs_managed_plugin_context>);
    STATIC_REQUIRE(offsetof(cs_sdk_bridge, log_info) == 0);
    STATIC_REQUIRE(offsetof(cs_sdk_bridge, struct_size) == sizeof(void*) * 3);
    STATIC_REQUIRE(sizeof(cs_managed_plugin_context) == sizeof(uint32_t) * 2 + sizeof(void*) * 4);

    int sdk_marker = 0;
    int loader_marker = 0;
    const cs_managed_plugin_context context{
        static_cast<uint32_t>(sizeof(cs_managed_plugin_context)),
        SAO_CSHOST_MANAGED_ABI_VERSION,
        &sdk_marker,
        &loader_marker,
    };
    REQUIRE(context.struct_size == sizeof(context));
    REQUIRE(context.abi_version == SAO_CSHOST_MANAGED_ABI_VERSION);
    REQUIRE(context.sdk_context != context.loader_context);
}

TEST_CASE("hostfxr close failure is diagnostic and retryable without a runtime",
          "[plugins][csharp][contract][close]") {
#if defined(_WIN32)
    g_close_calls = 0;
    std::string error;
    auto* context = reinterpret_cast<hostfxr_handle_t>(static_cast<uintptr_t>(1));
    REQUIRE(cshost_close_runtime_context(retrying_close, context, error) == SAO_ERR_OS_CALL_FAILED);
    REQUIRE(error.find("hostfxr_close failed rc=-1") != std::string::npos);
    REQUIRE(g_close_calls == 1);

    REQUIRE(cshost_close_runtime_context(retrying_close, context, error) == SAO_OK);
    REQUIRE(error.empty());
    REQUIRE(g_close_calls == 2);
#else
    SUCCEED("hostfxr close is a Windows-only contract");
#endif
}

TEST_CASE("direct and generic errors share one allocator", "[plugins][csharp][contract][error]") {
    char* value = nullptr;
    REQUIRE(cshost_copy_string("managed failure", &value) == SAO_OK);
    REQUIRE(value != nullptr);
    REQUIRE(std::strcmp(value, "managed failure") == 0);
    sao_plugins_cshost_free_string(value);
}

TEST_CASE("managed SDK table append-only ABI locks V1/V2/current windows",
          "[plugins][csharp][contract][abi][append_only]") {
    // ABI version is fixed at 1; append-only growth publishes larger struct
    // sizes without bumping the version, and consumers must gate optional
    // slots via safe_readable_extent.
    STATIC_REQUIRE(SAO_CSHOST_SDK_TABLE_ABI_VERSION == 1);
    STATIC_REQUIRE(SAO_CSHOST_SDK_TABLE_V1_SIZE == 72);
    STATIC_REQUIRE(SAO_CSHOST_SDK_TABLE_V2_SIZE == 88);
    STATIC_REQUIRE(SAO_CSHOST_SDK_TABLE_CURRENT_SIZE == 96);
    STATIC_REQUIRE(SAO_CSHOST_SDK_TABLE_CURRENT_SIZE == sizeof(cs_managed_sdk_table));

    // safe_readable_extent must clamp both under- and over-sized producer
    // struct sizes without exposing a partial pointer at the tail.
    STATIC_REQUIRE(safe_readable_extent(SAO_CSHOST_SDK_TABLE_V1_SIZE,
                                        SAO_CSHOST_SDK_TABLE_CURRENT_SIZE) ==
                   SAO_CSHOST_SDK_TABLE_V1_SIZE);
    STATIC_REQUIRE(safe_readable_extent(SAO_CSHOST_SDK_TABLE_V2_SIZE,
                                        SAO_CSHOST_SDK_TABLE_CURRENT_SIZE) ==
                   SAO_CSHOST_SDK_TABLE_V2_SIZE);
    STATIC_REQUIRE(safe_readable_extent(0, SAO_CSHOST_SDK_TABLE_CURRENT_SIZE) ==
                   SAO_CSHOST_SDK_TABLE_CURRENT_SIZE);
    STATIC_REQUIRE(safe_readable_extent(static_cast<uint32_t>(SAO_CSHOST_SDK_TABLE_CURRENT_SIZE +
                                                              256),
                                        SAO_CSHOST_SDK_TABLE_CURRENT_SIZE) ==
                   SAO_CSHOST_SDK_TABLE_CURRENT_SIZE);

    // Action v2 invocation and result records lock the append-only tail.
    STATIC_REQUIRE(sizeof(cs_managed_action_result_v2) == 24);
    STATIC_REQUIRE(offsetof(cs_managed_action_result_v2, handled) == 8);
    STATIC_REQUIRE(offsetof(cs_managed_action_result_v2, result_json_utf8) == 16);
    STATIC_REQUIRE(sizeof(cs_managed_entity_action_invocation_v2) == 48);
    STATIC_REQUIRE(offsetof(cs_managed_entity_action_invocation_v2, session) == 0);
    STATIC_REQUIRE(offsetof(cs_managed_entity_action_invocation_v2, callback_token) == 8);
    STATIC_REQUIRE(offsetof(cs_managed_entity_action_invocation_v2, action_id_utf8) == 16);
    STATIC_REQUIRE(offsetof(cs_managed_entity_action_invocation_v2, payload_json_utf8) == 24);
    STATIC_REQUIRE(offsetof(cs_managed_entity_action_invocation_v2, result_sink) == 32);
    STATIC_REQUIRE(offsetof(cs_managed_entity_action_invocation_v2, sink_user_data) == 40);
    STATIC_REQUIRE(static_cast<uint32_t>(cs_managed_callback_kind::entity_action_v2) ==
                   static_cast<uint32_t>(cs_managed_callback_kind::entity_action) + 1);
}
