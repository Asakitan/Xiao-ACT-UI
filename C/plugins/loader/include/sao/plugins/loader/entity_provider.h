#pragma once

#include <cstddef>
#include <cstdint>

#include "sao_plugins/abi.h"

namespace sao::plugins::loader {

typedef struct plugin_context_s plugin_context_t;

using entity_snapshot_content_token_t = uint64_t;

inline constexpr entity_snapshot_content_token_t kInvalidEntitySnapshotContentToken = 0;
inline constexpr uint32_t kEntitySnapshotAbiVersion1 = 1;
inline constexpr uint32_t kEntitySnapshotAbiVersion2 = 2;

struct entity_menu_row {
    uint32_t struct_size;
    const char* category_id_utf8;
    const char* category_label_utf8;
    const char* category_icon_utf8;
    double category_priority;
    const char* row_label_utf8;
    const char* row_icon_utf8;
    const char* action_id_utf8;
    const char* payload_json_utf8;
    uint8_t can_activate;
    uint8_t keep_menu_open;
    uint8_t close_menu_before;
    uint8_t reserved[5];
};

struct entity_menu_row_v2 {
    uint32_t struct_size;
    const char* category_id_utf8;
    const char* category_label_utf8;
    const char* category_icon_utf8;
    double category_priority;
    const char* row_label_utf8;
    const char* row_icon_utf8;
    const char* action_id_utf8;
    const char* payload_json_utf8;
    uint8_t can_activate;
    uint8_t keep_menu_open;
    uint8_t close_menu_before;
    uint8_t reserved[5];
};

// Snapshot callbacks use two phases. A probe call has rows == nullptr and
// capacity == 0. A zero-row probe must return SAO_OK. A non-empty probe may
// return SAO_OK or SAO_ERR_BUFFER_TOO_SMALL with the required count/revision.
// The fill call must return SAO_OK with exactly that count and revision. The
// loader copies row strings after the fill callback returns, so they must stay
// valid until the enclosing sao_plugins_entity_provider_snapshot call returns.
using entity_snapshot_callback_fn = int32_t(SAO_PLUGINS_CALL*)(entity_menu_row* rows,
                                                               uint32_t capacity,
                                                               uint32_t* out_count,
                                                               uint64_t* out_revision,
                                                               void* user_data);

// The v2 producer protocol also probes and fills a physical row stride. Probe
// calls use rows == nullptr, capacity == 0, and row_stride_bytes == 0. A
// zero-row probe must return SAO_OK, a zero stride, and a nonzero content token.
// Non-empty probes may return SAO_OK or SAO_ERR_BUFFER_TOO_SMALL and must report
// an aligned stride large enough for the v2 row required prefix. Fill receives
// the probed count and stride and must reproduce count, revision, content token,
// and stride exactly. Borrowed row strings follow the v1 snapshot lifetime.
using entity_snapshot_callback_v2_fn = int32_t(SAO_PLUGINS_CALL*)(
    void* rows, uint32_t capacity, uint32_t row_stride_bytes, uint32_t* out_count,
    uint64_t* out_revision, entity_snapshot_content_token_t* out_content_token,
    uint32_t* out_row_stride_bytes, void* user_data);
using entity_action_handler_fn = int32_t(SAO_PLUGINS_CALL*)(const char* action_id_utf8,
                                                            const char* payload_json_utf8,
                                                            void* user_data);

struct native_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
};

// Native descriptor arrays are byte-packed. The loader advances each element
// by that element's struct_size so append-only future tails remain iterable.

// Adapter-neutral registration ABI. Script hosts register one provider per
// dynamic root while the canonical plugin context is loading. Ownership,
// generation, enable/disable publication and rundown remain loader-owned.
struct entity_root_contribution_descriptor {
    uint32_t struct_size;
    const char* contribution_id_utf8;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    double priority;
};

struct context_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
    const entity_root_contribution_descriptor* root_contribution;
};

struct context_entity_provider_descriptor_v2 {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_v2_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
    const entity_root_contribution_descriptor* root_contribution;
};

inline constexpr size_t kNativeEntityProviderDescriptorRequiredPrefixSize =
    offsetof(native_entity_provider_descriptor, user_data) +
    sizeof(static_cast<native_entity_provider_descriptor*>(nullptr)->user_data);
inline constexpr size_t kEntityRootContributionDescriptorRequiredPrefixSize =
    offsetof(entity_root_contribution_descriptor, priority) +
    sizeof(static_cast<entity_root_contribution_descriptor*>(nullptr)->priority);
inline constexpr size_t kContextEntityProviderDescriptorRequiredPrefixSize =
    offsetof(context_entity_provider_descriptor, user_data) +
    sizeof(static_cast<context_entity_provider_descriptor*>(nullptr)->user_data);
inline constexpr size_t kContextEntityProviderDescriptorV2RequiredPrefixSize =
    offsetof(context_entity_provider_descriptor_v2, user_data) +
    sizeof(static_cast<context_entity_provider_descriptor_v2*>(nullptr)->user_data);

inline constexpr size_t kMaximumEntityProvidersPerContext = 256;
inline constexpr size_t kMaximumAttachedEntityProviders = 4096;
inline constexpr size_t kMaximumEntityProvidersPerCatalog = 4096;

static_assert(kNativeEntityProviderDescriptorRequiredPrefixSize <=
              sizeof(native_entity_provider_descriptor));
static_assert(kEntityRootContributionDescriptorRequiredPrefixSize <=
              sizeof(entity_root_contribution_descriptor));
static_assert(kContextEntityProviderDescriptorRequiredPrefixSize <=
              sizeof(context_entity_provider_descriptor));
static_assert(kContextEntityProviderDescriptorV2RequiredPrefixSize <=
              sizeof(context_entity_provider_descriptor_v2));

#if INTPTR_MAX == INT64_MAX
static_assert(kNativeEntityProviderDescriptorRequiredPrefixSize == 40);
static_assert(kEntityRootContributionDescriptorRequiredPrefixSize == 48);
static_assert(kContextEntityProviderDescriptorRequiredPrefixSize == 40);
static_assert(kContextEntityProviderDescriptorV2RequiredPrefixSize == 40);
static_assert(sizeof(context_entity_provider_descriptor_v2) == 48);
#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider(
    plugin_context_t* context, const context_entity_provider_descriptor* descriptor);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider_v2(
    plugin_context_t* context, const context_entity_provider_descriptor_v2* descriptor);

// Host-adapter teardown helper. Provider callbacks are quiesced before the
// matching context-owned registrations are removed.
int32_t plugin_context_unregister_entity_providers(plugin_context_t* context,
                                                   const char* const* provider_ids_utf8,
                                                   size_t count) noexcept;

struct entity_provider_view {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* owner_plugin_id_utf8;
    uint64_t generation;
    uint64_t revision;
    uint32_t row_count;
    const entity_menu_row* rows;
};

struct entity_root_action_ref_view {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* action_id_utf8;
};

struct entity_root_contribution_view {
    uint32_t struct_size;
    const char* owner_plugin_id_utf8;
    const char* contribution_id_utf8;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    double priority;
    uint32_t action_count;
    const entity_root_action_ref_view* actions;
};

struct entity_provider_catalog_view {
    uint32_t struct_size;
    uint64_t revision;
    uint32_t provider_count;
    const entity_provider_view* providers;
    uint32_t root_contribution_count;
    const entity_root_contribution_view* root_contributions;
};

struct entity_provider_view_v2 {
    uint32_t struct_size;
    uint32_t snapshot_abi_version;
    const char* provider_id_utf8;
    const char* owner_plugin_id_utf8;
    uint64_t generation;
    uint64_t revision;
    entity_snapshot_content_token_t content_token;
    uint32_t row_count;
    uint32_t row_stride_bytes;
    const void* rows;
};

struct entity_root_action_ref_view_v2 {
    uint32_t struct_size;
    const char* provider_id_utf8;
    const char* action_id_utf8;
};

struct entity_root_contribution_view_v2 {
    uint32_t struct_size;
    const char* owner_plugin_id_utf8;
    const char* contribution_id_utf8;
    const char* root_id_utf8;
    const char* name_utf8;
    const char* icon_utf8;
    double priority;
    uint32_t action_count;
    uint32_t action_stride_bytes;
    const void* actions;
};

struct entity_provider_catalog_view_v2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t revision;
    entity_snapshot_content_token_t content_token;
    uint32_t provider_count;
    uint32_t provider_stride_bytes;
    const void* providers;
    uint32_t root_contribution_count;
    uint32_t root_contribution_stride_bytes;
    const void* root_contributions;
};

// v1 output ABI contract: the catalog is a singleton whose struct_size may be
// interpreted by required prefix. Providers, root contributions, rows, and
// actions are typed fixed-stride arrays with frozen v1 element layouts. A
// required prefix only declares readable fields; it does not permit a physical
// tail after a v1 array element. Element extension requires a new version with
// an explicit stride or packed API, or a sidecar.
inline constexpr size_t kEntityProviderCatalogViewRequiredPrefixSize =
    offsetof(entity_provider_catalog_view, root_contributions) +
    sizeof(static_cast<entity_provider_catalog_view*>(nullptr)->root_contributions);
inline constexpr size_t kEntityProviderViewRequiredPrefixSize =
    offsetof(entity_provider_view, rows) +
    sizeof(static_cast<entity_provider_view*>(nullptr)->rows);
inline constexpr size_t kEntityRootContributionViewRequiredPrefixSize =
    offsetof(entity_root_contribution_view, actions) +
    sizeof(static_cast<entity_root_contribution_view*>(nullptr)->actions);
inline constexpr size_t kEntityRootActionRefViewRequiredPrefixSize =
    offsetof(entity_root_action_ref_view, action_id_utf8) +
    sizeof(static_cast<entity_root_action_ref_view*>(nullptr)->action_id_utf8);
inline constexpr size_t kEntityMenuRowRequiredPrefixSize =
    offsetof(entity_menu_row, close_menu_before) +
    sizeof(static_cast<entity_menu_row*>(nullptr)->close_menu_before);
inline constexpr size_t kEntityProviderCatalogViewV2RequiredPrefixSize =
    offsetof(entity_provider_catalog_view_v2, root_contributions) +
    sizeof(static_cast<entity_provider_catalog_view_v2*>(nullptr)->root_contributions);
inline constexpr size_t kEntityProviderViewV2RequiredPrefixSize =
    offsetof(entity_provider_view_v2, rows) +
    sizeof(static_cast<entity_provider_view_v2*>(nullptr)->rows);
inline constexpr size_t kEntityRootContributionViewV2RequiredPrefixSize =
    offsetof(entity_root_contribution_view_v2, actions) +
    sizeof(static_cast<entity_root_contribution_view_v2*>(nullptr)->actions);
inline constexpr size_t kEntityRootActionRefViewV2RequiredPrefixSize =
    offsetof(entity_root_action_ref_view_v2, action_id_utf8) +
    sizeof(static_cast<entity_root_action_ref_view_v2*>(nullptr)->action_id_utf8);
inline constexpr size_t kEntityMenuRowV2RequiredPrefixSize =
    offsetof(entity_menu_row_v2, close_menu_before) +
    sizeof(static_cast<entity_menu_row_v2*>(nullptr)->close_menu_before);

static_assert(kEntityProviderCatalogViewRequiredPrefixSize <= sizeof(entity_provider_catalog_view));
static_assert(kEntityProviderViewRequiredPrefixSize <= sizeof(entity_provider_view));
static_assert(kEntityRootContributionViewRequiredPrefixSize <=
              sizeof(entity_root_contribution_view));
static_assert(kEntityRootActionRefViewRequiredPrefixSize <= sizeof(entity_root_action_ref_view));
static_assert(kEntityMenuRowRequiredPrefixSize <= sizeof(entity_menu_row));
static_assert(kEntityProviderCatalogViewV2RequiredPrefixSize <=
              sizeof(entity_provider_catalog_view_v2));
static_assert(kEntityProviderViewV2RequiredPrefixSize <= sizeof(entity_provider_view_v2));
static_assert(kEntityRootContributionViewV2RequiredPrefixSize <=
              sizeof(entity_root_contribution_view_v2));
static_assert(kEntityRootActionRefViewV2RequiredPrefixSize <=
              sizeof(entity_root_action_ref_view_v2));
static_assert(kEntityMenuRowV2RequiredPrefixSize <= sizeof(entity_menu_row_v2));

#if INTPTR_MAX == INT64_MAX
static_assert(kEntityProviderCatalogViewRequiredPrefixSize == 48);
static_assert(kEntityProviderViewRequiredPrefixSize == 56);
static_assert(kEntityRootContributionViewRequiredPrefixSize == 72);
static_assert(kEntityRootActionRefViewRequiredPrefixSize == 24);
static_assert(kEntityMenuRowRequiredPrefixSize == 75);
static_assert(kEntityProviderCatalogViewV2RequiredPrefixSize == 56);
static_assert(sizeof(entity_provider_catalog_view_v2) == 56);
static_assert(kEntityProviderViewV2RequiredPrefixSize == 64);
static_assert(sizeof(entity_provider_view_v2) == 64);
static_assert(kEntityRootContributionViewV2RequiredPrefixSize == 72);
static_assert(sizeof(entity_root_contribution_view_v2) == 72);
static_assert(kEntityRootActionRefViewV2RequiredPrefixSize == 24);
static_assert(sizeof(entity_root_action_ref_view_v2) == 24);
static_assert(kEntityMenuRowV2RequiredPrefixSize == 75);
static_assert(sizeof(entity_menu_row_v2) == 80);
#endif

using entity_provider_catalog_callback =
    int32_t(SAO_PLUGINS_CALL*)(const entity_provider_catalog_view* catalog, void* user_data);

// Loader-derived v2 content tokens are deterministic, pointer-independent
// non-cryptographic hashes. They are suitable only for equality/change checks;
// callers must account for the theoretical possibility of collisions.
using entity_provider_catalog_callback_v2 =
    int32_t(SAO_PLUGINS_CALL*)(const entity_provider_catalog_view_v2* catalog, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_snapshot(entity_provider_catalog_callback callback, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_entity_provider_snapshot_v2(
    entity_provider_catalog_callback_v2 callback, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_invoke(const char* provider_id_utf8, uint64_t expected_generation,
                                   const char* action_id_utf8, const char* payload_json_utf8);

} // namespace sao::plugins::loader
