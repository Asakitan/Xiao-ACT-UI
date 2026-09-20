// SAO Auto — SDK: VT-Splitview (hypervisor) capability table.
//
// `ctx->vt` exposes the VT driver control surface to plugins through
// the versioned `SaoSdkVtTable` vtable.  The SDK implementation
// (sdk_vt_wire.cpp) owns a process-wide lazily created rt_io proxy
// runtime and forwards each slot to the matching
// `sao_rt_io_proxy_runtime_vt_*` proxy call — plugins never include or
// link rt_io headers directly.
//
// Result structs below are SDK-local mirrors of the wire types in
// `sao/rt_io/proxy/vt_proxy.h` (identical field order and packing; the
// wire memcpy's between them).  Each mirrors the response prefix as
// `operation_status` / `response_flags` tail fields so scripts can
// inspect the driver's own operation status in addition to the SDK
// slot return value.
//
// Fail-closed contract:
//   * Every slot returns an sao_sdk_status_t.  When the rt_io proxy
//     runtime cannot be created (helper unreachable), slots return
//     SAO_SDK_ERR_UNSUPPORTED.  When the typed call itself fails, the
//     raw outcome is surfaced in `operation_status` and the slot returns
//     SAO_SDK_ERR_INTERNAL.
//   * `provider_status` reports whether the shared proxy runtime is
//     currently usable (handle allocated AND snapshot epoch readable).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sao/sdk/sao_sdk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Mirrored result structs (Append-only POD; see vt_proxy.h) ──────

#define SAO_SDK_VT_MAX_HOOKS 64u
#define SAO_SDK_VT_MAX_HIDE_PAGES 64u
#define SAO_SDK_VT_MAX_PATCH 32u
#define SAO_SDK_VT_MAX_TEMPLATE 4096u
#define SAO_SDK_VT_MAX_TRANSFER (1024u * 1024u)

// map_user wire constants (SAO_RT_IO_VT_MAP_USER_*).  The helper-side
// validator pins abi_version to 0x00010000 and flags to F_READ_ONLY.
#define SAO_SDK_VT_MAP_USER_ABI_VERSION 0x00010000u
#define SAO_SDK_VT_MAP_USER_OP_INVALID 0u
#define SAO_SDK_VT_MAP_USER_OP_MAP 1u
#define SAO_SDK_VT_MAP_USER_OP_UNMAP 2u
#define SAO_SDK_VT_MAP_USER_F_READ_ONLY 0x00000001u
#define SAO_SDK_VT_MAP_USER_F_ALLOWED_MASK SAO_SDK_VT_MAP_USER_F_READ_ONLY

// Decoy modes (SAO_RT_IO_VT_PROXY_DECOY_*).
#define SAO_SDK_VT_DECOY_ZERO 0u
#define SAO_SDK_VT_DECOY_TEMPLATE 1u
#define SAO_SDK_VT_DECOY_CLEAN_SNAPSHOT 2u

// Probe item mask bits (SAO_RT_IO_VT_PROXY_PROBE_ITEM_*).
#define SAO_SDK_VT_PROBE_ITEM_STATUS 0x00000001ull
#define SAO_SDK_VT_PROBE_ITEM_CAPABILITIES 0x00000002ull
#define SAO_SDK_VT_PROBE_ITEM_PERF 0x00000004ull
#define SAO_SDK_VT_PROBE_ITEM_HOOKS 0x00000008ull
#define SAO_SDK_VT_PROBE_ITEM_ALL 0x0000000Full

// list_hooks filter values (SAO_RT_IO_VT_PROXY_LIST_FILTER_*).
#define SAO_SDK_VT_LIST_FILTER_NONE 0u
#define SAO_SDK_VT_LIST_FILTER_HIDDEN_ONLY 1u

#pragma pack(push, 1)
struct SaoSdkVtStatusSnapshot {
    uint32_t struct_size;
    uint32_t state;
    uint32_t abi_version;
    uint32_t root_cpu_count;
    uint32_t armed_hooks;
    uint64_t mapped_base;
    uint64_t heartbeat_age_ms;
    uint32_t engine_state;
    uint32_t status_flags;
    uint32_t availability_reason;
    uint32_t prior_hypervisor_kind;
    uint32_t code_integrity_state;
    uint32_t code_integrity_options;
    uint8_t  prior_hypervisor_signature[16];
};
struct SaoSdkVtCapabilities {
    uint32_t version;
    uint32_t vendor;
    uint64_t supported_flags;
    uint64_t active_flags;
    uint64_t raw_vmx_ept_vpid_cap;
    uint64_t raw_svm_features;
    uint32_t cpu_count;
    uint32_t max_cpu_count;
    uint64_t reserved[10];
};
struct SaoSdkVtPerfStats {
    uint32_t version;
    uint32_t cpu_count;
    uint64_t vmexit_count[64];
    uint64_t vmexit_buckets[16];
};
struct SaoSdkVtHookRow {
    uint64_t hook_id;
    uint64_t gpa;
    uint64_t write_count;
    uint32_t hook_flags;
    uint32_t patch_size;
    uint32_t backend;
    uint32_t state;
};
struct SaoSdkVtMapUserRequest {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t operation;
    uint32_t flags;
    uint64_t target_pid;
    uint64_t target_cr3;
    uint64_t target_user_va;
    uint64_t source_gpa;
    uint64_t expected_pte_gpa;
    uint64_t expected_pte_value;
    uint64_t mapping_id;
    uint64_t generation;
    int32_t  status;
    uint32_t reserved0;
    uint64_t reserved[2];
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(struct SaoSdkVtStatusSnapshot) == 76u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, struct_size) == 0u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, state) == 4u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, abi_version) == 8u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, root_cpu_count) == 12u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, armed_hooks) == 16u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, mapped_base) == 20u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, heartbeat_age_ms) == 28u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, engine_state) == 36u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, status_flags) == 40u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, availability_reason) == 44u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, prior_hypervisor_kind) == 48u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, code_integrity_state) == 52u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, code_integrity_options) == 56u);
static_assert(offsetof(struct SaoSdkVtStatusSnapshot, prior_hypervisor_signature) == 60u);
static_assert(sizeof(struct SaoSdkVtCapabilities) == 128u);
static_assert(offsetof(struct SaoSdkVtCapabilities, version) == 0u);
static_assert(offsetof(struct SaoSdkVtCapabilities, vendor) == 4u);
static_assert(offsetof(struct SaoSdkVtCapabilities, supported_flags) == 8u);
static_assert(offsetof(struct SaoSdkVtCapabilities, active_flags) == 16u);
static_assert(offsetof(struct SaoSdkVtCapabilities, raw_vmx_ept_vpid_cap) == 24u);
static_assert(offsetof(struct SaoSdkVtCapabilities, raw_svm_features) == 32u);
static_assert(offsetof(struct SaoSdkVtCapabilities, cpu_count) == 40u);
static_assert(offsetof(struct SaoSdkVtCapabilities, max_cpu_count) == 44u);
static_assert(offsetof(struct SaoSdkVtCapabilities, reserved) == 48u);
static_assert(sizeof(struct SaoSdkVtPerfStats) == 648u);
static_assert(offsetof(struct SaoSdkVtPerfStats, version) == 0u);
static_assert(offsetof(struct SaoSdkVtPerfStats, cpu_count) == 4u);
static_assert(offsetof(struct SaoSdkVtPerfStats, vmexit_count) == 8u);
static_assert(offsetof(struct SaoSdkVtPerfStats, vmexit_buckets) == 520u);
static_assert(sizeof(struct SaoSdkVtHookRow) == 40u);
static_assert(offsetof(struct SaoSdkVtHookRow, hook_id) == 0u);
static_assert(offsetof(struct SaoSdkVtHookRow, gpa) == 8u);
static_assert(offsetof(struct SaoSdkVtHookRow, write_count) == 16u);
static_assert(offsetof(struct SaoSdkVtHookRow, hook_flags) == 24u);
static_assert(offsetof(struct SaoSdkVtHookRow, patch_size) == 28u);
static_assert(offsetof(struct SaoSdkVtHookRow, backend) == 32u);
static_assert(offsetof(struct SaoSdkVtHookRow, state) == 36u);
static_assert(sizeof(struct SaoSdkVtMapUserRequest) == 104u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, struct_size) == 0u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, abi_version) == 4u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, operation) == 8u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, flags) == 12u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, target_pid) == 16u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, target_cr3) == 24u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, target_user_va) == 32u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, source_gpa) == 40u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, expected_pte_gpa) == 48u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, expected_pte_value) == 56u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, mapping_id) == 64u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, generation) == 72u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, status) == 80u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, reserved0) == 84u);
static_assert(offsetof(struct SaoSdkVtMapUserRequest, reserved) == 88u);
#endif

// Process-wide proxy runtime health snapshot returned by
// `vt->provider_status`.  `connected`/`helper_pid`/`state` come from
// `sao_rt_io_proxy_runtime_snapshot`; `handle_created` is 0 when the
// lazy runtime has not been materialized yet.
struct SaoSdkVtProviderStatus {
    uint32_t handle_created;   // 0/1
    uint32_t connected;        // 0/1
    uint32_t helper_pid;
    uint32_t state;            // sao_rt_io_proxy_runtime_state_e
    uint32_t inflight;
    uint32_t respawn_attempts;
    uint64_t epoch;
    uint64_t reserved;
};

// ─── VT vtable (ctx->vt) ────────────────────────────────────────────
struct SaoSdkVtTable {
    // Read-only queries.
    sao_sdk_status_t(SAO_SDK_CALL* provider_status)(void* ctx_impl,
                                                    SaoSdkVtProviderStatus* out_status);
    sao_sdk_status_t(SAO_SDK_CALL* status)(void* ctx_impl, uint32_t timeout_ms,
                                           SaoSdkVtStatusSnapshot* out_status,
                                           int32_t* out_operation_status,
                                           uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* capabilities)(void* ctx_impl, uint32_t timeout_ms,
                                                 SaoSdkVtCapabilities* out_caps,
                                                 int32_t* out_operation_status,
                                                 uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* probe)(void* ctx_impl, uint64_t items_mask,
                                          uint32_t timeout_ms,
                                          uint64_t* out_completed_mask,
                                          uint64_t* out_ready_mask,
                                          int32_t out_item_status[4],
                                          int32_t* out_operation_status,
                                          uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* list_hooks)(void* ctx_impl, uint32_t filter,
                                               SaoSdkVtHookRow* out_rows,
                                               size_t row_capacity, size_t* out_count,
                                               uint32_t timeout_ms,
                                               int32_t* out_operation_status,
                                               uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* perf_stats)(void* ctx_impl, uint32_t timeout_ms,
                                               SaoSdkVtPerfStats* out_stats,
                                               int32_t* out_operation_status,
                                               uint32_t* out_response_flags);

    // Physical address space access through the hypervisor.
    sao_sdk_status_t(SAO_SDK_CALL* read_phys)(void* ctx_impl, uint64_t gpa,
                                              uint8_t* out_buf, size_t out_capacity,
                                              size_t* out_bytes, uint32_t timeout_ms,
                                              int32_t* out_operation_status,
                                              uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* write_phys)(void* ctx_impl, uint64_t gpa,
                                               const uint8_t* data, size_t size,
                                               size_t* out_bytes, uint32_t timeout_ms,
                                               int32_t* out_operation_status,
                                               uint32_t* out_response_flags);

    // Hook / hide control.
    sao_sdk_status_t(SAO_SDK_CALL* hook_page)(void* ctx_impl, uint64_t kernel_gva,
                                              const uint8_t* patch, size_t patch_size,
                                              uint32_t timeout_ms,
                                              uint64_t* out_hook_id, uint64_t* out_gpa,
                                              int32_t* out_operation_status,
                                              uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* hide_region)(void* ctx_impl, uint64_t kernel_gva,
                                                uint32_t page_count, uint32_t decoy_mode,
                                                const uint8_t* template_bytes,
                                                size_t template_size, uint32_t timeout_ms,
                                                uint64_t* out_hook_ids,
                                                size_t hook_ids_capacity,
                                                uint32_t* out_installed_count,
                                                int32_t* out_operation_status,
                                                uint32_t* out_response_flags);
    sao_sdk_status_t(SAO_SDK_CALL* unhook)(void* ctx_impl, uint64_t hook_id,
                                           uint32_t timeout_ms,
                                           int32_t* out_operation_status,
                                           uint32_t* out_response_flags);

    // User page mapping transaction.
    sao_sdk_status_t(SAO_SDK_CALL* map_user)(void* ctx_impl,
                                             const SaoSdkVtMapUserRequest* request,
                                             uint32_t timeout_ms,
                                             SaoSdkVtMapUserRequest* out_mapping,
                                             int32_t* out_operation_status,
                                             uint32_t* out_response_flags);

    uint32_t abi_version;
    uint32_t struct_size;
};

#define SAO_SDK_VT_TABLE_ABI_VERSION_MAJOR SAO_SDK_ABI_VERSION_MAJOR
#define SAO_SDK_VT_TABLE_ABI_VERSION_MINOR SAO_SDK_ABI_VERSION_MINOR
#define SAO_SDK_VT_TABLE_ABI_VERSION                                                        \
    ((SAO_SDK_VT_TABLE_ABI_VERSION_MAJOR << 16) | SAO_SDK_VT_TABLE_ABI_VERSION_MINOR)
#define SAO_SDK_VT_TABLE_REQUIRED_SIZE                                                      \
    (offsetof(struct SaoSdkVtTable, struct_size) +                                          \
     sizeof(((struct SaoSdkVtTable*)0)->struct_size))

// Table presence + version gate, mirroring sao_sdk_gpu_hunt_table_status.
static inline sao_sdk_status_t
sao_sdk_vt_table_status(const struct SaoSdkContext* ctx) {
    if (ctx == NULL || ctx->vt == NULL)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const uint32_t context_major = ctx->abi_version >> 16;
    const uint32_t context_minor = ctx->abi_version & 0xffffu;
    if (context_major != SAO_SDK_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if (context_minor < SAO_SDK_VT_TABLE_ABI_VERSION_MINOR)
        return SAO_SDK_ERR_UNSUPPORTED;
    if ((ctx->vt->abi_version >> 16) != SAO_SDK_VT_TABLE_ABI_VERSION_MAJOR)
        return SAO_SDK_ERR_ABI_MISMATCH;
    if ((ctx->vt->abi_version & 0xffffu) < SAO_SDK_VT_TABLE_ABI_VERSION_MINOR)
        return SAO_SDK_ERR_UNSUPPORTED;
    if (ctx->vt->struct_size < SAO_SDK_VT_TABLE_REQUIRED_SIZE)
        return SAO_SDK_ERR_UNSUPPORTED;
    return SAO_SDK_OK;
}

#define SAO_SDK_VT_REQUIRE_SLOT(ctx, slot)                                                  \
    do {                                                                                  \
        const sao_sdk_status_t table_status = sao_sdk_vt_table_status(ctx);                 \
        if (table_status != SAO_SDK_OK)                                                     \
            return table_status;                                                          \
        if ((ctx)->vt->struct_size < offsetof(struct SaoSdkVtTable, slot) +                \
                                         sizeof(((struct SaoSdkVtTable*)0)->slot))         \
            return SAO_SDK_ERR_UNSUPPORTED;                                                 \
        if ((ctx)->vt->slot == NULL)                                                        \
            return SAO_SDK_ERR_UNSUPPORTED;                                                 \
    } while (0)

// ─── Convenience forwarders ─────────────────────────────────────────

static inline sao_sdk_status_t sao_sdk_vt_provider_status(
    const struct SaoSdkContext* ctx, SaoSdkVtProviderStatus* out_status) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, provider_status);
    return ctx->vt->provider_status(ctx->ctx_impl, out_status);
}

static inline sao_sdk_status_t sao_sdk_vt_status(
    const struct SaoSdkContext* ctx, uint32_t timeout_ms,
    SaoSdkVtStatusSnapshot* out_status, int32_t* out_operation_status,
    uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, status);
    return ctx->vt->status(ctx->ctx_impl, timeout_ms, out_status, out_operation_status,
                           out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_capabilities(
    const struct SaoSdkContext* ctx, uint32_t timeout_ms,
    SaoSdkVtCapabilities* out_caps, int32_t* out_operation_status,
    uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, capabilities);
    return ctx->vt->capabilities(ctx->ctx_impl, timeout_ms, out_caps, out_operation_status,
                                 out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_probe(
    const struct SaoSdkContext* ctx, uint64_t items_mask, uint32_t timeout_ms,
    uint64_t* out_completed_mask, uint64_t* out_ready_mask, int32_t out_item_status[4],
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, probe);
    return ctx->vt->probe(ctx->ctx_impl, items_mask, timeout_ms, out_completed_mask,
                          out_ready_mask, out_item_status, out_operation_status,
                          out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_list_hooks(
    const struct SaoSdkContext* ctx, uint32_t filter, SaoSdkVtHookRow* out_rows,
    size_t row_capacity, size_t* out_count, uint32_t timeout_ms,
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, list_hooks);
    return ctx->vt->list_hooks(ctx->ctx_impl, filter, out_rows, row_capacity, out_count,
                               timeout_ms, out_operation_status, out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_perf_stats(
    const struct SaoSdkContext* ctx, uint32_t timeout_ms, SaoSdkVtPerfStats* out_stats,
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, perf_stats);
    return ctx->vt->perf_stats(ctx->ctx_impl, timeout_ms, out_stats, out_operation_status,
                               out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_read_phys(
    const struct SaoSdkContext* ctx, uint64_t gpa, uint8_t* out_buf,
    size_t out_capacity, size_t* out_bytes, uint32_t timeout_ms,
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, read_phys);
    return ctx->vt->read_phys(ctx->ctx_impl, gpa, out_buf, out_capacity, out_bytes,
                              timeout_ms, out_operation_status, out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_write_phys(
    const struct SaoSdkContext* ctx, uint64_t gpa, const uint8_t* data, size_t size,
    size_t* out_bytes, uint32_t timeout_ms, int32_t* out_operation_status,
    uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, write_phys);
    return ctx->vt->write_phys(ctx->ctx_impl, gpa, data, size, out_bytes, timeout_ms,
                               out_operation_status, out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_hook_page(
    const struct SaoSdkContext* ctx, uint64_t kernel_gva, const uint8_t* patch,
    size_t patch_size, uint32_t timeout_ms, uint64_t* out_hook_id, uint64_t* out_gpa,
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, hook_page);
    return ctx->vt->hook_page(ctx->ctx_impl, kernel_gva, patch, patch_size, timeout_ms,
                              out_hook_id, out_gpa, out_operation_status,
                              out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_hide_region(
    const struct SaoSdkContext* ctx, uint64_t kernel_gva, uint32_t page_count,
    uint32_t decoy_mode, const uint8_t* template_bytes, size_t template_size,
    uint32_t timeout_ms, uint64_t* out_hook_ids, size_t hook_ids_capacity,
    uint32_t* out_installed_count, int32_t* out_operation_status,
    uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, hide_region);
    return ctx->vt->hide_region(ctx->ctx_impl, kernel_gva, page_count, decoy_mode,
                                template_bytes, template_size, timeout_ms, out_hook_ids,
                                hook_ids_capacity, out_installed_count,
                                out_operation_status, out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_unhook(
    const struct SaoSdkContext* ctx, uint64_t hook_id, uint32_t timeout_ms,
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, unhook);
    return ctx->vt->unhook(ctx->ctx_impl, hook_id, timeout_ms, out_operation_status,
                           out_response_flags);
}

static inline sao_sdk_status_t sao_sdk_vt_map_user(
    const struct SaoSdkContext* ctx, const SaoSdkVtMapUserRequest* request,
    uint32_t timeout_ms, SaoSdkVtMapUserRequest* out_mapping,
    int32_t* out_operation_status, uint32_t* out_response_flags) {
    SAO_SDK_VT_REQUIRE_SLOT(ctx, map_user);
    return ctx->vt->map_user(ctx->ctx_impl, request, timeout_ms, out_mapping,
                             out_operation_status, out_response_flags);
}

#ifdef __cplusplus
} // extern "C"
#endif
