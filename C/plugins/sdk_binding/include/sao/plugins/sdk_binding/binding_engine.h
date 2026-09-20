// binding_engine.h — 反射引擎面契约：SaoSdkContext 全部 vtable 槽与
// sao_sdk_* 自由函数的统一 JSON 调用目录。
//
// 目标：五种脚本宿主（python/pymini、csmini、lua、emma、angel）经
// `sao_plugins_sdk_context_dispatch(ctx, sdk_method_id::method_engine_call, req)`
// 调用平台引擎的【全部】功能，而不是仅有零散 ctx 方法。
//
// 调用协议：
//   args JSON: {"name": "<组.函数>", "args": {...}, ["callback_channel": "x"]}
//   结果写入 request->out_result_json_utf8：{"status": <sdk状态码>, "result": <值>}
//   `sdk_method_id::method_engine_list` 的 result 为目录数组
//   [{"name","args":[<顺序参数名>],"available":true|false}]。
//   `available` 由所需 vtable 槽是否为 null 决定（惰性探测）。
//
// 二进制数据约定：输入/输出 buffer 一律 base64 字符串字段（"..._b64"）。
// 句柄类（panel/tracker/hotkey/timer/subscription token）以无符号整数传输。
// 需要语言侧回调的槽使用 request->engine_callback 通用通道：
// channel_utf8 命名子通道（如 "net.frame"、"ui.render_hook"），payload 为
// 序列化 JSON；无 engine_callback 的 caller 对回调型函数得到
// SAO_ERR_INVALID_ARGUMENT（fail closed）。
//
// 分组 TU：每个 binding_engine_<group>.cpp 定义一张
// `const sdk_engine_function_desc kEngineFns<Group>[]` + count，
// catalog 聚合所有组表。新增组只需新文件 + catalog 聚合行。
//
// Host 侧命名包装契约（五个宿主统一）：
//   - 暴露 `ctx.engine` 命名空间/对象，含每个目录条目的命名函数，
//     名称 = catalog name 的 '.' 换成 '_'（"mem.read_u64" → mem_read_u64）。
//   - 位置参数按 arg_names 顺序映射；动态语言额外接受 keyword args。
//   - `engine.list()`（或等价）返回目录数组；`engine.on(channel, cb)`
//     注册通用回调（映射到 engine_callback）。
//   - 非零 status 按各语言惯例抛异常 / 返回错误值。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "sao/plugins/sdk_binding/binding_common.h"

struct SaoSdkContext;

namespace sao::plugins::sdk_binding {

using engine_json = nlohmann::ordered_json;

// 单条目录项的调用器：读 args、调 vtable/自由函数、写 result。
// 返回 SAO_OK 或具体 status/错误码；需要回调但请求未带 engine_callback
// 时返回 SAO_ERR_INVALID_ARGUMENT。
using sdk_engine_invoke_fn = int32_t (*)(const SaoSdkContext* ctx,
                                         const engine_json& args,
                                         sdk_context_call_request* request);

// 可用性探测：检查该条目依赖的 vtable 槽/自由函数提供者是否就绪。
// nullptr 表示随组探测或恒可用。
using sdk_engine_probe_fn = bool (*)(const SaoSdkContext* ctx) noexcept;

struct sdk_engine_function_desc {
    const char* name;              // "mem.read_u64"（'-'免，组名.函数名）
    const char* const* arg_names;  // 顺序位置参数名，可为 nullptr
    uint32_t arg_count;
    sdk_engine_invoke_fn invoke;
    sdk_engine_probe_fn availability;  // 可为 nullptr → 用组 probe
};

// 组表 + 组级探测（全组共享同一 vtable 时 nullptr desc.probe 用它）。
struct sdk_engine_group_table {
    const sdk_engine_function_desc* descs;
    size_t count;
    sdk_engine_probe_fn probe;  // 可为 nullptr → 组内条目恒可用
};

// ── 组表 extern（每组一个 TU 定义）──────────────────────────────
extern const sdk_engine_group_table kEngineGroupMem;
extern const sdk_engine_group_table kEngineGroupNet;
extern const sdk_engine_group_table kEngineGroupUi;
extern const sdk_engine_group_table kEngineGroupGpuHunt;
extern const sdk_engine_group_table kEngineGroupVt;  // ctx->vt (VT-Splitview proxy)
extern const sdk_engine_group_table kEngineGroupMisc;  // config/event/hotkey/tts/banner/
                                                       // sound/notify/timer/version/
                                                       // provider/engine/platform

// ── 目录查询 ──────────────────────────────────────────────────
const sdk_engine_function_desc* sdk_engine_catalog_find(std::string_view name) noexcept;
const sdk_engine_group_table* sdk_engine_catalog_group_of(
    const sdk_engine_function_desc* desc) noexcept;
size_t sdk_engine_catalog_size() noexcept;
const sdk_engine_function_desc* sdk_engine_catalog_at(size_t index) noexcept;
bool sdk_engine_entry_available(const SaoSdkContext* ctx,
                                const sdk_engine_function_desc* desc) noexcept;

// ── 共享 arg 读取 / 结果写出（binding_engine_dispatch.cpp 实装）──
int32_t engine_result(sdk_context_call_request* request, const engine_json& value) noexcept;
int32_t engine_unsupported() noexcept;
int32_t engine_no_provider() noexcept;   // 所需 vtable 缺失时的状态

bool engine_arg_string(const engine_json& args, const char* name, std::string* out,
                       bool optional = false) noexcept;
bool engine_arg_u64(const engine_json& args, const char* name, uint64_t* out,
                    uint64_t fallback = 0, bool optional = false) noexcept;
bool engine_arg_i64(const engine_json& args, const char* name, int64_t* out,
                    int64_t fallback = 0, bool optional = false) noexcept;
bool engine_arg_u32(const engine_json& args, const char* name, uint32_t* out,
                    uint32_t fallback = 0, bool optional = false) noexcept;
bool engine_arg_i32(const engine_json& args, const char* name, int32_t* out,
                    int32_t fallback = 0, bool optional = false) noexcept;
bool engine_arg_f64(const engine_json& args, const char* name, double* out,
                    double fallback = 0.0, bool optional = false) noexcept;
bool engine_arg_bool(const engine_json& args, const char* name, bool* out,
                     bool fallback = false, bool optional = false) noexcept;
// base64 解码进 out_bytes；数组形式 [0..255] 也接受。
bool engine_arg_bytes(const engine_json& args, const char* name,
                      std::vector<uint8_t>* out) noexcept;
// u64/u32 数组（如 read_ptr_chain offsets → i32 列）。
bool engine_arg_i32_array(const engine_json& args, const char* name,
                          std::vector<int32_t>* out) noexcept;

// bytes → base64 字符串。
std::string engine_b64_encode(const uint8_t* data, size_t size);

// 触发通用回调（request->engine_callback 非空时调用，payload 序列化 JSON）。
void engine_emit_callback(sdk_context_call_request* request,
                          const char* channel,
                          const engine_json& payload) noexcept;

// dispatch 入口（binding_context_dispatch.cpp 的 method_engine_call /
// method_engine_list arm 调用）。
int32_t dispatch_engine_call(const SaoSdkContext* ctx,
                             const engine_json& arguments,
                             sdk_context_call_request* request);
int32_t dispatch_engine_list(const SaoSdkContext* ctx,
                             sdk_context_call_request* request);

} // namespace sao::plugins::sdk_binding
