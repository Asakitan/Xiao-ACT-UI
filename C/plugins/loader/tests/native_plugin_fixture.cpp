#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

using namespace sao::plugins::loader;

namespace {

const char* const kCapabilities[] = {"native_test", "logging"};
std::atomic_int32_t g_on_load_status{SAO_OK};
std::atomic_int32_t g_on_unload_status{SAO_OK};
std::atomic_uint32_t g_on_load_calls{0};
std::atomic_uint32_t g_on_unload_calls{0};

enum class query_mode : int32_t {
    normal = 0,
    descriptor_base_only,
    v1_only,
    descriptor_without_arrays,
    descriptor_v1_only_with_v2_garbage,
    v2_only,
    dual,
    v2_empty,
    descriptor_future_tail,
    v2_required_prefix_only,
    v2_partial_root_tail,
    v2_count_without_pointer,
    v2_zero_count_with_metadata,
    v2_short_stride,
    v2_misaligned_stride,
    v2_unaligned_base,
    v2_short_struct,
    v2_struct_exceeds_stride,
    v2_pointer_overflow,
    v2_count_over_limit,
    duplicate_provider_id,
};

enum class snapshot_mode : int32_t {
    stable = 0,
    token_mismatch,
};

using action_hook_fn = int32_t(SAO_PLUGINS_CALL*)(void* user_data);

std::atomic_int32_t g_query_mode{static_cast<int32_t>(query_mode::normal)};
std::atomic_int32_t g_snapshot_mode{static_cast<int32_t>(snapshot_mode::stable)};
std::atomic_uint64_t g_v2_producer_token{0xd170001ULL};
std::atomic_uint32_t g_v2_content_variant{0};
std::atomic<action_hook_fn> g_action_hook{nullptr};
std::atomic<void*> g_action_hook_user_data{nullptr};

int32_t SAO_PLUGINS_CALL entity_snapshot(entity_menu_row* rows, uint32_t capacity,
                                         uint32_t* out_count, uint64_t* out_revision, void*) {
    if (out_count == nullptr || out_revision == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_count = 1;
    *out_revision = 7;
    if (rows == nullptr || capacity < 1) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    rows[0] = {
        sizeof(entity_menu_row),
        "fixture-category",
        "Fixture Category",
        "fixture-category-icon",
        5.0,
        "Fixture Action",
        "fixture-row-icon",
        "fixture.action",
        R"({"source":"fixture"})",
        true,
        false,
        false,
    };
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL entity_action(const char* action_id_utf8, const char* payload_json_utf8,
                                       void*) {
    return action_id_utf8 != nullptr && payload_json_utf8 != nullptr &&
                   std::string_view(action_id_utf8) == "fixture.action"
               ? SAO_OK
               : SAO_ERR_INVALID_ARGUMENT;
}

const native_entity_provider_descriptor kEntityProviders[] = {{
    sizeof(native_entity_provider_descriptor),
    "fixture",
    entity_snapshot,
    entity_action,
    nullptr,
}};

struct physical_entity_menu_row_v2 {
    entity_menu_row_v2 row{};
    uint64_t future_tail[2]{};
};

static_assert(sizeof(physical_entity_menu_row_v2) == 96);
static_assert(alignof(physical_entity_menu_row_v2) == alignof(entity_menu_row_v2));

int32_t SAO_PLUGINS_CALL entity_snapshot_v2(void* rows, uint32_t capacity,
                                            uint32_t row_stride_bytes, uint32_t* out_count,
                                            uint64_t* out_revision,
                                            entity_snapshot_content_token_t* out_content_token,
                                            uint32_t* out_row_stride_bytes, void*) {
    if (out_count == nullptr || out_revision == nullptr || out_content_token == nullptr ||
        out_row_stride_bytes == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_count = 1;
    *out_revision = 17;
    const auto producer_token = g_v2_producer_token.load(std::memory_order_acquire);
    *out_content_token = producer_token;
    *out_row_stride_bytes = sizeof(physical_entity_menu_row_v2);
    if (rows == nullptr || capacity < 1 || row_stride_bytes == 0) {
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (row_stride_bytes != sizeof(physical_entity_menu_row_v2))
        return SAO_ERR_INVALID_ARGUMENT;
    if (static_cast<snapshot_mode>(g_snapshot_mode.load(std::memory_order_acquire)) ==
        snapshot_mode::token_mismatch) {
        *out_content_token = producer_token + 1;
    }
    const bool changed = g_v2_content_variant.load(std::memory_order_acquire) != 0;
    physical_entity_menu_row_v2 physical{};
    physical.row = {
        sizeof(physical_entity_menu_row_v2),
        "fixture-v2-category",
        "Fixture V2 Category",
        "fixture-v2-category-icon",
        7.5,
        changed ? "Fixture V2 Action Changed" : "Fixture V2 Action",
        "fixture-v2-row-icon",
        "fixture.v2.action",
        changed ? R"({"source":"fixture-v2-changed"})" : R"({"source":"fixture-v2"})",
        true,
        false,
        true,
        {},
    };
    physical.future_tail[0] = 0xd17f0001ULL;
    physical.future_tail[1] = 0xd17f0002ULL;
    std::memcpy(rows, &physical, sizeof(physical));
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL entity_action_v2(const char* action_id_utf8, const char* payload_json_utf8,
                                          void*) {
    if (action_id_utf8 == nullptr || payload_json_utf8 == nullptr ||
        std::string_view(action_id_utf8) != "fixture.v2.action") {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto hook = g_action_hook.load(std::memory_order_acquire);
    return hook == nullptr ? SAO_OK : hook(g_action_hook_user_data.load(std::memory_order_acquire));
}

const entity_root_contribution_descriptor kEntityRootV2{
    sizeof(entity_root_contribution_descriptor),
    "fixture-v2-root",
    "plugin:native-fixture-v2",
    "Fixture Native V2",
    "V2",
    7.5,
};

struct physical_native_entity_provider_descriptor_v2 {
    native_entity_provider_descriptor_v2 current{};
    uint64_t future_tail[2]{};
};

static_assert(sizeof(physical_native_entity_provider_descriptor_v2) == 64);
static_assert(alignof(physical_native_entity_provider_descriptor_v2) ==
              alignof(native_entity_provider_descriptor_v2));

const physical_native_entity_provider_descriptor_v2 kEntityProvidersV2[] = {{
    {
        sizeof(physical_native_entity_provider_descriptor_v2),
        "fixture-v2",
        entity_snapshot_v2,
        entity_action_v2,
        nullptr,
        &kEntityRootV2,
    },
    {0xd17d0001ULL, 0xd17d0002ULL},
}};

const native_entity_provider_descriptor_v2 kRequiredPrefixEntityProvidersV2[] = {{
    static_cast<uint32_t>(kNativeEntityProviderDescriptorV2RequiredPrefixSize),
    "fixture-v2-prefix",
    entity_snapshot_v2,
    entity_action_v2,
    nullptr,
    &kEntityRootV2,
}};

const native_entity_provider_descriptor_v2 kPartialRootEntityProvidersV2[] = {{
    sizeof(native_entity_provider_descriptor_v2) - 1,
    "fixture-v2-partial-root",
    entity_snapshot_v2,
    entity_action_v2,
    nullptr,
    &kEntityRootV2,
}};

const physical_native_entity_provider_descriptor_v2 kDuplicateEntityProvidersV2[] = {{
    {
        sizeof(physical_native_entity_provider_descriptor_v2),
        "fixture",
        entity_snapshot_v2,
        entity_action_v2,
        nullptr,
        &kEntityRootV2,
    },
    {0xd17d1001ULL, 0xd17d1002ULL},
}};

const physical_native_entity_provider_descriptor_v2 kShortEntityProvidersV2[] = {{
    {
        static_cast<uint32_t>(kNativeEntityProviderDescriptorV2RequiredPrefixSize - 1),
        "fixture-v2-short",
        entity_snapshot_v2,
        entity_action_v2,
        nullptr,
        nullptr,
    },
    {},
}};

const physical_native_entity_provider_descriptor_v2 kOversizedEntityProvidersV2[] = {{
    {
        sizeof(physical_native_entity_provider_descriptor_v2),
        "fixture-v2-oversized",
        entity_snapshot_v2,
        entity_action_v2,
        nullptr,
        nullptr,
    },
    {},
}};

const void* unaligned_entity_providers_v2() noexcept {
    alignas(native_entity_provider_descriptor_v2) static const auto storage = [] {
        std::array<std::byte, sizeof(physical_native_entity_provider_descriptor_v2) + 1> value{};
        std::memcpy(value.data() + 1, kEntityProvidersV2, sizeof(kEntityProvidersV2));
        return value;
    }();
    return storage.data() + 1;
}

query_mode current_query_mode() noexcept {
    return static_cast<query_mode>(g_query_mode.load(std::memory_order_acquire));
}

native_plugin_descriptor make_descriptor(uint32_t caller_capacity) noexcept {
    native_plugin_descriptor descriptor{};
    descriptor.struct_size = static_cast<uint32_t>(
        (std::min)(static_cast<size_t>(caller_capacity), sizeof(native_plugin_descriptor)));
    descriptor.abi_version = 2;
    descriptor.plugin_version = "1.0.0";
    descriptor.capability_count = 2;
    descriptor.capabilities = kCapabilities;
    descriptor.entity_provider_count = 1;
    descriptor.entity_providers = kEntityProviders;

    const query_mode mode = current_query_mode();
    if (mode == query_mode::descriptor_base_only) {
        descriptor.struct_size =
            static_cast<uint32_t>(kNativePluginDescriptorBaseRequiredPrefixSize);
        return descriptor;
    }
    if (mode == query_mode::descriptor_without_arrays) {
        descriptor.struct_size = static_cast<uint32_t>(kNativePluginDescriptorV1Size - 1);
        return descriptor;
    }
    if (mode == query_mode::v1_only) {
        descriptor.struct_size = static_cast<uint32_t>(kNativePluginDescriptorV1Size);
        return descriptor;
    }
    if (mode == query_mode::descriptor_v1_only_with_v2_garbage) {
        descriptor.struct_size = static_cast<uint32_t>(kNativePluginDescriptorV2Size - 1);
        descriptor.entity_provider_v2_count = 1;
        descriptor.entity_provider_v2_stride_bytes = 1;
        descriptor.entity_providers_v2 = reinterpret_cast<const void*>(1);
        return descriptor;
    }
    if (caller_capacity < kNativePluginDescriptorV2Size)
        return descriptor;

    if (mode == query_mode::normal)
        return descriptor;
    if (mode == query_mode::v2_empty) {
        descriptor.entity_provider_count = 0;
        descriptor.entity_providers = nullptr;
        return descriptor;
    }

    descriptor.struct_size = static_cast<uint32_t>(kNativePluginDescriptorV2Size);
    descriptor.entity_provider_v2_count = 1;
    descriptor.entity_provider_v2_stride_bytes =
        sizeof(physical_native_entity_provider_descriptor_v2);
    descriptor.entity_providers_v2 = kEntityProvidersV2;
    switch (mode) {
    case query_mode::v2_only:
        descriptor.entity_provider_count = 0;
        descriptor.entity_providers = nullptr;
        break;
    case query_mode::descriptor_future_tail:
        descriptor.struct_size = static_cast<uint32_t>(kNativePluginDescriptorV2Size + 16);
        break;
    case query_mode::v2_required_prefix_only:
        descriptor.entity_provider_v2_stride_bytes =
            static_cast<uint32_t>(kNativeEntityProviderDescriptorV2RequiredPrefixSize);
        descriptor.entity_providers_v2 = kRequiredPrefixEntityProvidersV2;
        break;
    case query_mode::v2_partial_root_tail:
        descriptor.entity_provider_v2_stride_bytes = sizeof(native_entity_provider_descriptor_v2);
        descriptor.entity_providers_v2 = kPartialRootEntityProvidersV2;
        break;
    case query_mode::v2_count_without_pointer:
        descriptor.entity_providers_v2 = nullptr;
        break;
    case query_mode::v2_zero_count_with_metadata:
        descriptor.entity_provider_v2_count = 0;
        break;
    case query_mode::v2_short_stride:
        descriptor.entity_provider_v2_stride_bytes =
            static_cast<uint32_t>(kNativeEntityProviderDescriptorV2RequiredPrefixSize - 1);
        break;
    case query_mode::v2_misaligned_stride:
        descriptor.entity_provider_v2_stride_bytes =
            static_cast<uint32_t>(kNativeEntityProviderDescriptorV2RequiredPrefixSize + 4);
        break;
    case query_mode::v2_unaligned_base:
        descriptor.entity_providers_v2 = unaligned_entity_providers_v2();
        break;
    case query_mode::v2_short_struct:
        descriptor.entity_providers_v2 = kShortEntityProvidersV2;
        break;
    case query_mode::v2_struct_exceeds_stride:
        descriptor.entity_provider_v2_stride_bytes = sizeof(native_entity_provider_descriptor_v2);
        descriptor.entity_providers_v2 = kOversizedEntityProvidersV2;
        break;
    case query_mode::v2_pointer_overflow:
        descriptor.entity_provider_v2_stride_bytes = sizeof(native_entity_provider_descriptor_v2);
        descriptor.entity_providers_v2 =
            reinterpret_cast<const void*>((std::numeric_limits<uintptr_t>::max)() -
                                          (alignof(native_entity_provider_descriptor_v2) - 1));
        break;
    case query_mode::v2_count_over_limit:
        descriptor.entity_provider_v2_count = kMaximumEntityProvidersPerContext;
        break;
    case query_mode::duplicate_provider_id:
        descriptor.entity_providers_v2 = kDuplicateEntityProvidersV2;
        break;
    default:
        break;
    }
    return descriptor;
}

} // namespace

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_lifecycle_statuses(int32_t on_load_status, int32_t on_unload_status) {
    g_on_load_status.store(on_load_status);
    g_on_unload_status.store(on_unload_status);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_query_mode(int32_t mode) {
    if (mode < static_cast<int32_t>(query_mode::normal) ||
        mode > static_cast<int32_t>(query_mode::duplicate_provider_id)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    g_query_mode.store(mode, std::memory_order_release);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_snapshot_mode(int32_t mode) {
    if (mode < static_cast<int32_t>(snapshot_mode::stable) ||
        mode > static_cast<int32_t>(snapshot_mode::token_mismatch)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    g_snapshot_mode.store(mode, std::memory_order_release);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_v2_producer_token(uint64_t token) {
    if (token == kInvalidEntitySnapshotContentToken ||
        token == (std::numeric_limits<uint64_t>::max)()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    g_v2_producer_token.store(token, std::memory_order_release);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_v2_content_variant(uint32_t variant) {
    if (variant > 1)
        return SAO_ERR_INVALID_ARGUMENT;
    g_v2_content_variant.store(variant, std::memory_order_release);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_get_lifecycle_calls(uint32_t* out_on_load_calls, uint32_t* out_on_unload_calls) {
    if (out_on_load_calls == nullptr || out_on_unload_calls == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_on_load_calls = g_on_load_calls.load(std::memory_order_acquire);
    *out_on_unload_calls = g_on_unload_calls.load(std::memory_order_acquire);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_test_plugin_set_action_hook(action_hook_fn hook, void* user_data) {
    g_action_hook_user_data.store(user_data, std::memory_order_release);
    g_action_hook.store(hook, std::memory_order_release);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_test_plugin_reset() {
    g_action_hook.store(nullptr, std::memory_order_release);
    g_action_hook_user_data.store(nullptr, std::memory_order_release);
    g_query_mode.store(static_cast<int32_t>(query_mode::normal), std::memory_order_release);
    g_snapshot_mode.store(static_cast<int32_t>(snapshot_mode::stable), std::memory_order_release);
    g_v2_producer_token.store(0xd170001ULL, std::memory_order_release);
    g_v2_content_variant.store(0, std::memory_order_release);
    g_on_load_status.store(SAO_OK, std::memory_order_release);
    g_on_unload_status.store(SAO_OK, std::memory_order_release);
    g_on_load_calls.store(0, std::memory_order_release);
    g_on_unload_calls.store(0, std::memory_order_release);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_plugin_query_descriptor(native_plugin_descriptor* out_descriptor) {
    if (out_descriptor == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const uint32_t caller_capacity = out_descriptor->struct_size;
    if (caller_capacity < kNativePluginDescriptorBaseRequiredPrefixSize) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const native_plugin_descriptor descriptor = make_descriptor(caller_capacity);
    const size_t write_bytes = (std::min)(static_cast<size_t>(caller_capacity), sizeof(descriptor));
    std::memcpy(out_descriptor, &descriptor, write_bytes);
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL
sao_plugin_on_load(plugin_context_t* context) {
    g_on_load_calls.fetch_add(1, std::memory_order_relaxed);
    return context != nullptr ? g_on_load_status.load() : SAO_ERR_INVALID_ARGUMENT;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_enable() {
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_disable() {
    return SAO_OK;
}

extern "C" __declspec(dllexport) int32_t SAO_PLUGINS_CALL sao_plugin_on_unload() {
    g_on_unload_calls.fetch_add(1, std::memory_order_relaxed);
    return g_on_unload_status.load();
}
