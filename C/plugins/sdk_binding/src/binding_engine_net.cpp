// binding_engine_net.cpp — 反射引擎面 net 组：覆盖 SaoSdkNetTable 全部槽位
// （set_frame_callback / capture_start / capture_stop / parse_packet）。
// 回调槽经 request->engine_callback 通用通道桥接：trampoline 的 user_data
// 为堆分配的 engine_callback_state（只捕获 fn 指针 + user_data，绝不持有
// 栈上 request），所有权经 g_net_callback_owners（按 ctx_impl 键）在
// 反注册成功后回收。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include "sao/sdk/sao_sdk_net.h"

#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace sao::plugins::sdk_binding {
namespace {

// parse_packet 输出数组上限（对齐 provider 侧 kMaximumParsedResults）。
constexpr size_t kMaxParsedResults = 4096;

// 单个回调通道的桥接状态。只持有通用回调 fn + user_data，跨请求存活。
struct engine_callback_state {
    sdk_context_engine_callback_fn callback;
    void* user_data;
};

// 每个 SaoSdkContext（以 ctx_impl 为键）在本组注册的回调所有权。
struct net_callback_owner {
    engine_callback_state* frame = nullptr;
    engine_callback_state* packet = nullptr;
};

std::mutex g_net_callback_mutex;
std::unordered_map<const void*, net_callback_owner> g_net_callback_owners;

// 序列化 payload 并触发语言侧通用回调；任何序列化/回调异常就地吞掉，
// 不外泄到 provider 回调线程。
void engine_callback_emit(const engine_callback_state* state, const char* channel,
                          const engine_json& payload) noexcept {
    if (state == nullptr || state->callback == nullptr) return;
    try {
        const std::string serialized = payload.dump();
        if (serialized.empty() || serialized.size() > kMaximumBindingJsonBytes) return;
        state->callback(channel, reinterpret_cast<const uint8_t*>(serialized.data()),
                        serialized.size(), state->user_data);
    } catch (...) {
    }
}

void SAO_SDK_CALL net_frame_trampoline(const uint8_t* frame_bytes, size_t frame_length,
                                       uint64_t ts_unix_ns, void* user_data) {
    const auto* state = static_cast<const engine_callback_state*>(user_data);
    if (frame_length > 0 && frame_bytes == nullptr) return;
    engine_json payload = engine_json::object();
    payload["frame_b64"] =
        frame_bytes == nullptr ? std::string{} : engine_b64_encode(frame_bytes, frame_length);
    payload["length"] = frame_length;
    payload["ts_unix_ns"] = ts_unix_ns;
    engine_callback_emit(state, "net.frame", payload);
}

void SAO_SDK_CALL net_packet_trampoline(const SaoSdkNetPacketView* packet, void* user_data) {
    const auto* state = static_cast<const engine_callback_state*>(user_data);
    if (packet == nullptr) return;
    engine_json payload = engine_json::object();
    payload["packet_b64"] =
        packet->data == nullptr ? std::string{} : engine_b64_encode(packet->data, packet->data_size);
    payload["length"] = packet->data_size;
    payload["link_type"] = packet->link_type;
    payload["flags"] = packet->flags;
    payload["ts_unix_ns"] = packet->timestamp_unix_ns;
    engine_callback_emit(state, "net.packet", payload);
}

// 登记 owner 槽位；若已有旧 state 先交还（调用方须先完成 vtable 反注册）。
// 两个槽位都空时清掉 map 项，避免空 owner 驻留。
void net_owner_store_frame(const SaoSdkContext* ctx, engine_callback_state* state) {
    std::lock_guard<std::mutex> lock(g_net_callback_mutex);
    auto& owner = g_net_callback_owners[ctx->ctx_impl];
    delete owner.frame;
    owner.frame = state;
    if (owner.frame == nullptr && owner.packet == nullptr) {
        g_net_callback_owners.erase(ctx->ctx_impl);
    }
}

void net_owner_store_packet(const SaoSdkContext* ctx, engine_callback_state* state) {
    std::lock_guard<std::mutex> lock(g_net_callback_mutex);
    auto& owner = g_net_callback_owners[ctx->ctx_impl];
    delete owner.packet;
    owner.packet = state;
    if (owner.frame == nullptr && owner.packet == nullptr) {
        g_net_callback_owners.erase(ctx->ctx_impl);
    }
}

// 取出 owner 槽位并置空；两个槽位都空时清掉 map 项。返回的 state 由调用方 delete。
engine_callback_state* net_owner_take(const SaoSdkContext* ctx, bool frame_slot) {
    std::lock_guard<std::mutex> lock(g_net_callback_mutex);
    const auto found = g_net_callback_owners.find(ctx->ctx_impl);
    if (found == g_net_callback_owners.end()) return nullptr;
    engine_callback_state* state = frame_slot ? found->second.frame : found->second.packet;
    if (frame_slot) {
        found->second.frame = nullptr;
    } else {
        found->second.packet = nullptr;
    }
    if (found->second.frame == nullptr && found->second.packet == nullptr) {
        g_net_callback_owners.erase(found);
    }
    return state;
}

// ── 槽位探测 ─────────────────────────────────────────────────────

bool net_group_available(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr && ctx->net != nullptr;
}

bool net_frame_callback_available(const SaoSdkContext* ctx) noexcept {
    return net_group_available(ctx) && ctx->net->set_frame_callback != nullptr;
}

bool net_v1_5_ready(const SaoSdkContext* ctx) noexcept {
    return net_group_available(ctx) && sao_sdk_net_v1_5_status(ctx) == SAO_SDK_OK;
}

bool net_capture_start_available(const SaoSdkContext* ctx) noexcept {
    return net_v1_5_ready(ctx) && ctx->net->capture_start != nullptr;
}

bool net_capture_stop_available(const SaoSdkContext* ctx) noexcept {
    return net_v1_5_ready(ctx) && ctx->net->capture_stop != nullptr;
}

bool net_parse_packet_available(const SaoSdkContext* ctx) noexcept {
    return net_v1_5_ready(ctx) && ctx->net->parse_packet != nullptr;
}

// ── 调用器 ───────────────────────────────────────────────────────

int32_t invoke_net_set_frame_callback(const SaoSdkContext* ctx, const engine_json& args,
                                      sdk_context_call_request* request) {
    bool enable = false;
    if (!engine_arg_bool(args, "enable", &enable)) return SAO_ERR_INVALID_ARGUMENT;
    if (!net_frame_callback_available(ctx)) return engine_no_provider();
    if (!enable) {
        const sao_sdk_status_t status =
            sao_sdk_net_set_frame_callback(ctx, nullptr, nullptr);
        if (status != SAO_SDK_OK) return status;
        delete net_owner_take(ctx, true);
        return engine_result(request, engine_json(true));
    }
    if (request->engine_callback == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    auto* state = new (std::nothrow)
        engine_callback_state{request->engine_callback, request->callback_user_data};
    if (state == nullptr) return SAO_ERR_OS_CALL_FAILED;
    // 先反注册并回收旧订阅，再挂新 trampoline。
    if (sao_sdk_net_set_frame_callback(ctx, nullptr, nullptr) == SAO_SDK_OK) {
        delete net_owner_take(ctx, true);
    }
    const sao_sdk_status_t status =
        sao_sdk_net_set_frame_callback(ctx, &net_frame_trampoline, state);
    if (status != SAO_SDK_OK) {
        delete state;
        return status;
    }
    net_owner_store_frame(ctx, state);
    return engine_result(request, engine_json(true));
}

int32_t invoke_net_capture_start(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    uint32_t snap_length = 0;
    uint32_t flags = 0;
    std::string source_id;
    std::string filter;
    bool want_callback = true;
    if (!engine_arg_u32(args, "snap_length", &snap_length) ||
        !engine_arg_string(args, "source_id", &source_id) ||
        !engine_arg_string(args, "filter", &filter, true) ||
        !engine_arg_u32(args, "flags", &flags, 0, true) ||
        !engine_arg_bool(args, "callback", &want_callback, true, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (snap_length == 0 || snap_length > SAO_SDK_NET_MAX_PACKET_SIZE ||
        source_id.empty() || source_id.size() > SAO_SDK_NET_MAX_SOURCE_ID_SIZE ||
        filter.size() > SAO_SDK_NET_MAX_FILTER_SIZE) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (want_callback && request->engine_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!net_capture_start_available(ctx)) return engine_no_provider();
    SaoSdkNetCaptureConfig config{};
    config.struct_size = sizeof(config);
    config.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    config.snap_length = snap_length;
    config.flags = flags;
    config.source_id_utf8 = source_id.c_str();
    config.source_id_len = source_id.size();
    config.filter_utf8 = filter.empty() ? nullptr : filter.c_str();
    config.filter_len = filter.size();
    // provider 要求 callback 非空：不要回调的 caller 挂 trampoline + 空 state。
    engine_callback_state* state = nullptr;
    if (want_callback) {
        state = new (std::nothrow)
            engine_callback_state{request->engine_callback, request->callback_user_data};
        if (state == nullptr) return SAO_ERR_OS_CALL_FAILED;
    }
    const sao_sdk_status_t status =
        sao_sdk_net_capture_start(ctx, &config, &net_packet_trampoline, state);
    if (status != SAO_SDK_OK) {
        delete state;
        return status;
    }
    net_owner_store_packet(ctx, state);
    return engine_result(request, engine_json(true));
}

int32_t invoke_net_capture_stop(const SaoSdkContext* ctx, const engine_json&,
                                sdk_context_call_request* request) {
    if (!net_capture_stop_available(ctx)) return engine_no_provider();
    const sao_sdk_status_t status = sao_sdk_net_capture_stop(ctx);
    if (status != SAO_SDK_OK) return status;
    delete net_owner_take(ctx, false);
    return engine_result(request, engine_json(true));
}

int32_t invoke_net_parse_packet(const SaoSdkContext* ctx, const engine_json& args,
                                sdk_context_call_request* request) {
    std::vector<uint8_t> data;
    uint32_t link_type = SAO_SDK_NET_LINK_UNKNOWN;
    uint32_t flags = 0;
    uint64_t timestamp_unix_ns = 0;
    if (!engine_arg_bytes(args, "data_b64", &data) || data.empty() ||
        data.size() > SAO_SDK_NET_MAX_PACKET_SIZE ||
        !engine_arg_u32(args, "link_type", &link_type, SAO_SDK_NET_LINK_UNKNOWN, true) ||
        !engine_arg_u32(args, "flags", &flags, 0, true) ||
        !engine_arg_u64(args, "timestamp_unix_ns", &timestamp_unix_ns, 0, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!net_parse_packet_available(ctx)) return engine_no_provider();
    SaoSdkNetPacketView view{};
    view.struct_size = sizeof(view);
    view.abi_version = SAO_SDK_NET_PROVIDER_ABI_VERSION;
    view.link_type = link_type;
    view.flags = flags;
    view.data = data.data();
    view.data_size = data.size();
    view.timestamp_unix_ns = timestamp_unix_ns;
    // 两调容量模式：capacity=0 探测结果数，再按数分配。
    size_t count = 0;
    sao_sdk_status_t status = sao_sdk_net_parse_packet(
        ctx, &view, nullptr, 0, SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE, &count);
    if (status != SAO_SDK_OK && status != SAO_SDK_ERR_BUFFER_TOO_SMALL) return status;
    if (count == 0) return engine_result(request, engine_json::array());
    if (count > kMaxParsedResults) return SAO_ERR_OS_CALL_FAILED;
    std::vector<SaoSdkNetParsedResult> results(count);
    status = sao_sdk_net_parse_packet(ctx, &view, results.data(), results.size(),
                                      SAO_SDK_NET_PARSED_RESULT_ELEMENT_SIZE, &count);
    if (status != SAO_SDK_OK) return status;
    if (count > results.size()) return SAO_ERR_OS_CALL_FAILED;
    engine_json result = engine_json::array();
    for (size_t index = 0; index < count; ++index) {
        const SaoSdkNetParsedResult& parsed = results[index];
        engine_json entry = engine_json::object();
        entry["layer"] = parsed.layer;
        entry["protocol"] = parsed.protocol;
        entry["header_offset"] = parsed.header_offset;
        entry["header_size"] = parsed.header_size;
        entry["payload_offset"] = parsed.payload_offset;
        entry["payload_size"] = parsed.payload_size;
        entry["flow_id"] = parsed.flow_id;
        result.push_back(std::move(entry));
    }
    return engine_result(request, result);
}

// ── 目录表 ───────────────────────────────────────────────────────

const char* const kArgsNetSetFrameCallback[] = {"enable"};
const char* const kArgsNetCaptureStart[] = {"snap_length", "source_id", "filter", "flags",
                                            "callback"};
const char* const kArgsNetParsePacket[] = {"data_b64", "link_type", "flags",
                                           "timestamp_unix_ns"};

const sdk_engine_function_desc kEngineNetDescs[] = {
    {"net.set_frame_callback", kArgsNetSetFrameCallback, 1, &invoke_net_set_frame_callback,
     &net_frame_callback_available},
    {"net.capture_start", kArgsNetCaptureStart, 5, &invoke_net_capture_start,
     &net_capture_start_available},
    {"net.capture_stop", nullptr, 0, &invoke_net_capture_stop, &net_capture_stop_available},
    {"net.parse_packet", kArgsNetParsePacket, 4, &invoke_net_parse_packet,
     &net_parse_packet_available},
};

} // namespace

const sdk_engine_group_table kEngineGroupNet = {
    kEngineNetDescs,
    sizeof(kEngineNetDescs) / sizeof(kEngineNetDescs[0]),
    &net_group_available,
};

} // namespace sao::plugins::sdk_binding
