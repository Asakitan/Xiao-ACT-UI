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
inline constexpr uint32_t kEntityActionAbiVersion2 = 2;

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
// zero-row probe must return SAO_OK, a zero stride, and a nonzero producer token.
// Non-empty probes may return SAO_OK or SAO_ERR_BUFFER_TOO_SMALL and must report
// an aligned stride large enough for the v2 row required prefix. Fill receives
// the probed count and stride and must reproduce count, revision, producer token,
// and stride exactly. The producer token is only a probe/fill consistency guard;
// it is not exposed as the loader-derived canonical output content_token.
// Borrowed row strings follow the v1 snapshot lifetime.
using entity_snapshot_callback_v2_fn = int32_t(SAO_PLUGINS_CALL*)(
    void* rows, uint32_t capacity, uint32_t row_stride_bytes, uint32_t* out_count,
    uint64_t* out_revision, entity_snapshot_content_token_t* out_content_token,
    uint32_t* out_row_stride_bytes, void* user_data);
using entity_action_handler_fn = int32_t(SAO_PLUGINS_CALL*)(const char* action_id_utf8,
                                                            const char* payload_json_utf8,
                                                            void* user_data);

// Action v2 producers submit exactly one result through the provided sink
// before returning SAO_OK. The result struct and result_json_utf8 are borrowed
// only for the sink call; the loader validates and deep-copies them there.
// handled must be 0 or 1, all reserved bytes must be zero, and a declined
// result must not carry JSON. A handled result may use null for no JSON value.
struct entity_action_result_v2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t handled;
    uint8_t reserved[7];
    const char* result_json_utf8;
};

using entity_action_result_sink_v2_fn =
    int32_t(SAO_PLUGINS_CALL*)(const entity_action_result_v2* result, void* sink_user_data);
using entity_action_handler_v2_fn = int32_t(SAO_PLUGINS_CALL*)(
    const char* action_id_utf8, const char* payload_json_utf8,
    entity_action_result_sink_v2_fn result_sink, void* result_sink_user_data, void* user_data);

struct native_entity_provider_descriptor {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
};

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

// Native query-array v2 descriptor. The required prefix ends at user_data;
// root_contribution is the optional full-size tail. The query descriptor owns
// the physical array stride, so future descriptor tails remain iterable without
// changing this known 48-byte layout.
struct native_entity_provider_descriptor_v2 {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_v2_fn snapshot_v2;
    entity_action_handler_fn action_handler;
    void* user_data;
    const entity_root_contribution_descriptor* root_contribution;
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

// Context v3 preserves the complete 48-byte v2 descriptor prefix and appends
// an action-v2 binding. action_handler is the legacy prefix slot and must be
// null. user_data remains snapshot-owned; action_user_data is independently
// borrowed until replacement, unregister, or context teardown completes.
// ACTION_ONLY with an existing provider_id replaces only its action binding;
// without an existing provider it creates a zero-row, rootless provider.
inline constexpr uint32_t kContextEntityProviderV3ActionOnly = 1u;

struct context_entity_provider_descriptor_v3 {
    uint32_t struct_size;
    const char* provider_id_utf8;
    entity_snapshot_callback_v2_fn snapshot;
    entity_action_handler_fn action_handler;
    void* user_data;
    const entity_root_contribution_descriptor* root_contribution;
    entity_action_handler_v2_fn action_handler_v2;
    void* action_user_data;
    uint32_t flags;
    uint32_t reserved;
};

inline constexpr size_t kNativeEntityProviderDescriptorRequiredPrefixSize =
    offsetof(native_entity_provider_descriptor, user_data) +
    sizeof(static_cast<native_entity_provider_descriptor*>(nullptr)->user_data);
inline constexpr size_t kNativeEntityProviderDescriptorV2RequiredPrefixSize =
    offsetof(native_entity_provider_descriptor_v2, user_data) +
    sizeof(static_cast<native_entity_provider_descriptor_v2*>(nullptr)->user_data);
inline constexpr size_t kEntityRootContributionDescriptorRequiredPrefixSize =
    offsetof(entity_root_contribution_descriptor, priority) +
    sizeof(static_cast<entity_root_contribution_descriptor*>(nullptr)->priority);
inline constexpr size_t kContextEntityProviderDescriptorRequiredPrefixSize =
    offsetof(context_entity_provider_descriptor, user_data) +
    sizeof(static_cast<context_entity_provider_descriptor*>(nullptr)->user_data);
inline constexpr size_t kContextEntityProviderDescriptorV2RequiredPrefixSize =
    offsetof(context_entity_provider_descriptor_v2, user_data) +
    sizeof(static_cast<context_entity_provider_descriptor_v2*>(nullptr)->user_data);
inline constexpr size_t kContextEntityProviderDescriptorV3RequiredPrefixSize =
    offsetof(context_entity_provider_descriptor_v3, action_user_data) +
    sizeof(static_cast<context_entity_provider_descriptor_v3*>(nullptr)->action_user_data);
inline constexpr size_t kEntityActionResultV2RequiredPrefixSize =
    offsetof(entity_action_result_v2, result_json_utf8) +
    sizeof(static_cast<entity_action_result_v2*>(nullptr)->result_json_utf8);

inline constexpr size_t kMaximumEntityActionResultJsonBytes = 1024 * 1024;

inline constexpr size_t kMaximumEntityProvidersPerContext = 256;
inline constexpr size_t kMaximumAttachedEntityProviders = 4096;
inline constexpr size_t kMaximumEntityProvidersPerCatalog = 4096;

static_assert(kNativeEntityProviderDescriptorRequiredPrefixSize <=
              sizeof(native_entity_provider_descriptor));
static_assert(kNativeEntityProviderDescriptorV2RequiredPrefixSize <=
              sizeof(native_entity_provider_descriptor_v2));
static_assert(kEntityRootContributionDescriptorRequiredPrefixSize <=
              sizeof(entity_root_contribution_descriptor));
static_assert(kContextEntityProviderDescriptorRequiredPrefixSize <=
              sizeof(context_entity_provider_descriptor));
static_assert(kContextEntityProviderDescriptorV2RequiredPrefixSize <=
              sizeof(context_entity_provider_descriptor_v2));
static_assert(kContextEntityProviderDescriptorV3RequiredPrefixSize <=
              sizeof(context_entity_provider_descriptor_v3));
static_assert(kEntityActionResultV2RequiredPrefixSize <= sizeof(entity_action_result_v2));

#if INTPTR_MAX == INT64_MAX
static_assert(kNativeEntityProviderDescriptorRequiredPrefixSize == 40);
static_assert(kNativeEntityProviderDescriptorV2RequiredPrefixSize == 40);
static_assert(alignof(native_entity_provider_descriptor_v2) == 8);
static_assert(sizeof(native_entity_provider_descriptor_v2) == 48);
static_assert(offsetof(native_entity_provider_descriptor_v2, struct_size) == 0);
static_assert(offsetof(native_entity_provider_descriptor_v2, provider_id_utf8) == 8);
static_assert(offsetof(native_entity_provider_descriptor_v2, snapshot_v2) == 16);
static_assert(offsetof(native_entity_provider_descriptor_v2, action_handler) == 24);
static_assert(offsetof(native_entity_provider_descriptor_v2, user_data) == 32);
static_assert(offsetof(native_entity_provider_descriptor_v2, root_contribution) == 40);
static_assert(kEntityRootContributionDescriptorRequiredPrefixSize == 48);
static_assert(kContextEntityProviderDescriptorRequiredPrefixSize == 40);
static_assert(kContextEntityProviderDescriptorV2RequiredPrefixSize == 40);
static_assert(alignof(context_entity_provider_descriptor_v2) == 8);
static_assert(sizeof(context_entity_provider_descriptor_v2) == 48);
static_assert(offsetof(context_entity_provider_descriptor_v2, struct_size) == 0);
static_assert(offsetof(context_entity_provider_descriptor_v2, provider_id_utf8) == 8);
static_assert(offsetof(context_entity_provider_descriptor_v2, snapshot) == 16);
static_assert(offsetof(context_entity_provider_descriptor_v2, action_handler) == 24);
static_assert(offsetof(context_entity_provider_descriptor_v2, user_data) == 32);
static_assert(offsetof(context_entity_provider_descriptor_v2, root_contribution) == 40);

static_assert(kContextEntityProviderDescriptorV3RequiredPrefixSize == 64);
static_assert(alignof(context_entity_provider_descriptor_v3) == 8);
static_assert(sizeof(context_entity_provider_descriptor_v3) == 72);
static_assert(offsetof(context_entity_provider_descriptor_v3, struct_size) == 0);
static_assert(offsetof(context_entity_provider_descriptor_v3, provider_id_utf8) == 8);
static_assert(offsetof(context_entity_provider_descriptor_v3, snapshot) == 16);
static_assert(offsetof(context_entity_provider_descriptor_v3, action_handler) == 24);
static_assert(offsetof(context_entity_provider_descriptor_v3, user_data) == 32);
static_assert(offsetof(context_entity_provider_descriptor_v3, root_contribution) == 40);
static_assert(offsetof(context_entity_provider_descriptor_v3, action_handler_v2) == 48);
static_assert(offsetof(context_entity_provider_descriptor_v3, action_user_data) == 56);
static_assert(offsetof(context_entity_provider_descriptor_v3, flags) == 64);
static_assert(offsetof(context_entity_provider_descriptor_v3, reserved) == 68);
static_assert(offsetof(context_entity_provider_descriptor_v3, struct_size) ==
              offsetof(context_entity_provider_descriptor_v2, struct_size));
static_assert(offsetof(context_entity_provider_descriptor_v3, provider_id_utf8) ==
              offsetof(context_entity_provider_descriptor_v2, provider_id_utf8));
static_assert(offsetof(context_entity_provider_descriptor_v3, snapshot) ==
              offsetof(context_entity_provider_descriptor_v2, snapshot));
static_assert(offsetof(context_entity_provider_descriptor_v3, action_handler) ==
              offsetof(context_entity_provider_descriptor_v2, action_handler));
static_assert(offsetof(context_entity_provider_descriptor_v3, user_data) ==
              offsetof(context_entity_provider_descriptor_v2, user_data));
static_assert(offsetof(context_entity_provider_descriptor_v3, root_contribution) ==
              offsetof(context_entity_provider_descriptor_v2, root_contribution));

static_assert(kEntityActionResultV2RequiredPrefixSize == 24);
static_assert(alignof(entity_action_result_v2) == 8);
static_assert(sizeof(entity_action_result_v2) == 24);
static_assert(offsetof(entity_action_result_v2, struct_size) == 0);
static_assert(offsetof(entity_action_result_v2, abi_version) == 4);
static_assert(offsetof(entity_action_result_v2, handled) == 8);
static_assert(offsetof(entity_action_result_v2, reserved) == 9);
static_assert(offsetof(entity_action_result_v2, result_json_utf8) == 16);
#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider(
    plugin_context_t* context, const context_entity_provider_descriptor* descriptor);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider_v2(
    plugin_context_t* context, const context_entity_provider_descriptor_v2* descriptor);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ctx_register_entity_provider_v3(
    plugin_context_t* context, const context_entity_provider_descriptor_v3* descriptor);

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

// Loader-produced v2 stride fields always report the documented known element
// size, including when the corresponding count is zero and data pointer is
// null. Producer-side future tails are accepted through the input stride, but
// loader output copies and exposes only the known v2 prefix.

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
static_assert(alignof(entity_provider_catalog_view_v2) == 8);
static_assert(sizeof(entity_provider_catalog_view_v2) == 56);
static_assert(offsetof(entity_provider_catalog_view_v2, struct_size) == 0);
static_assert(offsetof(entity_provider_catalog_view_v2, abi_version) == 4);
static_assert(offsetof(entity_provider_catalog_view_v2, revision) == 8);
static_assert(offsetof(entity_provider_catalog_view_v2, content_token) == 16);
static_assert(offsetof(entity_provider_catalog_view_v2, provider_count) == 24);
static_assert(offsetof(entity_provider_catalog_view_v2, provider_stride_bytes) == 28);
static_assert(offsetof(entity_provider_catalog_view_v2, providers) == 32);
static_assert(offsetof(entity_provider_catalog_view_v2, root_contribution_count) == 40);
static_assert(offsetof(entity_provider_catalog_view_v2, root_contribution_stride_bytes) == 44);
static_assert(offsetof(entity_provider_catalog_view_v2, root_contributions) == 48);

static_assert(kEntityProviderViewV2RequiredPrefixSize == 64);
static_assert(alignof(entity_provider_view_v2) == 8);
static_assert(sizeof(entity_provider_view_v2) == 64);
static_assert(offsetof(entity_provider_view_v2, struct_size) == 0);
static_assert(offsetof(entity_provider_view_v2, snapshot_abi_version) == 4);
static_assert(offsetof(entity_provider_view_v2, provider_id_utf8) == 8);
static_assert(offsetof(entity_provider_view_v2, owner_plugin_id_utf8) == 16);
static_assert(offsetof(entity_provider_view_v2, generation) == 24);
static_assert(offsetof(entity_provider_view_v2, revision) == 32);
static_assert(offsetof(entity_provider_view_v2, content_token) == 40);
static_assert(offsetof(entity_provider_view_v2, row_count) == 48);
static_assert(offsetof(entity_provider_view_v2, row_stride_bytes) == 52);
static_assert(offsetof(entity_provider_view_v2, rows) == 56);

static_assert(kEntityRootContributionViewV2RequiredPrefixSize == 72);
static_assert(alignof(entity_root_contribution_view_v2) == 8);
static_assert(sizeof(entity_root_contribution_view_v2) == 72);
static_assert(offsetof(entity_root_contribution_view_v2, struct_size) == 0);
static_assert(offsetof(entity_root_contribution_view_v2, owner_plugin_id_utf8) == 8);
static_assert(offsetof(entity_root_contribution_view_v2, contribution_id_utf8) == 16);
static_assert(offsetof(entity_root_contribution_view_v2, root_id_utf8) == 24);
static_assert(offsetof(entity_root_contribution_view_v2, name_utf8) == 32);
static_assert(offsetof(entity_root_contribution_view_v2, icon_utf8) == 40);
static_assert(offsetof(entity_root_contribution_view_v2, priority) == 48);
static_assert(offsetof(entity_root_contribution_view_v2, action_count) == 56);
static_assert(offsetof(entity_root_contribution_view_v2, action_stride_bytes) == 60);
static_assert(offsetof(entity_root_contribution_view_v2, actions) == 64);

static_assert(kEntityRootActionRefViewV2RequiredPrefixSize == 24);
static_assert(alignof(entity_root_action_ref_view_v2) == 8);
static_assert(sizeof(entity_root_action_ref_view_v2) == 24);
static_assert(offsetof(entity_root_action_ref_view_v2, struct_size) == 0);
static_assert(offsetof(entity_root_action_ref_view_v2, provider_id_utf8) == 8);
static_assert(offsetof(entity_root_action_ref_view_v2, action_id_utf8) == 16);

static_assert(kEntityMenuRowV2RequiredPrefixSize == 75);
static_assert(alignof(entity_menu_row_v2) == 8);
static_assert(sizeof(entity_menu_row_v2) == 80);
static_assert(offsetof(entity_menu_row_v2, struct_size) == 0);
static_assert(offsetof(entity_menu_row_v2, category_id_utf8) == 8);
static_assert(offsetof(entity_menu_row_v2, category_label_utf8) == 16);
static_assert(offsetof(entity_menu_row_v2, category_icon_utf8) == 24);
static_assert(offsetof(entity_menu_row_v2, category_priority) == 32);
static_assert(offsetof(entity_menu_row_v2, row_label_utf8) == 40);
static_assert(offsetof(entity_menu_row_v2, row_icon_utf8) == 48);
static_assert(offsetof(entity_menu_row_v2, action_id_utf8) == 56);
static_assert(offsetof(entity_menu_row_v2, payload_json_utf8) == 64);
static_assert(offsetof(entity_menu_row_v2, can_activate) == 72);
static_assert(offsetof(entity_menu_row_v2, keep_menu_open) == 73);
static_assert(offsetof(entity_menu_row_v2, close_menu_before) == 74);
static_assert(offsetof(entity_menu_row_v2, reserved) == 75);
static_assert(sizeof(entity_menu_row_v2) == sizeof(entity_menu_row));
static_assert(alignof(entity_menu_row_v2) == alignof(entity_menu_row));
static_assert(offsetof(entity_menu_row_v2, struct_size) == offsetof(entity_menu_row, struct_size));
static_assert(offsetof(entity_menu_row_v2, category_id_utf8) ==
              offsetof(entity_menu_row, category_id_utf8));
static_assert(offsetof(entity_menu_row_v2, category_label_utf8) ==
              offsetof(entity_menu_row, category_label_utf8));
static_assert(offsetof(entity_menu_row_v2, category_icon_utf8) ==
              offsetof(entity_menu_row, category_icon_utf8));
static_assert(offsetof(entity_menu_row_v2, category_priority) ==
              offsetof(entity_menu_row, category_priority));
static_assert(offsetof(entity_menu_row_v2, row_label_utf8) ==
              offsetof(entity_menu_row, row_label_utf8));
static_assert(offsetof(entity_menu_row_v2, row_icon_utf8) ==
              offsetof(entity_menu_row, row_icon_utf8));
static_assert(offsetof(entity_menu_row_v2, action_id_utf8) ==
              offsetof(entity_menu_row, action_id_utf8));
static_assert(offsetof(entity_menu_row_v2, payload_json_utf8) ==
              offsetof(entity_menu_row, payload_json_utf8));
static_assert(offsetof(entity_menu_row_v2, can_activate) ==
              offsetof(entity_menu_row, can_activate));
static_assert(offsetof(entity_menu_row_v2, keep_menu_open) ==
              offsetof(entity_menu_row, keep_menu_open));
static_assert(offsetof(entity_menu_row_v2, close_menu_before) ==
              offsetof(entity_menu_row, close_menu_before));
static_assert(offsetof(entity_menu_row_v2, reserved) == offsetof(entity_menu_row, reserved));
#endif

// Output catalog views are borrowed snapshots. The catalog and every pointer
// reachable from it are valid only while the corresponding callback executes;
// callers must copy any data that needs to outlive the callback.
using entity_provider_catalog_callback =
    int32_t(SAO_PLUGINS_CALL*)(const entity_provider_catalog_view* catalog, void* user_data);

// Loader-derived v2 content_token values are canonical identities of copied
// known content. A provider token covers provider/owner identity, generation,
// every copied known row field and flag, row/root counts, and copied root
// metadata. It excludes provider revision, the producer consistency token, and
// the producer snapshot ABI version. A catalog token covers the ordered
// provider canonical tokens and provider count, and excludes catalog revision.
// Tokens are deterministic, pointer-independent, nonzero, non-cryptographic
// hashes suitable only for equality/change checks; callers must account for the
// theoretical possibility of collisions.
using entity_provider_catalog_callback_v2 =
    int32_t(SAO_PLUGINS_CALL*)(const entity_provider_catalog_view_v2* catalog, void* user_data);

// The loader owns this callback view. result_json_utf8 points at the validated
// deep copy and remains valid only for the callback duration.
using entity_action_result_callback_v2_fn =
    int32_t(SAO_PLUGINS_CALL*)(const entity_action_result_v2* result, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_snapshot(entity_provider_catalog_callback callback, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_entity_provider_snapshot_v2(
    entity_provider_catalog_callback_v2 callback, void* user_data);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_entity_provider_invoke(const char* provider_id_utf8, uint64_t expected_generation,
                                   const char* action_id_utf8, const char* payload_json_utf8);

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_entity_provider_invoke_v2(
    const char* provider_id_utf8, uint64_t expected_generation, const char* action_id_utf8,
    const char* payload_json_utf8, entity_action_result_callback_v2_fn callback, void* user_data);

} // namespace sao::plugins::loader
