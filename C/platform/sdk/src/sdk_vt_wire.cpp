// SAO Auto — SDK VT wire.
//
// Backs `SaoSdkContext::vt` (SaoSdkVtTable).  Every slot forwards into
// the client-side rt_io VT proxy (`sao_rt_io_proxy_runtime_vt_*`) over
// a process-wide lazily materialized `sao_rt_io_proxy_runtime_handle_t`
// — the same borrowed-handle pattern used by plugins/ai_editor
// vt_bridge.cpp (`autospawn = 0`, attach-only; the SDK never spawns the
// helper process itself).
//
// Plugins only see `sao_sdk_vt.h`; no rt_io header crosses the SDK /
// plugin ABI.  Results are copied field-by-field into the SDK-local
// mirror structs (which are layout-identical to the wire types).
//
// Concurrency: a single mutex serializes handle materialization, epoch
// checks, and per-call responses.  VT ops are heavyweight ioctl-shaped
// transactions; per-call serialization is intentional and matches the
// helper session model.
//
// Fail closed:
//   * runtime create / snapshot failure → the handle is destroyed and
//     the slot returns SAO_SDK_ERR_UNSUPPORTED (provider_status stays
//     readable and reports handle_created=0);
//   * typed call failure → prefix fields are surfaced and the slot
//     returns SAO_SDK_ERR_INTERNAL when no typed response arrived.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sao/rt_io/proxy/rt_io_proxy.h"
#include "sao/rt_io/proxy/vt_proxy.h"

#include <cstring>
#include <mutex>

namespace sao_sdk_internal {
namespace {

std::mutex g_vt_runtime_mutex;
sao_rt_io_proxy_runtime_handle_t g_vt_runtime = nullptr;
uint64_t g_vt_epoch = 0;
bool g_vt_epoch_valid = false;

// Materialize the shared proxy runtime.  `autospawn = 0`: attach-only,
// no helper spawn side effects from the SDK.
sao_status_t ensure_runtime_locked() {
    if (g_vt_runtime != nullptr)
        return SAO_STATUS_OK;
    SaoRtIoProxyRuntimeConfig config{};
    config.autospawn = 0u;
    sao_rt_io_proxy_runtime_handle_t created = nullptr;
    const sao_status_t status = sao_rt_io_proxy_runtime_create(&config, &created);
    if (status != SAO_STATUS_OK || created == nullptr)
        return status == SAO_STATUS_OK ? SAO_RT_IO_ERR_INTERNAL_ERROR : status;
    g_vt_runtime = created;
    g_vt_epoch_valid = false;
    return SAO_STATUS_OK;
}

void invalidate_locked() {
    if (g_vt_runtime != nullptr) {
        sao_rt_io_proxy_runtime_destroy(g_vt_runtime);
        g_vt_runtime = nullptr;
    }
    g_vt_epoch_valid = false;
}

// Ensure the handle exists and refresh the helper epoch.  Any snapshot
// failure or epoch drift invalidates the handle so the next call
// re-attaches cleanly.
sao_status_t sync_locked() {
    const sao_status_t runtime_status = ensure_runtime_locked();
    if (runtime_status != SAO_STATUS_OK)
        return runtime_status;
    SaoRtIoProxyRuntimeSnapshot snapshot{};
    const sao_status_t status =
        sao_rt_io_proxy_runtime_snapshot(g_vt_runtime, &snapshot);
    if (status != SAO_STATUS_OK) {
        invalidate_locked();
        return SAO_RT_IO_ERR_SESSION_REKEY_REQUIRED;
    }
    if (!g_vt_epoch_valid || g_vt_epoch != snapshot.epoch) {
        g_vt_epoch = snapshot.epoch;
        g_vt_epoch_valid = true;
    }
    return SAO_STATUS_OK;
}

// Map a transport-level failure into an SDK slot status.  Typed
// responses (authenticated, request_id matched, transport complete)
// still carry their own operation_status in the response prefix.
sao_sdk_status_t map_call_status(sao_status_t status,
                                 const SaoRtIoProxyRuntimeCallResult& call) {
    if (status == SAO_STATUS_OK && call.authenticated != 0 &&
        call.request_id_matched != 0)
        return SAO_SDK_OK;
    return SAO_SDK_ERR_INTERNAL;
}

void fill_prefix(const SaoRtIoVtProxyResponsePrefix& prefix, int32_t* out_op_status,
                 uint32_t* out_response_flags) {
    if (out_op_status != nullptr)
        *out_op_status = prefix.operation_status;
    if (out_response_flags != nullptr)
        *out_response_flags = prefix.response_flags;
}

sao_sdk_status_t vt_provider_status(void* /*ctx_impl*/,
                                    SaoSdkVtProviderStatus* out_status) {
    if (out_status == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    *out_status = SaoSdkVtProviderStatus{};
    if (g_vt_runtime == nullptr) {
        // Materialize on demand so availability reflects reality even
        // before the first real call.
        if (ensure_runtime_locked() != SAO_STATUS_OK)
            return SAO_SDK_OK; // handle_created stays 0
    }
    out_status->handle_created = 1u;
    SaoRtIoProxyRuntimeSnapshot snapshot{};
    const sao_status_t status =
        sao_rt_io_proxy_runtime_snapshot(g_vt_runtime, &snapshot);
    if (status != SAO_STATUS_OK) {
        invalidate_locked();
        return SAO_SDK_OK; // handle was dropped; status reflects that
    }
    out_status->connected = snapshot.connected != 0u ? 1u : 0u;
    out_status->helper_pid = snapshot.helper_pid;
    out_status->state = static_cast<uint32_t>(snapshot.state);
    out_status->inflight = snapshot.inflight;
    out_status->respawn_attempts = snapshot.respawn_attempts;
    out_status->epoch = snapshot.epoch;
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_status(void* /*ctx_impl*/, uint32_t timeout_ms,
                           SaoSdkVtStatusSnapshot* out_status,
                           int32_t* out_operation_status,
                           uint32_t* out_response_flags) {
    if (out_status == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyStatusResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_status_v2(
        g_vt_runtime, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    std::memcpy(out_status, &response.status, sizeof(response.status));
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_capabilities(void* /*ctx_impl*/, uint32_t timeout_ms,
                                 SaoSdkVtCapabilities* out_caps,
                                 int32_t* out_operation_status,
                                 uint32_t* out_response_flags) {
    if (out_caps == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyCapabilitiesResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_capabilities(
        g_vt_runtime, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    std::memcpy(out_caps, &response.capabilities, sizeof(response.capabilities));
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_probe(void* /*ctx_impl*/, uint64_t items_mask,
                          uint32_t timeout_ms, uint64_t* out_completed_mask,
                          uint64_t* out_ready_mask, int32_t out_item_status[4],
                          int32_t* out_operation_status,
                          uint32_t* out_response_flags) {
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyProbeResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_probe(
        g_vt_runtime, items_mask, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_completed_mask != nullptr)
        *out_completed_mask = response.completed_mask;
    if (out_ready_mask != nullptr)
        *out_ready_mask = response.ready_mask;
    if (out_item_status != nullptr) {
        for (size_t i = 0; i < 4; ++i)
            out_item_status[i] = response.item_status[i];
    }
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_list_hooks(void* /*ctx_impl*/, uint32_t filter,
                               SaoSdkVtHookRow* out_rows, size_t row_capacity,
                               size_t* out_count, uint32_t timeout_ms,
                               int32_t* out_operation_status,
                               uint32_t* out_response_flags) {
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyHookRow rows[SAO_RT_IO_VT_PROXY_MAX_HOOKS]{};
    SaoRtIoVtProxyListResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    size_t count = 0;
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_list_hooks(
        g_vt_runtime, filter, rows, SAO_RT_IO_VT_PROXY_MAX_HOOKS, &count,
        timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    const size_t copy_count =
        count < row_capacity ? count : row_capacity;
    if (copy_count > 0 && out_rows == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < copy_count; ++i)
        std::memcpy(&out_rows[i], &rows[i], sizeof(SaoRtIoVtProxyHookRow));
    if (out_count != nullptr)
        *out_count = count;
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_perf_stats(void* /*ctx_impl*/, uint32_t timeout_ms,
                               SaoSdkVtPerfStats* out_stats,
                               int32_t* out_operation_status,
                               uint32_t* out_response_flags) {
    if (out_stats == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyPerfResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_perf_stats(
        g_vt_runtime, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    std::memcpy(out_stats, &response.stats, sizeof(response.stats));
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_read_phys(void* /*ctx_impl*/, uint64_t gpa,
                              uint8_t* out_buf, size_t out_capacity,
                              size_t* out_bytes, uint32_t timeout_ms,
                              int32_t* out_operation_status,
                              uint32_t* out_response_flags) {
    if (out_buf == nullptr || out_capacity == 0 ||
        out_capacity > SAO_RT_IO_VT_PROXY_MAX_TRANSFER)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyTransferResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    size_t bytes = 0;
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_read_phys(
        g_vt_runtime, gpa, out_capacity, out_buf, out_capacity, &bytes,
        timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_bytes != nullptr)
        *out_bytes = bytes;
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_write_phys(void* /*ctx_impl*/, uint64_t gpa,
                               const uint8_t* data, size_t size,
                               size_t* out_bytes, uint32_t timeout_ms,
                               int32_t* out_operation_status,
                               uint32_t* out_response_flags) {
    if (data == nullptr || size == 0 || size > SAO_RT_IO_VT_PROXY_MAX_TRANSFER)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyTransferResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_write_phys(
        g_vt_runtime, gpa, data, size, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_bytes != nullptr)
        *out_bytes = response.bytes_completed;
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_hook_page(void* /*ctx_impl*/, uint64_t kernel_gva,
                              const uint8_t* patch, size_t patch_size,
                              uint32_t timeout_ms, uint64_t* out_hook_id,
                              uint64_t* out_gpa, int32_t* out_operation_status,
                              uint32_t* out_response_flags) {
    if (patch == nullptr || patch_size == 0 ||
        patch_size > SAO_RT_IO_VT_PROXY_MAX_PATCH)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyHookPageResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_hook_page(
        g_vt_runtime, kernel_gva, patch, patch_size, timeout_ms, &response,
        &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_hook_id != nullptr)
        *out_hook_id = response.hook_id;
    if (out_gpa != nullptr)
        *out_gpa = response.gpa;
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_hide_region(void* /*ctx_impl*/, uint64_t kernel_gva,
                                uint32_t page_count, uint32_t decoy_mode,
                                const uint8_t* template_bytes,
                                size_t template_size, uint32_t timeout_ms,
                                uint64_t* out_hook_ids,
                                size_t hook_ids_capacity,
                                uint32_t* out_installed_count,
                                int32_t* out_operation_status,
                                uint32_t* out_response_flags) {
    if (page_count == 0 || page_count > SAO_RT_IO_VT_PROXY_MAX_HIDE_PAGES)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (decoy_mode == SAO_RT_IO_VT_PROXY_DECOY_TEMPLATE &&
        (template_bytes == nullptr || template_size == 0 ||
         template_size > SAO_RT_IO_VT_PROXY_MAX_TEMPLATE))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyHideRegionResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_hide_region(
        g_vt_runtime, kernel_gva, page_count, decoy_mode, template_bytes,
        template_size, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    // hook_ids[] has MAX_HIDE_PAGES slots; installed_count beyond that
    // is still reported raw in out_installed_count but never copied.
    const size_t installed =
        response.installed_count < SAO_RT_IO_VT_PROXY_MAX_HIDE_PAGES
            ? response.installed_count
            : SAO_RT_IO_VT_PROXY_MAX_HIDE_PAGES;
    const size_t copy_count =
        installed < hook_ids_capacity ? installed : hook_ids_capacity;
    if (copy_count > 0 && out_hook_ids == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < copy_count; ++i)
        out_hook_ids[i] = response.hook_ids[i];
    if (out_installed_count != nullptr)
        *out_installed_count = response.installed_count;
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_unhook(void* /*ctx_impl*/, uint64_t hook_id,
                           uint32_t timeout_ms, int32_t* out_operation_status,
                           uint32_t* out_response_flags) {
    if (hook_id == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtProxyUnhookResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_unhook(
        g_vt_runtime, hook_id, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

sao_sdk_status_t vt_map_user(void* /*ctx_impl*/,
                             const SaoSdkVtMapUserRequest* request,
                             uint32_t timeout_ms,
                             SaoSdkVtMapUserRequest* out_mapping,
                             int32_t* out_operation_status,
                             uint32_t* out_response_flags) {
    if (request == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_vt_runtime_mutex);
    const sao_status_t runtime_status = sync_locked();
    if (runtime_status != SAO_STATUS_OK)
        return SAO_SDK_ERR_UNSUPPORTED;
    SaoRtIoVtMapUserV1 wire_request{};
    std::memcpy(&wire_request, request, sizeof(wire_request));
    SaoRtIoVtProxyMapUserResponse response{};
    SaoRtIoProxyRuntimeCallResult call{};
    const sao_status_t status = sao_rt_io_proxy_runtime_vt_map_user(
        g_vt_runtime, &wire_request, timeout_ms, &response, &call);
    if (map_call_status(status, call) != SAO_SDK_OK) {
        invalidate_locked();
        return SAO_SDK_ERR_INTERNAL;
    }
    if (out_mapping != nullptr)
        std::memcpy(out_mapping, &response.mapping, sizeof(response.mapping));
    fill_prefix(response.prefix, out_operation_status, out_response_flags);
    return SAO_SDK_OK;
}

const SaoSdkVtTable kVtTable = {
    /*provider_status*/ vt_provider_status,
    /*status*/ vt_status,
    /*capabilities*/ vt_capabilities,
    /*probe*/ vt_probe,
    /*list_hooks*/ vt_list_hooks,
    /*perf_stats*/ vt_perf_stats,
    /*read_phys*/ vt_read_phys,
    /*write_phys*/ vt_write_phys,
    /*hook_page*/ vt_hook_page,
    /*hide_region*/ vt_hide_region,
    /*unhook*/ vt_unhook,
    /*map_user*/ vt_map_user,
    /*abi_version*/ SAO_SDK_VT_TABLE_ABI_VERSION,
    /*struct_size*/ sizeof(SaoSdkVtTable),
};

} // namespace

const SaoSdkVtTable* make_vt_table() {
    return &kVtTable;
}

} // namespace sao_sdk_internal
