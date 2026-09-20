// binding_engine_gpuhunt.cpp — 反射引擎面 gpu_hunt 组表。
//
// 将 SaoSdkGpuHuntTable 的每一个 vtable 槽映射为 "gpu_hunt.<slot>" 目录项，
// 供五种脚本宿主经 method_engine_call 调用。约定：
//   - tracker 句柄经 u64 参数 "tracker" 传入；object_id/pid/index 等按 C
//     形参同名传递。
//   - mat4 → 16 个 double 数组；vec3 → {"x","y","z"} 对象（输入同形，
//     也接受 [x,y,z] 数组）。
//   - 结构体输出 → 映射每个公开字段的 JSON 对象（reserved 填充除外）。
//   - buffer 型槽走两段式容量模式：先 (nullptr,0) 取 required count，再按
//     可选 "max" 参数封顶分配第二次调用；result 为序列化元素数组。
//   - 带 struct_size 的结构体入参/缓冲在调用前写入 sizeof（ABI fill）。
//
// 可用性：组 probe = ctx->gpu_hunt != nullptr；每条目录项带 per-slot
// probe（表 struct_size 覆盖该槽偏移 + 槽指针非空）。probe 失败 →
// catalog 标 available=false；invoke 时同样校验并返回
// engine_no_provider()。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include "sao/sdk/sao_sdk_context.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sao::plugins::sdk_binding {
namespace {

// 序列化元素硬上限：JSON envelope 上限 kMaximumBindingJsonBytes（8 MiB），
// 超过该规模的批量结果注定无法返回，直接在分配前截断。
constexpr size_t kGpuHuntMaxElements = 65536;

const SaoSdkGpuHuntTable* gpu_table(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr ? ctx->gpu_hunt : nullptr;
}

// 槽位存在门：镜像 SAO_SDK_GPU_HUNT_REQUIRE_SLOT —— 表非空、表自声明
// struct_size 覆盖该槽、槽指针非空。struct_size 读取本身是 ABI 规定的
// 授权机制（宿主线程总是填全表，见 sdk_gpu_hunt_wire.cpp）。
#define GPU_HUNT_SLOT_AVAILABLE(ctx, slot)                                     \
    (gpu_table(ctx) != nullptr &&                                              \
     gpu_table(ctx)->struct_size >=                                            \
         offsetof(SaoSdkGpuHuntTable, slot) +                                  \
             sizeof(((SaoSdkGpuHuntTable*)nullptr)->slot) &&                   \
     gpu_table(ctx)->slot != nullptr)

#define GPU_HUNT_PROBE(slot)                                                   \
    [](const SaoSdkContext* probe_ctx) noexcept -> bool {                      \
        return GPU_HUNT_SLOT_AVAILABLE(probe_ctx, slot);                       \
    }

// ── 序列化 ──────────────────────────────────────────────────────

engine_json vec3_to_json(const float v[3]) {
    engine_json out = engine_json::object();
    out["x"] = static_cast<double>(v[0]);
    out["y"] = static_cast<double>(v[1]);
    out["z"] = static_cast<double>(v[2]);
    return out;
}

engine_json mat4_to_json(const float m[16]) {
    engine_json out = engine_json::array();
    for (size_t index = 0; index < 16; ++index) {
        out.push_back(static_cast<double>(m[index]));
    }
    return out;
}

engine_json scene_object_to_json(const SaoSdkGpuHuntSceneObject& object) {
    engine_json out = engine_json::object();
    out["struct_size"] = object.struct_size;
    out["abi_version"] = object.abi_version;
    out["object_id"] = object.object_id;
    out["observation_epoch"] = object.observation_epoch;
    out["first_seen_tick"] = object.first_seen_tick;
    out["last_seen_tick"] = object.last_seen_tick;
    out["heap_base"] = object.heap_base;
    out["heap_offset"] = object.heap_offset;
    out["kind"] = object.kind;
    out["source"] = object.source;
    out["coordinate_space"] = object.coordinate_space;
    out["graph_source"] = object.graph_source;
    out["flags"] = object.flags;
    out["layout"] = object.layout;
    out["tier"] = object.tier;
    out["bone_count"] = object.bone_count;
    out["confidence"] = static_cast<double>(object.confidence);
    out["bounding_radius"] = static_cast<double>(object.bounding_radius);
    out["centroid"] = vec3_to_json(object.centroid);
    out["velocity"] = vec3_to_json(object.velocity);
    out["stale_ticks"] = object.stale_ticks;
    return out;
}

// ── 参数读取 ────────────────────────────────────────────────────

bool read_tracker(const engine_json& args, sao_sdk_gpu_tracker_t* out) noexcept {
    if (out == nullptr) return false;
    uint64_t value = 0;
    if (!engine_arg_u64(args, "tracker", &value)) return false;
    *out = static_cast<sao_sdk_gpu_tracker_t>(value);
    return true;
}

// 可选 "max" 封顶；同时施加 kGpuHuntMaxElements 硬上限。
size_t requested_limit(const engine_json& args, size_t required) noexcept {
    uint64_t max_arg = 0;
    engine_arg_u64(args, "max", &max_arg, 0, /*optional=*/true);
    size_t limit = required;
    if (limit > kGpuHuntMaxElements) limit = kGpuHuntMaxElements;
    if (max_arg != 0 && max_arg < limit) limit = static_cast<size_t>(max_arg);
    return limit;
}

bool read_vec3_node(const engine_json& node, float out[3]) noexcept {
    if (out == nullptr) return false;
    try {
        double values[3] = {};
        if (node.is_object()) {
            const auto x = node.find("x");
            const auto y = node.find("y");
            const auto z = node.find("z");
            if (x == node.end() || y == node.end() || z == node.end() ||
                !x->is_number() || !y->is_number() || !z->is_number()) {
                return false;
            }
            values[0] = x->get<double>();
            values[1] = y->get<double>();
            values[2] = z->get<double>();
        } else if (node.is_array() && node.size() >= 3) {
            for (size_t index = 0; index < 3; ++index) {
                const auto& item = node.at(index);
                if (!item.is_number()) return false;
                values[index] = item.get<double>();
            }
        } else {
            return false;
        }
        for (size_t index = 0; index < 3; ++index) {
            if (!std::isfinite(values[index])) return false;
            out[index] = static_cast<float>(values[index]);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool read_f32_array_node(const engine_json& node,
                         std::vector<float>* out) noexcept {
    if (out == nullptr || !node.is_array() ||
        node.size() > kGpuHuntMaxElements) {
        return false;
    }
    try {
        out->clear();
        out->reserve(node.size());
        for (const auto& item : node) {
            if (!item.is_number()) return false;
            const double value = item.get<double>();
            if (!std::isfinite(value)) return false;
            out->push_back(static_cast<float>(value));
        }
        return true;
    } catch (...) {
        return false;
    }
}

// hot-heap 元素 = 4 x u64 (size, protect, region_type, header_8b)。
// "heaps" 接受对象数组，"flat" 接受扁平 u64 数组（长度须为 4 的倍数）。
bool read_hot_heaps_node(const engine_json& node,
                         std::vector<uint64_t>* out) noexcept {
    if (out == nullptr || !node.is_array() ||
        node.size() > kGpuHuntMaxElements * 4u) {
        return false;
    }
    try {
        out->clear();
        if (node.empty()) return true;
        const auto& first = node.front();
        if (first.is_object()) {
            if (node.size() > kGpuHuntMaxElements) return false;
            out->reserve(node.size() * 4u);
            static const char* const kFields[4] = {
                "size", "protect", "region_type", "header_8b"};
            for (const auto& entry : node) {
                if (!entry.is_object()) return false;
                for (const char* field : kFields) {
                    const auto found = entry.find(field);
                    if (found == entry.end() || !found->is_number_unsigned()) {
                        return false;
                    }
                    out->push_back(found->get<uint64_t>());
                }
            }
            return true;
        }
        out->reserve(node.size());
        for (const auto& item : node) {
            if (!item.is_number_unsigned()) return false;
            out->push_back(item.get<uint64_t>());
        }
        return out->size() % 4u == 0;
    } catch (...) {
        return false;
    }
}

// ── invoke 防护：分配 / JSON 构造异常统一收敛为 OS_CALL_FAILED ──
template <typename Fn>
int32_t invoke_guard(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── 宏：tracker-only 无输出操作 ──────────────────────────────────
#define GPU_HUNT_SIMPLE_OP(fn_name, slot)                                      \
    int32_t fn_name(const SaoSdkContext* ctx, const engine_json& args,         \
                    sdk_context_call_request* request) {                       \
        return invoke_guard([&]() -> int32_t {                                              \
            if (!GPU_HUNT_SLOT_AVAILABLE(ctx, slot))                           \
                return engine_no_provider();                                   \
            sao_sdk_gpu_tracker_t tracker = 0;                                 \
            if (!read_tracker(args, &tracker))                                 \
                return SAO_ERR_INVALID_ARGUMENT;                               \
            const sao_sdk_status_t status =                                    \
                gpu_table(ctx)->slot(ctx->ctx_impl, tracker);                  \
            if (status != SAO_SDK_OK) return status;                           \
            return engine_result(request, true);                               \
        });                                                                    \
    }

// ── 宏：matrix16 + locked 输出 ────────────────────────────────────
#define GPU_HUNT_MATRIX_OP(fn_name, slot)                                      \
    int32_t fn_name(const SaoSdkContext* ctx, const engine_json& args,         \
                    sdk_context_call_request* request) {                       \
        return invoke_guard([&]() -> int32_t {                                              \
            if (!GPU_HUNT_SLOT_AVAILABLE(ctx, slot))                           \
                return engine_no_provider();                                   \
            sao_sdk_gpu_tracker_t tracker = 0;                                 \
            if (!read_tracker(args, &tracker))                                 \
                return SAO_ERR_INVALID_ARGUMENT;                               \
            float matrix[16] = {};                                             \
            uint8_t locked = 0;                                                \
            const sao_sdk_status_t status = gpu_table(ctx)->slot(              \
                ctx->ctx_impl, tracker, matrix, &locked);                      \
            if (status != SAO_SDK_OK) return status;                           \
            engine_json out = engine_json::object();                           \
            out["matrix"] = mat4_to_json(matrix);                              \
            out["locked"] = locked != 0;                                       \
            return engine_result(request, out);                                \
        });                                                                    \
    }

// ── 宏：vec3 输出 ─────────────────────────────────────────────────
#define GPU_HUNT_VEC3_OP(fn_name, slot)                                        \
    int32_t fn_name(const SaoSdkContext* ctx, const engine_json& args,         \
                    sdk_context_call_request* request) {                       \
        return invoke_guard([&]() -> int32_t {                                              \
            if (!GPU_HUNT_SLOT_AVAILABLE(ctx, slot))                           \
                return engine_no_provider();                                   \
            sao_sdk_gpu_tracker_t tracker = 0;                                 \
            if (!read_tracker(args, &tracker))                                 \
                return SAO_ERR_INVALID_ARGUMENT;                               \
            float pos[3] = {};                                                 \
            const sao_sdk_status_t status =                                    \
                gpu_table(ctx)->slot(ctx->ctx_impl, tracker, pos);             \
            if (status != SAO_SDK_OK) return status;                           \
            return engine_result(request, vec3_to_json(pos));                  \
        });                                                                    \
    }

GPU_HUNT_SIMPLE_OP(inv_destroy_tracker, destroy_tracker)
GPU_HUNT_SIMPLE_OP(inv_tick, tick)
GPU_HUNT_SIMPLE_OP(inv_invalidate, invalidate)
GPU_HUNT_SIMPLE_OP(inv_detach_tracker, detach_tracker)

GPU_HUNT_MATRIX_OP(inv_get_view_proj, get_view_proj)
GPU_HUNT_MATRIX_OP(inv_get_view_matrix, get_view_matrix)
GPU_HUNT_MATRIX_OP(inv_get_proj_matrix, get_proj_matrix)

GPU_HUNT_VEC3_OP(inv_get_camera_pos, get_camera_pos)
GPU_HUNT_VEC3_OP(inv_get_camera_world_pos, get_camera_world_pos)

// ── 生命周期 ────────────────────────────────────────────────────

int32_t inv_create_tracker(const SaoSdkContext* ctx, const engine_json&,
                           sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, create_tracker))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        const sao_sdk_status_t status =
            gpu_table(ctx)->create_tracker(ctx->ctx_impl, &tracker);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, tracker);
    });
}

int32_t inv_attach_tracker(const SaoSdkContext* ctx, const engine_json& args,
                           sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, attach_tracker))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint32_t pid = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u32(args, "pid", &pid)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const sao_sdk_status_t status =
            gpu_table(ctx)->attach_tracker(ctx->ctx_impl, tracker, pid);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

// ── 骨骼 / 簇 buffer（两段式容量模式 + "max" 封顶） ──────────────────

int32_t inv_get_skeleton_positions(const SaoSdkContext* ctx,
                                   const engine_json& args,
                                   sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_skeleton_positions))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        size_t required = 0;
        sao_sdk_status_t status = gpu_table(ctx)->get_skeleton_positions(
            ctx->ctx_impl, tracker, nullptr, 0, &required);
        if (status != SAO_SDK_OK) return status;
        const size_t limit = requested_limit(args, required);
        std::vector<float> positions(limit * 3u);
        size_t written = 0;
        if (limit != 0) {
            status = gpu_table(ctx)->get_skeleton_positions(
                ctx->ctx_impl, tracker, positions.data(), limit, &written);
            if (status != SAO_SDK_OK) return status;
        }
        written = std::min(written, limit);
        engine_json out = engine_json::array();
        for (size_t index = 0; index < written; ++index) {
            out.push_back(vec3_to_json(&positions[index * 3u]));
        }
        return engine_result(request, out);
    });
}

int32_t inv_get_bone_cluster_count(const SaoSdkContext* ctx,
                                   const engine_json& args,
                                   sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_bone_cluster_count))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        size_t count = 0;
        const sao_sdk_status_t status =
            gpu_table(ctx)->get_bone_cluster_count(ctx->ctx_impl, tracker,
                                                   &count);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, static_cast<uint64_t>(count));
    });
}

int32_t inv_get_bone_cluster_positions(const SaoSdkContext* ctx,
                                       const engine_json& args,
                                       sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_bone_cluster_positions))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint64_t index = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u64(args, "index", &index)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        size_t required = 0;
        sao_sdk_status_t status = gpu_table(ctx)->get_bone_cluster_positions(
            ctx->ctx_impl, tracker, static_cast<size_t>(index), nullptr, 0,
            &required);
        if (status != SAO_SDK_OK) return status;
        const size_t limit = requested_limit(args, required);
        std::vector<float> positions(limit * 3u);
        size_t written = 0;
        if (limit != 0) {
            status = gpu_table(ctx)->get_bone_cluster_positions(
                ctx->ctx_impl, tracker, static_cast<size_t>(index),
                positions.data(), limit, &written);
            if (status != SAO_SDK_OK) return status;
        }
        // get_bone_cluster_positions 的 out_bone_count 返回真实簇大小而非
        // 拷贝数，序列化时仍需按 limit 截断。
        written = std::min(written, limit);
        engine_json out = engine_json::array();
        for (size_t element = 0; element < written; ++element) {
            out.push_back(vec3_to_json(&positions[element * 3u]));
        }
        return engine_result(request, out);
    });
}

// ── 投影 ────────────────────────────────────────────────────────

int32_t inv_world_to_screen(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, world_to_screen))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        float world_pos[3] = {};
        int32_t viewport_w = 0;
        int32_t viewport_h = 0;
        const auto pos_arg = args.find("world_pos");
        if (!read_tracker(args, &tracker) || pos_arg == args.end() ||
            !read_vec3_node(*pos_arg, world_pos) ||
            !engine_arg_i32(args, "viewport_w", &viewport_w) ||
            !engine_arg_i32(args, "viewport_h", &viewport_h)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        float screen_x = 0.0f;
        float screen_y = 0.0f;
        uint8_t visible = 0;
        const sao_sdk_status_t status = gpu_table(ctx)->world_to_screen(
            ctx->ctx_impl, tracker, world_pos, viewport_w, viewport_h,
            &screen_x, &screen_y, &visible);
        if (status != SAO_SDK_OK) return status;
        engine_json out = engine_json::object();
        out["screen_x"] = static_cast<double>(screen_x);
        out["screen_y"] = static_cast<double>(screen_y);
        out["visible"] = visible != 0;
        return engine_result(request, out);
    });
}

// ── 锁状态 / hint ───────────────────────────────────────────────

int32_t inv_get_matrix_lock_info(const SaoSdkContext* ctx,
                                 const engine_json& args,
                                 sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_matrix_lock_info))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        uint64_t heap_base = 0;
        uint32_t offset = 0;
        uint64_t heap_size = 0;
        uint8_t locked = 0;
        const sao_sdk_status_t status = gpu_table(ctx)->get_matrix_lock_info(
            ctx->ctx_impl, tracker, &heap_base, &offset, &heap_size, &locked);
        if (status != SAO_SDK_OK) return status;
        engine_json out = engine_json::object();
        out["heap_base"] = heap_base;
        out["offset"] = offset;
        out["heap_size"] = heap_size;
        out["locked"] = locked != 0;
        return engine_result(request, out);
    });
}

int32_t inv_set_prior_lock_hint(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, set_prior_lock_hint))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint64_t hint_heap_size = 0;
        uint32_t hint_offset = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u64(args, "hint_heap_size", &hint_heap_size)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        engine_arg_u32(args, "hint_offset", &hint_offset, 0, /*optional=*/true);
        const sao_sdk_status_t status = gpu_table(ctx)->set_prior_lock_hint(
            ctx->ctx_impl, tracker, hint_heap_size, hint_offset);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

int32_t inv_get_skeleton_fingerprint(const SaoSdkContext* ctx,
                                     const engine_json& args,
                                     sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_skeleton_fingerprint))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        size_t required = 0;
        sao_sdk_status_t status = gpu_table(ctx)->get_skeleton_fingerprint(
            ctx->ctx_impl, tracker, nullptr, 0, &required);
        if (status != SAO_SDK_OK) return status;
        const size_t limit = requested_limit(args, required);
        std::vector<float> floats(limit);
        size_t written = 0;
        if (limit != 0) {
            status = gpu_table(ctx)->get_skeleton_fingerprint(
                ctx->ctx_impl, tracker, floats.data(), limit, &written);
            if (status != SAO_SDK_OK) return status;
        }
        written = std::min(written, limit);
        engine_json out = engine_json::array();
        for (size_t index = 0; index < written; ++index) {
            out.push_back(static_cast<double>(floats[index]));
        }
        return engine_result(request, out);
    });
}

int32_t inv_set_skeleton_fingerprint_hint(const SaoSdkContext* ctx,
                                          const engine_json& args,
                                          sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, set_skeleton_fingerprint_hint))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        std::vector<float> floats;
        const auto floats_arg = args.find("floats");
        if (floats_arg != args.end() &&
            !read_f32_array_node(*floats_arg, &floats)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        // 参数缺省等价 C 侧 floats=NULL/count=0：仅清空 fingerprint 切片。
        const sao_sdk_status_t status =
            gpu_table(ctx)->set_skeleton_fingerprint_hint(
                ctx->ctx_impl, tracker,
                floats.empty() ? nullptr : floats.data(), floats.size());
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

// ── hot heaps ───────────────────────────────────────────────────

int32_t inv_get_prior_hot_heaps(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_prior_hot_heaps))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        size_t required = 0;
        sao_sdk_status_t status = gpu_table(ctx)->get_prior_hot_heaps(
            ctx->ctx_impl, tracker, nullptr, 0, &required);
        if (status != SAO_SDK_OK) return status;
        const size_t limit = requested_limit(args, required);
        std::vector<uint64_t> flat(limit * 4u);
        size_t written = 0;
        if (limit != 0) {
            status = gpu_table(ctx)->get_prior_hot_heaps(
                ctx->ctx_impl, tracker, flat.data(), limit, &written);
            if (status != SAO_SDK_OK) return status;
        }
        written = std::min(written, limit);
        engine_json out = engine_json::array();
        for (size_t index = 0; index < written; ++index) {
            const uint64_t* entry = &flat[index * 4u];
            engine_json item = engine_json::object();
            item["size"] = entry[0];
            item["protect"] = entry[1];
            item["region_type"] = entry[2];
            item["header_8b"] = entry[3];
            out.push_back(item);
        }
        return engine_result(request, out);
    });
}

int32_t inv_set_prior_hot_heaps(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, set_prior_hot_heaps))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        std::vector<uint64_t> flat;
        const auto heaps_arg = args.find("heaps");
        const auto flat_arg = args.find("flat");
        if (heaps_arg != args.end() || flat_arg != args.end()) {
            const engine_json& node =
                heaps_arg != args.end() ? *heaps_arg : *flat_arg;
            if (!read_hot_heaps_node(node, &flat)) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
        }
        // 参数缺省等价 C 侧 flat=NULL/count=0：仅清空 hot_heaps 切片。
        const sao_sdk_status_t status = gpu_table(ctx)->set_prior_hot_heaps(
            ctx->ctx_impl, tracker, flat.empty() ? nullptr : flat.data(),
            flat.size() / 4u);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

// ── pin / toggles / locator profile ──────────────────────────────

int32_t inv_set_pinned_heap(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, set_pinned_heap))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint64_t heap_base = 0;
        uint64_t heap_size = 0;
        bool pinned_only = false;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u64(args, "heap_base", &heap_base)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        engine_arg_u64(args, "heap_size", &heap_size, 0, /*optional=*/true);
        engine_arg_bool(args, "pinned_only", &pinned_only, false,
                        /*optional=*/true);
        const sao_sdk_status_t status = gpu_table(ctx)->set_pinned_heap(
            ctx->ctx_impl, tracker, heap_base, heap_size,
            static_cast<uint8_t>(pinned_only ? 1 : 0));
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

int32_t inv_get_pinned_heap(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_pinned_heap))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        uint64_t heap_base = 0;
        uint64_t heap_size = 0;
        uint8_t pinned_only = 0;
        uint8_t active = 0;
        const sao_sdk_status_t status = gpu_table(ctx)->get_pinned_heap(
            ctx->ctx_impl, tracker, &heap_base, &heap_size, &pinned_only,
            &active);
        if (status != SAO_SDK_OK) return status;
        engine_json out = engine_json::object();
        out["heap_base"] = heap_base;
        out["heap_size"] = heap_size;
        out["pinned_only"] = pinned_only != 0;
        out["active"] = active != 0;
        return engine_result(request, out);
    });
}

int32_t inv_get_runtime_toggles(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_runtime_toggles))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        uint64_t mask = 0;
        const sao_sdk_status_t status = gpu_table(ctx)->get_runtime_toggles(
            ctx->ctx_impl, tracker, &mask);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, mask);
    });
}

int32_t inv_set_runtime_toggles(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, set_runtime_toggles))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint64_t mask = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u64(args, "mask", &mask)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        uint64_t changed_mask = mask;
        engine_arg_u64(args, "changed_mask", &changed_mask, mask,
                       /*optional=*/true);
        const sao_sdk_status_t status = gpu_table(ctx)->set_runtime_toggles(
            ctx->ctx_impl, tracker, mask, changed_mask);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

int32_t inv_get_locator_profile(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_locator_profile))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        uint32_t profile = 0;
        const sao_sdk_status_t status = gpu_table(ctx)->get_locator_profile(
            ctx->ctx_impl, tracker, &profile);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, static_cast<uint64_t>(profile));
    });
}

int32_t inv_set_locator_profile(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, set_locator_profile))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint32_t profile = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u32(args, "profile", &profile)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const sao_sdk_status_t status = gpu_table(ctx)->set_locator_profile(
            ctx->ctx_impl, tracker, profile);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, true);
    });
}

// ── 分离锁 ──────────────────────────────────────────────────────

int32_t inv_get_split_lock_info(const SaoSdkContext* ctx,
                                const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_split_lock_info))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        uint64_t view_heap_base = 0;
        uint32_t view_offset = 0;
        uint64_t proj_heap_base = 0;
        uint32_t proj_offset = 0;
        uint8_t view_locked = 0;
        uint8_t proj_locked = 0;
        const sao_sdk_status_t status = gpu_table(ctx)->get_split_lock_info(
            ctx->ctx_impl, tracker, &view_heap_base, &view_offset,
            &proj_heap_base, &proj_offset, &view_locked, &proj_locked);
        if (status != SAO_SDK_OK) return status;
        engine_json out = engine_json::object();
        engine_json view = engine_json::object();
        view["heap_base"] = view_heap_base;
        view["offset"] = view_offset;
        view["locked"] = view_locked != 0;
        engine_json proj = engine_json::object();
        proj["heap_base"] = proj_heap_base;
        proj["offset"] = proj_offset;
        proj["locked"] = proj_locked != 0;
        out["view"] = view;
        out["proj"] = proj;
        return engine_result(request, out);
    });
}

// ── 运动预测尾部（ABI 1.11 可选槽） ─────────────────────────────────

int32_t inv_get_predicted_view(const SaoSdkContext* ctx,
                               const engine_json& args,
                               sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_predicted_view))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint32_t future_tick = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u32(args, "future_tick", &future_tick)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        float matrix[16] = {};
        float camera[3] = {};
        float confidence = 0.0f;
        const sao_sdk_status_t status = gpu_table(ctx)->get_predicted_view(
            ctx->ctx_impl, tracker, future_tick, matrix, camera, &confidence);
        if (status != SAO_SDK_OK) return status;
        engine_json out = engine_json::object();
        out["matrix"] = mat4_to_json(matrix);
        out["camera"] = vec3_to_json(camera);
        out["confidence"] = static_cast<double>(confidence);
        return engine_result(request, out);
    });
}

int32_t inv_get_motion_prediction_confidence(
    const SaoSdkContext* ctx, const engine_json& args,
    sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_motion_prediction_confidence))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        float confidence = 0.0f;
        const sao_sdk_status_t status =
            gpu_table(ctx)->get_motion_prediction_confidence(ctx->ctx_impl,
                                                             tracker,
                                                             &confidence);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, static_cast<double>(confidence));
    });
}

int32_t inv_get_motion_prediction_sample_count(
    const SaoSdkContext* ctx, const engine_json& args,
    sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_motion_prediction_sample_count))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        uint32_t count = 0;
        const sao_sdk_status_t status =
            gpu_table(ctx)->get_motion_prediction_sample_count(ctx->ctx_impl,
                                                               tracker,
                                                               &count);
        if (status != SAO_SDK_OK) return status;
        return engine_result(request, static_cast<uint64_t>(count));
    });
}

// ── 场景观察尾部（可选槽） ─────────────────────────────────────────

int32_t inv_get_scene_snapshot(const SaoSdkContext* ctx,
                               const engine_json& args,
                               sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_scene_snapshot))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        SaoSdkGpuHuntSceneSnapshot snapshot{};
        snapshot.struct_size = static_cast<uint32_t>(sizeof(snapshot));
        snapshot.abi_version = SAO_SDK_GPU_HUNT_SCENE_DATA_ABI_VERSION;
        const sao_sdk_status_t status = gpu_table(ctx)->get_scene_snapshot(
            ctx->ctx_impl, tracker, &snapshot);
        if (status != SAO_SDK_OK) return status;
        engine_json out = engine_json::object();
        out["struct_size"] = snapshot.struct_size;
        out["abi_version"] = snapshot.abi_version;
        out["observation_epoch"] = snapshot.observation_epoch;
        out["scene_revision"] = snapshot.scene_revision;
        out["sample_sequence"] = snapshot.sample_sequence;
        out["tick"] = snapshot.tick;
        out["object_count"] = snapshot.object_count;
        out["added_count"] = snapshot.added_count;
        out["updated_count"] = snapshot.updated_count;
        out["removed_count"] = snapshot.removed_count;
        out["matrix_locked"] = snapshot.matrix_locked != 0;
        out["camera_valid"] = snapshot.camera_valid != 0;
        out["camera_position"] = vec3_to_json(snapshot.camera_position);
        out["scene_confidence"] =
            static_cast<double>(snapshot.scene_confidence);
        return engine_result(request, out);
    });
}

int32_t inv_get_scene_objects(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_scene_objects))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        if (!read_tracker(args, &tracker)) return SAO_ERR_INVALID_ARGUMENT;
        size_t required = 0;
        sao_sdk_status_t status = gpu_table(ctx)->get_scene_objects(
            ctx->ctx_impl, tracker, nullptr, 0, &required);
        if (status != SAO_SDK_OK) return status;
        const size_t limit = requested_limit(args, required);
        std::vector<SaoSdkGpuHuntSceneObject> objects(limit);
        for (auto& object : objects) {
            object.struct_size = static_cast<uint32_t>(
                sizeof(SaoSdkGpuHuntSceneObject));
            object.abi_version = SAO_SDK_GPU_HUNT_SCENE_DATA_ABI_VERSION;
        }
        size_t written = 0;
        if (limit != 0) {
            status = gpu_table(ctx)->get_scene_objects(
                ctx->ctx_impl, tracker, objects.data(), limit, &written);
            if (status != SAO_SDK_OK) return status;
        }
        written = std::min(written, limit);
        engine_json out = engine_json::array();
        for (size_t index = 0; index < written; ++index) {
            out.push_back(scene_object_to_json(objects[index]));
        }
        return engine_result(request, out);
    });
}

int32_t inv_get_scene_object_bones(const SaoSdkContext* ctx,
                                   const engine_json& args,
                                   sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!GPU_HUNT_SLOT_AVAILABLE(ctx, get_scene_object_bones))
            return engine_no_provider();
        sao_sdk_gpu_tracker_t tracker = 0;
        uint64_t object_id = 0;
        if (!read_tracker(args, &tracker) ||
            !engine_arg_u64(args, "object_id", &object_id)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        size_t required = 0;
        sao_sdk_status_t status = gpu_table(ctx)->get_scene_object_bones(
            ctx->ctx_impl, tracker, object_id, nullptr, nullptr, 0, &required);
        if (status != SAO_SDK_OK) return status;
        const size_t limit = requested_limit(args, required);
        std::vector<float> positions(limit * 3u);
        std::vector<uint32_t> parents(limit);
        size_t written = 0;
        if (limit != 0) {
            status = gpu_table(ctx)->get_scene_object_bones(
                ctx->ctx_impl, tracker, object_id, positions.data(),
                parents.data(), limit, &written);
            if (status != SAO_SDK_OK) return status;
        }
        written = std::min(written, limit);
        engine_json out = engine_json::array();
        for (size_t index = 0; index < written; ++index) {
            engine_json item = vec3_to_json(&positions[index * 3u]);
            item["parent"] = parents[index];
            out.push_back(item);
        }
        return engine_result(request, out);
    });
}

// ── 表元数据（非函数槽，仅读报） ────────────────────────────────────

bool gpu_hunt_abi_version_present(const SaoSdkContext* ctx) noexcept {
    const auto* table = gpu_table(ctx);
    return table != nullptr &&
           table->struct_size >=
               offsetof(SaoSdkGpuHuntTable, abi_version) +
                   sizeof(((SaoSdkGpuHuntTable*)nullptr)->abi_version);
}

bool gpu_hunt_struct_size_present(const SaoSdkContext* ctx) noexcept {
    const auto* table = gpu_table(ctx);
    return table != nullptr &&
           table->struct_size >= SAO_SDK_GPU_HUNT_TABLE_REQUIRED_SIZE;
}

int32_t inv_abi_version(const SaoSdkContext* ctx, const engine_json&,
                        sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!gpu_hunt_abi_version_present(ctx)) return engine_no_provider();
        return engine_result(request, static_cast<uint64_t>(
                                          gpu_table(ctx)->abi_version));
    });
}

int32_t inv_struct_size(const SaoSdkContext* ctx, const engine_json&,
                        sdk_context_call_request* request) {
    return invoke_guard([&]() -> int32_t {
        if (!gpu_hunt_struct_size_present(ctx)) return engine_no_provider();
        return engine_result(request, static_cast<uint64_t>(
                                          gpu_table(ctx)->struct_size));
    });
}

// ── arg 名表 ────────────────────────────────────────────────────

const char* const kArgsTracker[] = {"tracker"};
const char* const kArgsTrackerPid[] = {"tracker", "pid"};
const char* const kArgsTrackerMax[] = {"tracker", "max"};
const char* const kArgsTrackerIndexMax[] = {"tracker", "index", "max"};
const char* const kArgsTrackerObjectIdMax[] = {"tracker", "object_id", "max"};
const char* const kArgsWorldToScreen[] = {"tracker", "world_pos", "viewport_w",
                                        "viewport_h"};
const char* const kArgsPriorLockHint[] = {"tracker", "hint_heap_size",
                                        "hint_offset"};
const char* const kArgsFingerprintHint[] = {"tracker", "floats"};
const char* const kArgsHotHeaps[] = {"tracker", "flat"};
const char* const kArgsPinnedHeap[] = {"tracker", "heap_base", "heap_size",
                                     "pinned_only"};
const char* const kArgsToggles[] = {"tracker", "mask", "changed_mask"};
const char* const kArgsProfile[] = {"tracker", "profile"};
const char* const kArgsPredictedView[] = {"tracker", "future_tick"};

#define GPU_HUNT_DESC(name_literal, names, invoke_fn, probe_fn)                \
    {name_literal, names, static_cast<uint32_t>(sizeof(names) / sizeof((names)[0])), \
     invoke_fn, probe_fn}

// ── 组表 ────────────────────────────────────────────────────────

const sdk_engine_function_desc kEngineFnsGpuHunt[] = {
    // 生命周期 / tick / 投影
    {"gpu_hunt.create_tracker", nullptr, 0, &inv_create_tracker,
     GPU_HUNT_PROBE(create_tracker)},
    GPU_HUNT_DESC("gpu_hunt.destroy_tracker", kArgsTracker,
                  &inv_destroy_tracker, GPU_HUNT_PROBE(destroy_tracker)),
    GPU_HUNT_DESC("gpu_hunt.tick", kArgsTracker, &inv_tick,
                  GPU_HUNT_PROBE(tick)),
    GPU_HUNT_DESC("gpu_hunt.get_view_proj", kArgsTracker, &inv_get_view_proj,
                  GPU_HUNT_PROBE(get_view_proj)),
    GPU_HUNT_DESC("gpu_hunt.get_camera_pos", kArgsTracker,
                  &inv_get_camera_pos, GPU_HUNT_PROBE(get_camera_pos)),
    GPU_HUNT_DESC("gpu_hunt.get_skeleton_positions", kArgsTrackerMax,
                  &inv_get_skeleton_positions,
                  GPU_HUNT_PROBE(get_skeleton_positions)),
    GPU_HUNT_DESC("gpu_hunt.world_to_screen", kArgsWorldToScreen,
                  &inv_world_to_screen, GPU_HUNT_PROBE(world_to_screen)),
    GPU_HUNT_DESC("gpu_hunt.invalidate", kArgsTracker, &inv_invalidate,
                  GPU_HUNT_PROBE(invalidate)),
    // attach / detach
    GPU_HUNT_DESC("gpu_hunt.attach_tracker", kArgsTrackerPid,
                  &inv_attach_tracker, GPU_HUNT_PROBE(attach_tracker)),
    GPU_HUNT_DESC("gpu_hunt.detach_tracker", kArgsTracker,
                  &inv_detach_tracker, GPU_HUNT_PROBE(detach_tracker)),
    // 锁持久化 / hint
    GPU_HUNT_DESC("gpu_hunt.get_matrix_lock_info", kArgsTracker,
                  &inv_get_matrix_lock_info,
                  GPU_HUNT_PROBE(get_matrix_lock_info)),
    GPU_HUNT_DESC("gpu_hunt.set_prior_lock_hint", kArgsPriorLockHint,
                  &inv_set_prior_lock_hint,
                  GPU_HUNT_PROBE(set_prior_lock_hint)),
    // skeleton fingerprint
    GPU_HUNT_DESC("gpu_hunt.get_skeleton_fingerprint", kArgsTrackerMax,
                  &inv_get_skeleton_fingerprint,
                  GPU_HUNT_PROBE(get_skeleton_fingerprint)),
    GPU_HUNT_DESC("gpu_hunt.set_skeleton_fingerprint_hint",
                  kArgsFingerprintHint,
                  &inv_set_skeleton_fingerprint_hint,
                  GPU_HUNT_PROBE(set_skeleton_fingerprint_hint)),
    // 多簇骨骼
    GPU_HUNT_DESC("gpu_hunt.get_bone_cluster_count", kArgsTracker,
                  &inv_get_bone_cluster_count,
                  GPU_HUNT_PROBE(get_bone_cluster_count)),
    GPU_HUNT_DESC("gpu_hunt.get_bone_cluster_positions", kArgsTrackerIndexMax,
                  &inv_get_bone_cluster_positions,
                  GPU_HUNT_PROBE(get_bone_cluster_positions)),
    // hot heaps
    GPU_HUNT_DESC("gpu_hunt.get_prior_hot_heaps", kArgsTrackerMax,
                  &inv_get_prior_hot_heaps,
                  GPU_HUNT_PROBE(get_prior_hot_heaps)),
    GPU_HUNT_DESC("gpu_hunt.set_prior_hot_heaps", kArgsHotHeaps,
                  &inv_set_prior_hot_heaps,
                  GPU_HUNT_PROBE(set_prior_hot_heaps)),
    // pin / toggles / locator
    GPU_HUNT_DESC("gpu_hunt.set_pinned_heap", kArgsPinnedHeap,
                  &inv_set_pinned_heap, GPU_HUNT_PROBE(set_pinned_heap)),
    GPU_HUNT_DESC("gpu_hunt.get_pinned_heap", kArgsTracker,
                  &inv_get_pinned_heap, GPU_HUNT_PROBE(get_pinned_heap)),
    GPU_HUNT_DESC("gpu_hunt.get_runtime_toggles", kArgsTracker,
                  &inv_get_runtime_toggles,
                  GPU_HUNT_PROBE(get_runtime_toggles)),
    GPU_HUNT_DESC("gpu_hunt.set_runtime_toggles", kArgsToggles,
                  &inv_set_runtime_toggles,
                  GPU_HUNT_PROBE(set_runtime_toggles)),
    GPU_HUNT_DESC("gpu_hunt.get_locator_profile", kArgsTracker,
                  &inv_get_locator_profile,
                  GPU_HUNT_PROBE(get_locator_profile)),
    GPU_HUNT_DESC("gpu_hunt.set_locator_profile", kArgsProfile,
                  &inv_set_locator_profile,
                  GPU_HUNT_PROBE(set_locator_profile)),
    // 分离锁
    GPU_HUNT_DESC("gpu_hunt.get_view_matrix", kArgsTracker,
                  &inv_get_view_matrix, GPU_HUNT_PROBE(get_view_matrix)),
    GPU_HUNT_DESC("gpu_hunt.get_proj_matrix", kArgsTracker,
                  &inv_get_proj_matrix, GPU_HUNT_PROBE(get_proj_matrix)),
    GPU_HUNT_DESC("gpu_hunt.get_camera_world_pos", kArgsTracker,
                  &inv_get_camera_world_pos,
                  GPU_HUNT_PROBE(get_camera_world_pos)),
    GPU_HUNT_DESC("gpu_hunt.get_split_lock_info", kArgsTracker,
                  &inv_get_split_lock_info,
                  GPU_HUNT_PROBE(get_split_lock_info)),
    // 运动预测尾部
    GPU_HUNT_DESC("gpu_hunt.get_predicted_view", kArgsPredictedView,
                  &inv_get_predicted_view,
                  GPU_HUNT_PROBE(get_predicted_view)),
    GPU_HUNT_DESC("gpu_hunt.get_motion_prediction_confidence", kArgsTracker,
                  &inv_get_motion_prediction_confidence,
                  GPU_HUNT_PROBE(get_motion_prediction_confidence)),
    GPU_HUNT_DESC("gpu_hunt.get_motion_prediction_sample_count", kArgsTracker,
                  &inv_get_motion_prediction_sample_count,
                  GPU_HUNT_PROBE(get_motion_prediction_sample_count)),
    // 场景观察尾部
    GPU_HUNT_DESC("gpu_hunt.get_scene_snapshot", kArgsTracker,
                  &inv_get_scene_snapshot,
                  GPU_HUNT_PROBE(get_scene_snapshot)),
    GPU_HUNT_DESC("gpu_hunt.get_scene_objects", kArgsTrackerMax,
                  &inv_get_scene_objects,
                  GPU_HUNT_PROBE(get_scene_objects)),
    GPU_HUNT_DESC("gpu_hunt.get_scene_object_bones", kArgsTrackerObjectIdMax,
                  &inv_get_scene_object_bones,
                  GPU_HUNT_PROBE(get_scene_object_bones)),
    // 表元数据
    {"gpu_hunt.abi_version", nullptr, 0, &inv_abi_version,
     &gpu_hunt_abi_version_present},
    {"gpu_hunt.struct_size", nullptr, 0, &inv_struct_size,
     &gpu_hunt_struct_size_present},
};

bool gpu_hunt_group_probe(const SaoSdkContext* ctx) noexcept {
    return gpu_table(ctx) != nullptr;
}

} // namespace

const sdk_engine_group_table kEngineGroupGpuHunt = {
    kEngineFnsGpuHunt,
    sizeof(kEngineFnsGpuHunt) / sizeof(kEngineFnsGpuHunt[0]),
    &gpu_hunt_group_probe,
};

} // namespace sao::plugins::sdk_binding
