// binding_engine_vt.cpp — 反射引擎面 VT 组：SaoSdkContext::vt
// (SaoSdkVtTable) 全槽位 → "vt.<fn>" 目录项。
//
// VT-Splitview hypervisor 控制面（status / capabilities / probe /
// perf_stats / list_hooks / read_phys / write_phys / hook_page /
// hide_region / unhook / map_user）+ 进程级 provider_status。
//
// 数据约定：
//   * 物理/内核地址、gpa、hook_id、mapping_id、cr3 等句柄级值用 u64；
//   * 二进制载荷（read_phys 结果、patch、template、write_phys 数据）
//     一律 base64 字符串字段（data_b64 / patch_b64 / template_b64）；
//   * 每个槽的 result 附 operation_status / response_flags 前缀字段，
//     反映 driver 自身操作状态（与槽返回的 sdk status 分离）。
//
// 组级 probe = ctx->vt 非空；每条 desc 的 availability 再按表
// struct_size offsetof 门控具体槽（镜像 gpu_hunt 组的 per-slot 门）。
// 运行时可用性（helper/driver 未就绪）不影响目录 available 标志 —
// invoke 侧按 fail-closed 返回 SAO_SDK_ERR_UNSUPPORTED。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include "sao/sdk/sao_sdk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sao::plugins::sdk_binding {
namespace {

bool vt_probe_ctx(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr && ctx->vt != nullptr;
}

// 槽位存在门：表非空 + struct_size 覆盖该槽 + 槽非空。
#define VT_SLOT_AVAILABLE(ctx, slot)                                                      \
    (vt_probe_ctx(ctx) &&                                                                 \
     (ctx)->vt->struct_size >= offsetof(struct SaoSdkVtTable, slot) +                     \
                                  sizeof(((struct SaoSdkVtTable*)0)->slot) &&            \
     (ctx)->vt->slot != nullptr)
#define VT_PROBE(slot)                                                                    \
    [](const SaoSdkContext* probe_ctx) noexcept {                                         \
        return VT_SLOT_AVAILABLE(probe_ctx, slot);                                        \
    }

// 参数读取失败 → 协议错误。
int32_t vt_bad_args() noexcept { return SAO_ERR_INVALID_ARGUMENT; }

// 统一结果尾：把 operation_status/response_flags 拼进 result JSON。
engine_json vt_prefix_tail(int32_t operation_status, uint32_t response_flags) {
    engine_json out = engine_json::object();
    out["operation_status"] = operation_status;
    out["response_flags"] = response_flags;
    return out;
}

// ── invoke 实现 ──────────────────────────────────────────────

int32_t inv_vt_provider_status(const SaoSdkContext* ctx, const engine_json&,
                               sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, provider_status))
        return engine_no_provider();
    SaoSdkVtProviderStatus status{};
    const auto rc = sao_sdk_vt_provider_status(ctx, &status);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out;
    out["handle_created"] = status.handle_created != 0u;
    out["connected"] = status.connected != 0u;
    out["helper_pid"] = status.helper_pid;
    out["state"] = status.state;
    out["inflight"] = status.inflight;
    out["respawn_attempts"] = status.respawn_attempts;
    out["epoch"] = status.epoch;
    return engine_result(request, out);
}

int32_t inv_vt_status(const SaoSdkContext* ctx, const engine_json& args,
                      sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, status))
        return engine_no_provider();
    uint32_t timeout_ms = 0;
    if (!engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    SaoSdkVtStatusSnapshot snapshot{};
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_status(ctx, timeout_ms, &snapshot, &operation_status,
                                      &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["state"] = snapshot.state;
    out["abi_version"] = snapshot.abi_version;
    out["root_cpu_count"] = snapshot.root_cpu_count;
    out["armed_hooks"] = snapshot.armed_hooks;
    out["mapped_base"] = snapshot.mapped_base;
    out["heartbeat_age_ms"] = snapshot.heartbeat_age_ms;
    out["engine_state"] = snapshot.engine_state;
    out["status_flags"] = snapshot.status_flags;
    out["availability_reason"] = snapshot.availability_reason;
    out["prior_hypervisor_kind"] = snapshot.prior_hypervisor_kind;
    out["prior_hypervisor_signature"] =
        engine_b64_encode(snapshot.prior_hypervisor_signature,
                          sizeof(snapshot.prior_hypervisor_signature));
    out["code_integrity_state"] = snapshot.code_integrity_state;
    out["code_integrity_options"] = snapshot.code_integrity_options;
    return engine_result(request, out);
}

int32_t inv_vt_capabilities(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, capabilities))
        return engine_no_provider();
    uint32_t timeout_ms = 0;
    if (!engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    SaoSdkVtCapabilities caps{};
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_capabilities(ctx, timeout_ms, &caps,
                                            &operation_status, &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["version"] = caps.version;
    out["vendor"] = caps.vendor;
    out["supported_flags"] = caps.supported_flags;
    out["active_flags"] = caps.active_flags;
    out["raw_vmx_ept_vpid_cap"] = caps.raw_vmx_ept_vpid_cap;
    out["raw_svm_features"] = caps.raw_svm_features;
    out["cpu_count"] = caps.cpu_count;
    out["max_cpu_count"] = caps.max_cpu_count;
    return engine_result(request, out);
}

int32_t inv_vt_probe(const SaoSdkContext* ctx, const engine_json& args,
                     sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, probe))
        return engine_no_provider();
    uint64_t items_mask = SAO_SDK_VT_PROBE_ITEM_ALL;
    uint32_t timeout_ms = 0;
    if (!engine_arg_u64(args, "items_mask", &items_mask, SAO_SDK_VT_PROBE_ITEM_ALL,
                        true) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    uint64_t completed_mask = 0, ready_mask = 0;
    int32_t item_status[4] = {};
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_probe(ctx, items_mask, timeout_ms, &completed_mask,
                                     &ready_mask, item_status, &operation_status,
                                     &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["completed_mask"] = completed_mask;
    out["ready_mask"] = ready_mask;
    engine_json items = engine_json::array();
    for (int i = 0; i < 4; ++i)
        items.push_back(item_status[i]);
    out["item_status"] = items;
    return engine_result(request, out);
}

int32_t inv_vt_perf_stats(const SaoSdkContext* ctx, const engine_json& args,
                          sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, perf_stats))
        return engine_no_provider();
    uint32_t timeout_ms = 0;
    if (!engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    SaoSdkVtPerfStats stats{};
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_perf_stats(ctx, timeout_ms, &stats,
                                          &operation_status, &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["version"] = stats.version;
    out["cpu_count"] = stats.cpu_count;
    engine_json counts = engine_json::array();
    const uint32_t cpu_used = stats.cpu_count > 64u ? 64u : stats.cpu_count;
    for (uint32_t i = 0; i < cpu_used; ++i)
        counts.push_back(stats.vmexit_count[i]);
    out["vmexit_count"] = counts;
    engine_json buckets = engine_json::array();
    for (int i = 0; i < 16; ++i)
        buckets.push_back(stats.vmexit_buckets[i]);
    out["vmexit_buckets"] = buckets;
    return engine_result(request, out);
}

int32_t inv_vt_list_hooks(const SaoSdkContext* ctx, const engine_json& args,
                          sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, list_hooks))
        return engine_no_provider();
    uint32_t filter = SAO_SDK_VT_LIST_FILTER_NONE;
    uint32_t timeout_ms = 0;
    if (!engine_arg_u32(args, "filter", &filter, SAO_SDK_VT_LIST_FILTER_NONE,
                        true) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    SaoSdkVtHookRow rows[SAO_SDK_VT_MAX_HOOKS]{};
    size_t count = 0;
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_list_hooks(ctx, filter, rows, SAO_SDK_VT_MAX_HOOKS,
                                          &count, timeout_ms, &operation_status,
                                          &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json list = engine_json::array();
    const size_t emit = count > SAO_SDK_VT_MAX_HOOKS ? SAO_SDK_VT_MAX_HOOKS : count;
    for (size_t i = 0; i < emit; ++i) {
        engine_json row;
        row["hook_id"] = rows[i].hook_id;
        row["gpa"] = rows[i].gpa;
        row["write_count"] = rows[i].write_count;
        row["hook_flags"] = rows[i].hook_flags;
        row["patch_size"] = rows[i].patch_size;
        row["backend"] = rows[i].backend;
        row["state"] = rows[i].state;
        list.push_back(row);
    }
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["count"] = count;
    out["hooks"] = list;
    return engine_result(request, out);
}

int32_t inv_vt_read_phys(const SaoSdkContext* ctx, const engine_json& args,
                         sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, read_phys))
        return engine_no_provider();
    uint64_t gpa = 0, size = 0;
    uint32_t timeout_ms = 0;
    if (!engine_arg_u64(args, "gpa", &gpa) ||
        !engine_arg_u64(args, "size", &size) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    if (size == 0 || size > SAO_SDK_VT_MAX_TRANSFER)
        return vt_bad_args();
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    size_t bytes = 0;
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_read_phys(ctx, gpa, buffer.data(), buffer.size(),
                                         &bytes, timeout_ms, &operation_status,
                                         &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["bytes"] = bytes;
    out["data_b64"] = engine_b64_encode(buffer.data(), bytes);
    return engine_result(request, out);
}

int32_t inv_vt_write_phys(const SaoSdkContext* ctx, const engine_json& args,
                          sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, write_phys))
        return engine_no_provider();
    uint64_t gpa = 0;
    uint32_t timeout_ms = 0;
    std::vector<uint8_t> data;
    if (!engine_arg_u64(args, "gpa", &gpa) ||
        !engine_arg_bytes(args, "data_b64", &data) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    size_t bytes = 0;
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_write_phys(ctx, gpa, data.data(), data.size(),
                                          &bytes, timeout_ms, &operation_status,
                                          &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["bytes"] = bytes;
    return engine_result(request, out);
}

int32_t inv_vt_hook_page(const SaoSdkContext* ctx, const engine_json& args,
                         sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, hook_page))
        return engine_no_provider();
    uint64_t gva = 0;
    uint32_t timeout_ms = 0;
    std::vector<uint8_t> patch;
    if (!engine_arg_u64(args, "kernel_gva", &gva) ||
        !engine_arg_bytes(args, "patch_b64", &patch) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    uint64_t hook_id = 0, gpa = 0;
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_hook_page(ctx, gva, patch.data(), patch.size(),
                                         timeout_ms, &hook_id, &gpa,
                                         &operation_status, &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["hook_id"] = hook_id;
    out["gpa"] = gpa;
    return engine_result(request, out);
}

int32_t inv_vt_hide_region(const SaoSdkContext* ctx, const engine_json& args,
                           sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, hide_region))
        return engine_no_provider();
    uint64_t gva = 0;
    uint32_t page_count = 0, decoy_mode = SAO_SDK_VT_DECOY_ZERO, timeout_ms = 0;
    std::vector<uint8_t> tmpl;
    if (!engine_arg_u64(args, "kernel_gva", &gva) ||
        !engine_arg_u32(args, "page_count", &page_count) ||
        !engine_arg_u32(args, "decoy_mode", &decoy_mode, SAO_SDK_VT_DECOY_ZERO,
                        true) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    // template_b64 仅 DECOY_TEMPLATE 模式必需；缺省视为空模板。
    if (args.contains("template_b64") &&
        !engine_arg_bytes(args, "template_b64", &tmpl))
        return vt_bad_args();
    uint64_t hook_ids[SAO_SDK_VT_MAX_HIDE_PAGES] = {};
    uint32_t installed = 0;
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_hide_region(
        ctx, gva, page_count, decoy_mode,
        tmpl.empty() ? nullptr : tmpl.data(), tmpl.size(), timeout_ms, hook_ids,
        SAO_SDK_VT_MAX_HIDE_PAGES, &installed, &operation_status,
        &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json ids = engine_json::array();
    for (uint32_t i = 0; i < installed && i < SAO_SDK_VT_MAX_HIDE_PAGES; ++i)
        ids.push_back(hook_ids[i]);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["installed_count"] = installed;
    out["hook_ids"] = ids;
    return engine_result(request, out);
}

int32_t inv_vt_unhook(const SaoSdkContext* ctx, const engine_json& args,
                      sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, unhook))
        return engine_no_provider();
    uint64_t hook_id = 0;
    uint32_t timeout_ms = 0;
    if (!engine_arg_u64(args, "hook_id", &hook_id) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_unhook(ctx, hook_id, timeout_ms, &operation_status,
                                      &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    return engine_result(request, vt_prefix_tail(operation_status, response_flags));
}

int32_t inv_vt_map_user(const SaoSdkContext* ctx, const engine_json& args,
                        sdk_context_call_request* request) {
    if (!VT_SLOT_AVAILABLE(ctx, map_user))
        return engine_no_provider();
    SaoSdkVtMapUserRequest req{};
    req.struct_size = sizeof(req);
    req.abi_version = SAO_SDK_VT_MAP_USER_ABI_VERSION;
    uint32_t timeout_ms = 0;
    if (!engine_arg_u32(args, "operation", &req.operation) ||
        !engine_arg_u32(args, "flags", &req.flags,
                        SAO_SDK_VT_MAP_USER_F_READ_ONLY, true) ||
        !engine_arg_u64(args, "target_pid", &req.target_pid, 0, true) ||
        !engine_arg_u64(args, "target_cr3", &req.target_cr3, 0, true) ||
        !engine_arg_u64(args, "target_user_va", &req.target_user_va, 0, true) ||
        !engine_arg_u64(args, "source_gpa", &req.source_gpa, 0, true) ||
        !engine_arg_u64(args, "expected_pte_gpa", &req.expected_pte_gpa, 0,
                        true) ||
        !engine_arg_u64(args, "expected_pte_value", &req.expected_pte_value, 0,
                        true) ||
        !engine_arg_u64(args, "mapping_id", &req.mapping_id, 0, true) ||
        !engine_arg_u64(args, "generation", &req.generation, 0, true) ||
        !engine_arg_u32(args, "timeout_ms", &timeout_ms, 0, true))
        return vt_bad_args();
    SaoSdkVtMapUserRequest mapping{};
    int32_t operation_status = 0;
    uint32_t response_flags = 0;
    const auto rc = sao_sdk_vt_map_user(ctx, &req, timeout_ms, &mapping,
                                        &operation_status, &response_flags);
    if (rc != SAO_SDK_OK)
        return static_cast<int32_t>(rc);
    engine_json out = vt_prefix_tail(operation_status, response_flags);
    out["status"] = mapping.status;
    out["operation"] = mapping.operation;
    out["mapping_id"] = mapping.mapping_id;
    out["generation"] = mapping.generation;
    out["source_gpa"] = mapping.source_gpa;
    out["target_user_va"] = mapping.target_user_va;
    return engine_result(request, out);
}

// ── 参数名表 ─────────────────────────────────────────────────

const char* const kArgsTimeout[] = {"timeout_ms"};
const char* const kArgsProbe[] = {"items_mask", "timeout_ms"};
const char* const kArgsListHooks[] = {"filter", "timeout_ms"};
const char* const kArgsReadPhys[] = {"gpa", "size", "timeout_ms"};
const char* const kArgsWritePhys[] = {"gpa", "data_b64", "timeout_ms"};
const char* const kArgsHookPage[] = {"kernel_gva", "patch_b64", "timeout_ms"};
const char* const kArgsHideRegion[] = {"kernel_gva", "page_count", "decoy_mode",
                                      "template_b64", "timeout_ms"};
const char* const kArgsUnhook[] = {"hook_id", "timeout_ms"};
const char* const kArgsMapUser[] = {"operation", "flags", "target_pid",
                                   "target_cr3", "target_user_va", "source_gpa",
                                   "expected_pte_gpa", "expected_pte_value",
                                   "mapping_id", "generation", "timeout_ms"};

// ── 条目表 ───────────────────────────────────────────────────

const sdk_engine_function_desc kEngineVtDescs[] = {
    {"vt.provider_status", nullptr, 0, &inv_vt_provider_status,
     VT_PROBE(provider_status)},
    {"vt.status", kArgsTimeout, 1, &inv_vt_status, VT_PROBE(status)},
    {"vt.capabilities", kArgsTimeout, 1, &inv_vt_capabilities,
     VT_PROBE(capabilities)},
    {"vt.probe", kArgsProbe, 2, &inv_vt_probe, VT_PROBE(probe)},
    {"vt.perf_stats", kArgsTimeout, 1, &inv_vt_perf_stats,
     VT_PROBE(perf_stats)},
    {"vt.list_hooks", kArgsListHooks, 2, &inv_vt_list_hooks,
     VT_PROBE(list_hooks)},
    {"vt.read_phys", kArgsReadPhys, 3, &inv_vt_read_phys,
     VT_PROBE(read_phys)},
    {"vt.write_phys", kArgsWritePhys, 3, &inv_vt_write_phys,
     VT_PROBE(write_phys)},
    {"vt.hook_page", kArgsHookPage, 3, &inv_vt_hook_page,
     VT_PROBE(hook_page)},
    {"vt.hide_region", kArgsHideRegion, 5, &inv_vt_hide_region,
     VT_PROBE(hide_region)},
    {"vt.unhook", kArgsUnhook, 2, &inv_vt_unhook, VT_PROBE(unhook)},
    {"vt.map_user", kArgsMapUser, 11, &inv_vt_map_user, VT_PROBE(map_user)},
};

} // namespace

// 组表：probe 只要求 ctx->vt 表存活（槽级门控在各 desc.availability）。
const sdk_engine_group_table kEngineGroupVt = {
    kEngineVtDescs,
    sizeof(kEngineVtDescs) / sizeof(kEngineVtDescs[0]),
    &vt_probe_ctx,
};

} // namespace sao::plugins::sdk_binding
