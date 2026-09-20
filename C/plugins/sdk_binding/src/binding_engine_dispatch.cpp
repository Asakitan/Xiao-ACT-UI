// binding_engine_dispatch.cpp — 反射引擎面共享基元与 method_engine_call /
// method_engine_list 的 dispatch 实现。
//
// 结果形状对齐 binding_engine.h 契约：写向 request->out_result_json_utf8 的
// 一律是 {"status": <sdk 状态码>, "result": <值>} 的有界 JSON；缓冲语义与
// binding_context_dispatch.cpp 的 write_result 完全相同（先写
// out_required、双 nullptr 即空写返回 SAO_OK、容量不足返回
// SAO_ERR_BUFFER_TOO_SMALL 并把首字节清零）。
//
// 本 TU 自含：有界 JSON 校验复用导出的
// sao_plugins_binding_validate_json_text，不依赖其它 TU 的 static 帮手。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace sao::plugins::sdk_binding {
namespace {

// dump + 有界校验（大小上限、UTF-8、深度/节点/字符串约束由导出 validator 兜底）。
bool engine_dump_bounded(const engine_json& value, std::string& output) noexcept {
    try {
        output = value.dump();
        if (output.empty() || output.size() > kMaximumBindingJsonBytes) return false;
        return sao_plugins_binding_validate_json_text(
            reinterpret_cast<const uint8_t*>(output.data()), output.size());
    } catch (...) {
        output.clear();
        return false;
    }
}

// name 缺失或为 null → true；否则把命中的 json 节点放进 *found。
bool engine_arg_absent(const engine_json& args, const char* name,
                       const engine_json** found) noexcept {
    *found = nullptr;
    if (name == nullptr || !args.is_object()) return true;
    try {
        const auto it = args.find(name);
        if (it == args.end() || it->is_null()) return true;
        *found = &*it;
        return false;
    } catch (...) {
        return true;
    }
}

constexpr char k_engine_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int32_t engine_b64_index(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// 严格 base64：非法字符拒绝；'=' 只允许出现在尾部收尾。
bool engine_b64_decode(std::string_view text, std::vector<uint8_t>* out) noexcept {
    if (out == nullptr) return false;
    out->clear();
    try {
        uint32_t accumulator = 0;
        uint32_t bits = 0;
        size_t padding = 0;
        out->reserve((text.size() / 4U + 1U) * 3U);
        for (const char character : text) {
            if (character == '=') {
                if (++padding > 2U) return false;
                continue;
            }
            if (padding != 0U) return false;
            const int32_t value = engine_b64_index(character);
            if (value < 0) return false;
            accumulator = (accumulator << 6U) | static_cast<uint32_t>(value);
            bits += 6U;
            if (bits >= 8U) {
                bits -= 8U;
                out->push_back(static_cast<uint8_t>((accumulator >> bits) & 0xffU));
            }
        }
        return true;
    } catch (...) {
        out->clear();
        return false;
    }
}

// 严格整数读取：无符号或 >=0 的有符号整数 → u64。
bool engine_json_to_u64(const engine_json& value, uint64_t* out) noexcept {
    try {
        if (value.is_number_unsigned()) {
            *out = value.get<uint64_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const int64_t number = value.get<int64_t>();
            if (number < 0) return false;
            *out = static_cast<uint64_t>(number);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool engine_json_to_i64(const engine_json& value, int64_t* out) noexcept {
    try {
        if (value.is_number_integer()) {
            *out = value.get<int64_t>();
            return true;
        }
        if (value.is_number_unsigned()) {
            const uint64_t number = value.get<uint64_t>();
            if (number >
                static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
                return false;
            }
            *out = static_cast<int64_t>(number);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace

// ── 结果写出 / 状态帮手 ─────────────────────────────────────

int32_t engine_result(sdk_context_call_request* request,
                      const engine_json& value) noexcept {
    if (request == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        // 契约形状 {"status","result"}；若 invoker 自己给出带这两个键的
        // 对象则原样写出，避免重复包壳。
        engine_json envelope;
        const engine_json* output = &value;
        const bool already_shaped = value.is_object() && value.contains("status") &&
                                    value.contains("result");
        if (!already_shaped) {
            envelope["status"] = SAO_OK;
            envelope["result"] = value;
            output = &envelope;
        }
        std::string serialized;
        if (!engine_dump_bounded(*output, serialized)) return SAO_ERR_INVALID_ARGUMENT;
        if (request->out_required != nullptr) {
            *request->out_required = serialized.size() + 1;
        }
        if (request->out_result_json_utf8 == nullptr &&
            request->out_required == nullptr) {
            return SAO_OK;
        }
        if (request->out_result_json_utf8 == nullptr ||
            request->out_capacity < serialized.size() + 1) {
            if (request->out_result_json_utf8 != nullptr &&
                request->out_capacity > 0) {
                request->out_result_json_utf8[0] = '\0';
            }
            return SAO_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(request->out_result_json_utf8, serialized.data(),
                    serialized.size());
        request->out_result_json_utf8[serialized.size()] = '\0';
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t engine_unsupported() noexcept {
    return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
}

int32_t engine_no_provider() noexcept {
    return SAO_ERR_NOT_IMPLEMENTED;
}

// ── 参数读取（严格类型检查；optional 时缺省填 fallback）──

bool engine_arg_string(const engine_json& args, const char* name, std::string* out,
                       bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) return optional;
    try {
        if (!found->is_string()) return false;
        *out = found->get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

bool engine_arg_u64(const engine_json& args, const char* name, uint64_t* out,
                    uint64_t fallback, bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) {
        if (!optional) return false;
        *out = fallback;
        return true;
    }
    return engine_json_to_u64(*found, out);
}

bool engine_arg_i64(const engine_json& args, const char* name, int64_t* out,
                    int64_t fallback, bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) {
        if (!optional) return false;
        *out = fallback;
        return true;
    }
    return engine_json_to_i64(*found, out);
}

bool engine_arg_u32(const engine_json& args, const char* name, uint32_t* out,
                    uint32_t fallback, bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) {
        if (!optional) return false;
        *out = fallback;
        return true;
    }
    uint64_t wide = 0;
    if (!engine_json_to_u64(*found, &wide) ||
        wide > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    *out = static_cast<uint32_t>(wide);
    return true;
}

bool engine_arg_i32(const engine_json& args, const char* name, int32_t* out,
                    int32_t fallback, bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) {
        if (!optional) return false;
        *out = fallback;
        return true;
    }
    int64_t wide = 0;
    if (!engine_json_to_i64(*found, &wide) ||
        wide < (std::numeric_limits<int32_t>::min)() ||
        wide > (std::numeric_limits<int32_t>::max)()) {
        return false;
    }
    *out = static_cast<int32_t>(wide);
    return true;
}

bool engine_arg_f64(const engine_json& args, const char* name, double* out,
                    double fallback, bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) {
        if (!optional) return false;
        *out = fallback;
        return true;
    }
    try {
        if (!found->is_number()) return false;
        const double number = found->get<double>();
        if (!std::isfinite(number)) return false;
        *out = number;
        return true;
    } catch (...) {
        return false;
    }
}

bool engine_arg_bool(const engine_json& args, const char* name, bool* out,
                     bool fallback, bool optional) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) {
        if (!optional) return false;
        *out = fallback;
        return true;
    }
    try {
        if (!found->is_boolean()) return false;
        *out = found->get<bool>();
        return true;
    } catch (...) {
        return false;
    }
}

bool engine_arg_bytes(const engine_json& args, const char* name,
                      std::vector<uint8_t>* out) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) return false;
    try {
        if (found->is_string()) {
            return engine_b64_decode(found->get<std::string>(), out);
        }
        if (found->is_array()) {
            out->clear();
            out->reserve(found->size());
            for (const engine_json& item : *found) {
                int64_t byte = 0;
                if (!engine_json_to_i64(item, &byte) || byte < 0 || byte > 255) {
                    out->clear();
                    return false;
                }
                out->push_back(static_cast<uint8_t>(byte));
            }
            return true;
        }
        return false;
    } catch (...) {
        out->clear();
        return false;
    }
}

bool engine_arg_i32_array(const engine_json& args, const char* name,
                          std::vector<int32_t>* out) noexcept {
    if (out == nullptr) return false;
    const engine_json* found = nullptr;
    if (engine_arg_absent(args, name, &found)) return false;
    try {
        if (!found->is_array()) return false;
        out->clear();
        out->reserve(found->size());
        for (const engine_json& item : *found) {
            int64_t element = 0;
            if (!engine_json_to_i64(item, &element) ||
                element < (std::numeric_limits<int32_t>::min)() ||
                element > (std::numeric_limits<int32_t>::max)()) {
                out->clear();
                return false;
            }
            out->push_back(static_cast<int32_t>(element));
        }
        return true;
    } catch (...) {
        out->clear();
        return false;
    }
}

// ── base64 输出 ──────────────────────────────────────────────

std::string engine_b64_encode(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return {};
    std::string output;
    output.reserve(((size + 2U) / 3U) * 4U);
    for (size_t index = 0; index < size; index += 3U) {
        const uint32_t b0 = data[index];
        const uint32_t b1 = index + 1U < size ? data[index + 1U] : 0U;
        const uint32_t b2 = index + 2U < size ? data[index + 2U] : 0U;
        const uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;
        output.push_back(k_engine_b64_alphabet[(triple >> 18U) & 0x3fU]);
        output.push_back(k_engine_b64_alphabet[(triple >> 12U) & 0x3fU]);
        output.push_back(index + 1U < size
                             ? k_engine_b64_alphabet[(triple >> 6U) & 0x3fU]
                             : '=');
        output.push_back(index + 2U < size ? k_engine_b64_alphabet[triple & 0x3fU]
                                           : '=');
    }
    return output;
}

// ── 通用回调通道 ────────────────────────────────────────────

void engine_emit_callback(sdk_context_call_request* request,
                          const char* channel,
                          const engine_json& payload) noexcept {
    if (request == nullptr || channel == nullptr ||
        request->engine_callback == nullptr) {
        return;
    }
    std::string serialized;
    if (!engine_dump_bounded(payload, serialized)) return;
    try {
        request->engine_callback(channel,
                                 reinterpret_cast<const uint8_t*>(serialized.data()),
                                 serialized.size(), request->callback_user_data);
    } catch (...) {
    }
}

// ── dispatch 入口 ────────────────────────────────────────────

int32_t dispatch_engine_call(const SaoSdkContext* ctx,
                             const engine_json& arguments,
                             sdk_context_call_request* request) {
    if (request == nullptr || !arguments.is_object()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::string name;
    if (!engine_arg_string(arguments, "name", &name) || name.empty()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    // "args" 默认 {}；顶层 "callback_channel" 透传进 invoker 的 args。
    engine_json call_args = engine_json::object();
    try {
        const auto found_args = arguments.find("args");
        if (found_args != arguments.end() && !found_args->is_null()) {
            if (!found_args->is_object()) return SAO_ERR_INVALID_ARGUMENT;
            call_args = *found_args;
        }
        const auto found_channel = arguments.find("callback_channel");
        if (found_channel != arguments.end()) {
            if (!found_channel->is_string()) return SAO_ERR_INVALID_ARGUMENT;
            if (call_args.find("callback_channel") == call_args.end()) {
                call_args["callback_channel"] = *found_channel;
            }
        }
    } catch (...) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const sdk_engine_function_desc* desc = sdk_engine_catalog_find(name);
    if (desc == nullptr || desc->invoke == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!sdk_engine_entry_available(ctx, desc)) {
        return engine_no_provider();
    }
    try {
        // invoker 内部自行调用 engine_result 写结果，返回值即最终状态。
        return desc->invoke(ctx, call_args, request);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t dispatch_engine_list(const SaoSdkContext* ctx,
                             sdk_context_call_request* request) {
    if (request == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        engine_json catalog = engine_json::array();
        const size_t count = sdk_engine_catalog_size();
        for (size_t index = 0; index < count; ++index) {
            const sdk_engine_function_desc* desc = sdk_engine_catalog_at(index);
            if (desc == nullptr || desc->name == nullptr) continue;
            engine_json arg_names = engine_json::array();
            for (uint32_t arg = 0; arg < desc->arg_count; ++arg) {
                const char* arg_name =
                    desc->arg_names != nullptr ? desc->arg_names[arg] : nullptr;
                arg_names.push_back(arg_name != nullptr ? arg_name : "");
            }
            catalog.push_back(engine_json{
                {"name", desc->name},
                {"args", std::move(arg_names)},
                {"available", sdk_engine_entry_available(ctx, desc)},
            });
        }
        return engine_result(request, catalog);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::sdk_binding
