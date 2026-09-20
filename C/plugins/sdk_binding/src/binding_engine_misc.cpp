// binding_engine_misc.cpp — 反射引擎面 MISC 组：
//   SaoSdkContext 的 config / event / hotkey / tts / banner vtable 槽 +
//   sao_sdk_* 中脚本可调用的其余自由函数（sound / notify / dialog /
//   timer / overlay / context / platform / provider status / version）。
//
// 组级 probe = nullptr —— 每条 desc 自带 availability：
//   * config / event 直查 ctx->table->slot；
//   * 经 SaoSdkProviderVTable 的能力（hotkey / tts / banner / timer /
//     notify / dialog / overlay）按 binding_context_dispatch.cpp
//     method_status 的 provider 惯例使用
//     sao_sdk_context_provider_status(ctx) == SAO_SDK_OK；
//   * 纯 free export（sound / platform / context / version /
//     *_provider_status）只要求 ctx 有效或常驻可用。
//
// ── 本组刻意跳过项 ──────────────────────────────────────────────
//   engine.* — sao_sdk_register_engine / sao_sdk_engine_get：
//       不存在接受 SaoSdkContext 的导出；plugin-abi.md 仅列名，真实
//       engine 注册表在 loader 的 plugin_context_t
//       （sao_plugins_ctx_register_engine / sao_plugins_ctx_get_engine，
//       不同上下文类型），不属于 engine-call surface。
//   sao_sdk_context_create / sao_sdk_context_destroy /
//       sao_sdk_context_try_destroy / sao_sdk_bind_context：
//       上下文生命周期宿主函数（脚本只消费已绑定的 ctx）。
//   sao_sdk_context_bind_provider / sao_sdk_context_configure_memory_provider /
//       sao_sdk_context_configure_net_provider / sao_sdk_platform_memory_configure_provider /
//       sao_sdk_platform_net_configure_provider / sao_sdk_platform_gpu_hunt_configure_provider：
//       需要 SaoSdk*ProviderVTable 宿主 vtable —— host/provider 配置面。
//   sao_sdk_platform_bind_ui_compositor / sao_sdk_platform_unbind_ui_compositor /
//       sao_sdk_platform_get_ui_compositor：compositor 内部
//       （platform-internal），void* 句柄无法脚本化。
//   sao_sdk_platform_bind_streaming_mode_apply /
//       sao_sdk_platform_bind_panel_open：宿主回调绑定注册
//       （launcher 侧函数指针，非脚本回调面）。
//   sao_sdk_register_ui_panel / sao_sdk_unregister_ui_panel /
//       sao_sdk_panel_add_widget / sao_sdk_panel_update_widget /
//       sao_sdk_panel_remove_widget / sao_sdk_register_render_hook /
//       sao_sdk_register_render_hook_ex / sao_sdk_unregister_render_hook /
//       sao_sdk_request_redraw / sao_sdk_request_redraw_surface +
//       SaoSdkUiTable 全部槽：kEngineGroupUi 域。
//   sao_sdk_mem_* + SaoSdkMemTable 槽：kEngineGroupMem 域。
//   sao_sdk_net_capture_start / capture_stop / parse_packet /
//       set_frame_callback + SaoSdkNetTable / SaoSdkNetProviderVTable：
//       kEngineGroupNet 域（net.provider_status 仅按契约在此暴露只读查询）。
//   sao_sdk_gpu_hunt_* + SaoSdkGpuHuntTable 槽：kEngineGroupGpuHunt 域。
//   sao_sdk_test_* 导出：SAO_SDK_TESTING 诊断面，非脚本接口。
//   loader plugin_context_t 方法族（should_stop / on_* / snapshot /
//       register_* / menu trio / compositor×11 / call_runtime 等）：
//       loader-ctx-only，不在 SaoSdkContext 面上。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/sdk/sao_sdk_platform_panels.h"
#include "sao/sdk/sao_sdk_sound.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace sao::plugins::sdk_binding {
namespace {

// ── 探测 ──────────────────────────────────────────────────────
//
// probe_ctx：free export 只要求请求携带有效 ctx。
// probe_provider_bound：vtable 槽恒定布线的 provider 依赖能力
//   （hotkey / tts / banner 的 ctx 槽内部走 provider_* lease；timer /
//   notify / dialog / overlay 直接是 provider export）。

bool probe_ctx(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr && ctx->ctx_impl != nullptr;
}

bool probe_always(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr;
}

bool probe_provider_bound(const SaoSdkContext* ctx) noexcept {
    return probe_ctx(ctx) && sao_sdk_context_provider_status(ctx) == SAO_SDK_OK;
}

// config / event 槽探测：槽缺失 → 不可用。
#define MISC_DEFINE_SLOT_PROBE(fn_name, member, slot)                                    \
    bool fn_name(const SaoSdkContext* ctx) noexcept {                                    \
        return probe_ctx(ctx) && ctx->member != nullptr &&                               \
               ctx->member->slot != nullptr;                                             \
    }

MISC_DEFINE_SLOT_PROBE(probe_config_get_bool, config, get_bool)
MISC_DEFINE_SLOT_PROBE(probe_config_get_int, config, get_int)
MISC_DEFINE_SLOT_PROBE(probe_config_get_double, config, get_double)
MISC_DEFINE_SLOT_PROBE(probe_config_get_string, config, get_string)
MISC_DEFINE_SLOT_PROBE(probe_config_set_bool, config, set_bool)
MISC_DEFINE_SLOT_PROBE(probe_config_set_int, config, set_int)
MISC_DEFINE_SLOT_PROBE(probe_config_set_double, config, set_double)
MISC_DEFINE_SLOT_PROBE(probe_config_set_string, config, set_string)

MISC_DEFINE_SLOT_PROBE(probe_event_subscribe, event, subscribe)
MISC_DEFINE_SLOT_PROBE(probe_event_unsubscribe, event, unsubscribe)
MISC_DEFINE_SLOT_PROBE(probe_event_publish, event, publish)

#undef MISC_DEFINE_SLOT_PROBE

// provider 依赖 + ctx 槽常量布线能力：两者都要存活。
#define MISC_DEFINE_PROVIDER_SLOT_PROBE(fn_name, member, slot)                           \
    bool fn_name(const SaoSdkContext* ctx) noexcept {                                    \
        return probe_provider_bound(ctx) && ctx->member != nullptr &&                    \
               ctx->member->slot != nullptr;                                             \
    }

MISC_DEFINE_PROVIDER_SLOT_PROBE(probe_hotkey_register, hotkey, register_hotkey)
MISC_DEFINE_PROVIDER_SLOT_PROBE(probe_hotkey_unregister, hotkey, unregister_hotkey)
MISC_DEFINE_PROVIDER_SLOT_PROBE(probe_tts_speak, tts, speak)
MISC_DEFINE_PROVIDER_SLOT_PROBE(probe_tts_stop, tts, stop)
MISC_DEFINE_PROVIDER_SLOT_PROBE(probe_banner_show, banner, show)

#undef MISC_DEFINE_PROVIDER_SLOT_PROBE

// ── 参数数组 ──────────────────────────────────────────────────

const char* const kArgsKey[] = {"key"};
const char* const kArgsKeyValue[] = {"key", "value"};
const char* const kArgsTopic[] = {"topic"};
const char* const kArgsToken[] = {"token"};
const char* const kArgsTopicPayload[] = {"topic", "payload"};
const char* const kArgsHotkeyRegister[] = {
    "id", "default_key", "virtual_key", "modifiers", "enforce_ctrl_prefix",
    "prevent_default", "allow_repeat"};
const char* const kArgsHotkeyId[] = {"id"};
const char* const kArgsTtsSpeak[] = {"text", "volume", "rate"};
const char* const kArgsBannerShow[] = {"text", "duration_ms", "argb_color"};
const char* const kArgsSoundKind[] = {"kind"};
const char* const kArgsPath[] = {"path"};
const char* const kArgsDurationMs[] = {"duration_ms"};
const char* const kArgsNotifyShow[] = {"message", "title", "duration_s", "argb_color"};
const char* const kArgsNotifyToast[] = {"message"};
const char* const kArgsDialogShow[] = {
    "message", "kind", "title", "input_prompt", "input_default",
    "input_max_length", "dismiss_on_focus_out", "dismiss_on_esc"};
const char* const kArgsSeconds[] = {"seconds"};
const char* const kArgsOverlaySet[] = {"surface", "spec"};
const char* const kArgsSurface[] = {"surface"};
const char* const kArgsPanelName[] = {"name"};
const char* const kArgsEnabled[] = {"enabled"};
const char* const kArgsRenderDispatch[] = {
    "surface", "hook_point", "monotonic_time_ns", "viewport_x", "viewport_y",
    "viewport_width", "viewport_height", "dispatch_flags"};

// ── 本地小工具 ────────────────────────────────────────────────

// dispatch_hotkey 的 parse_hotkey 最小拷贝（static 无法跨 TU 引用）：
// "CTRL+ALT+SHIFT+WIN+F1..F24 / 单字符 alnum" → vk + modifier mask。
bool parse_hotkey(std::string_view text, uint32_t* out_key, uint32_t* out_modifiers) noexcept {
    if (out_key == nullptr || out_modifiers == nullptr) return false;
    *out_key = 0;
    *out_modifiers = 0;
    std::string token;
    for (size_t index = 0; index <= text.size(); ++index) {
        const char character = index == text.size() ? '\0' : text[index];
        if (character != '+' && character != '\0') {
            if (!std::isspace(static_cast<unsigned char>(character))) {
                token.push_back(
                    static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
            }
            continue;
        }
        if (token == "CTRL" || token == "CONTROL") {
            *out_modifiers |= 1u << 0u;
        } else if (token == "ALT") {
            *out_modifiers |= 1u << 1u;
        } else if (token == "SHIFT") {
            *out_modifiers |= 1u << 2u;
        } else if (token == "WIN" || token == "WINDOWS") {
            *out_modifiers |= 1u << 3u;
        } else if (token.size() >= 2 && token[0] == 'F') {
            char* end = nullptr;
            const long number = std::strtol(token.c_str() + 1, &end, 10);
            if (end != token.c_str() + token.size() || number < 1 || number > 24) {
                return false;
            }
            *out_key = 0x70u + static_cast<uint32_t>(number - 1);
        } else if (token.size() == 1 && std::isalnum(static_cast<unsigned char>(token[0]))) {
            *out_key = static_cast<uint32_t>(token[0]);
        } else {
            return false;
        }
        token.clear();
    }
    return *out_key != 0;
}

// 任意 JSON → 序列化 UTF-8（publish / overlay spec 用），带 binding 界
// 的大小上限；catch-all 保证 noexcept 边界。
bool serialize_payload(const engine_json& value, std::string* out) noexcept {
    if (out == nullptr) return false;
    try {
        *out = value.dump();
        return !out->empty() && out->size() <= kMaximumBindingJsonBytes;
    } catch (...) {
        out->clear();
        return false;
    }
}

// dialog 回调桥：sao_sdk_dialog_callback_t 形状 ≠ 任何 typed slot，
// 走 request->engine_callback 通用通道（channel = "dialog.result"）。
// 桥对象经 sao_plugins_binding_track_callback 登记，绑定 unload 时由
// sao_plugins_binding_release_all_callbacks 统一回收。
struct MiscDialogBridge {
    sdk_context_engine_callback_fn emit;
    void* user_data;
};

void SAO_PLUGINS_CALL misc_dialog_bridge_release(void* user_data) {
    delete static_cast<MiscDialogBridge*>(user_data);
}

void SAO_SDK_CALL misc_dialog_forward(sao_sdk_dialog_token_t dialog, int32_t pressed_button,
                                      const char* input_text_utf8, size_t input_text_len,
                                      void* user_data) {
    auto* bridge = static_cast<MiscDialogBridge*>(user_data);
    if (bridge == nullptr || bridge->emit == nullptr) return;
    engine_json payload;
    payload["dialog"] = dialog;
    payload["button"] = pressed_button;
    payload["input"] = input_text_utf8 == nullptr
                           ? std::string{}
                           : std::string(input_text_utf8, input_text_len);
    std::string serialized;
    try {
        serialized = payload.dump();
    } catch (...) {
        return;
    }
    if (serialized.empty() || serialized.size() > kMaximumBindingJsonBytes) return;
    bridge->emit("dialog.result", reinterpret_cast<const uint8_t*>(serialized.data()),
                 serialized.size(), bridge->user_data);
}

// ── config.* ─────────────────────────────────────────────────
// 命名按类型显式分叉（config.get_bool 等），结果透传原始值；
// 键不存在/类型不匹配时透传底层 SDK status。

int32_t invoke_config_get_bool(const SaoSdkContext* ctx, const engine_json& args,
                               sdk_context_call_request* request) {
    std::string key;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->get_bool == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key)) return SAO_ERR_INVALID_ARGUMENT;
    bool value = false;
    const int32_t status = sao_sdk_config_get_bool(ctx, key.c_str(), &value);
    return status == SAO_SDK_OK ? engine_result(request, value) : status;
}

int32_t invoke_config_get_int(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    std::string key;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->get_int == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key)) return SAO_ERR_INVALID_ARGUMENT;
    int64_t value = 0;
    const int32_t status = sao_sdk_config_get_int(ctx, key.c_str(), &value);
    return status == SAO_SDK_OK ? engine_result(request, value) : status;
}

int32_t invoke_config_get_double(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    std::string key;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->get_double == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key)) return SAO_ERR_INVALID_ARGUMENT;
    double value = 0.0;
    const int32_t status = sao_sdk_config_get_double(ctx, key.c_str(), &value);
    return status == SAO_SDK_OK ? engine_result(request, value) : status;
}

int32_t invoke_config_get_string(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    std::string key;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->get_string == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key)) return SAO_ERR_INVALID_ARGUMENT;
    size_t needed = 0;
    int32_t status = sao_sdk_config_get_string(ctx, key.c_str(), nullptr, 0, &needed);
    if (status == SAO_SDK_ERR_BUFFER_TOO_SMALL && needed > 0) {
        if (needed > kMaximumBindingJsonBytes) return SAO_ERR_INVALID_ARGUMENT;
        std::string value(needed, '\0');
        status = sao_sdk_config_get_string(ctx, key.c_str(), value.data(), value.size(), &needed);
        if (status != SAO_SDK_OK) return status;
        const auto terminator = value.find('\0');
        if (terminator == std::string::npos) return SAO_ERR_INVALID_ARGUMENT;
        value.resize(terminator);
        return engine_result(request, value);
    }
    if (status == SAO_SDK_OK) return engine_result(request, std::string{});
    return status;
}

int32_t invoke_config_set_bool(const SaoSdkContext* ctx, const engine_json& args,
                               sdk_context_call_request* request) {
    std::string key;
    bool value = false;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->set_bool == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key) ||
        !engine_arg_bool(args, "value", &value)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_config_set_bool(ctx, key.c_str(), value);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_config_set_int(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    std::string key;
    int64_t value = 0;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->set_int == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key) ||
        !engine_arg_i64(args, "value", &value)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_config_set_int(ctx, key.c_str(), value);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_config_set_double(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    std::string key;
    double value = 0.0;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->set_double == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key) ||
        !engine_arg_f64(args, "value", &value) || !std::isfinite(value)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_config_set_double(ctx, key.c_str(), value);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_config_set_string(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    std::string key;
    std::string value;
    if (ctx == nullptr || ctx->config == nullptr || ctx->config->set_string == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "key", &key) ||
        !engine_arg_string(args, "value", &value)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_config_set_string(ctx, key.c_str(), value.c_str());
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── event.* ──────────────────────────────────────────────────

int32_t invoke_event_subscribe(const SaoSdkContext* ctx, const engine_json& args,
                               sdk_context_call_request* request) {
    std::string topic;
    if (ctx == nullptr || ctx->event == nullptr || ctx->event->subscribe == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "topic", &topic) || request->event_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    sao_sdk_subscription_t token = 0;
    const int32_t status = sao_sdk_event_subscribe(
        ctx, topic.c_str(), request->event_callback, request->callback_user_data, &token);
    return status == SAO_SDK_OK ? engine_result(request, token) : status;
}

int32_t invoke_event_unsubscribe(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    uint64_t token = 0;
    if (ctx == nullptr || ctx->event == nullptr || ctx->event->unsubscribe == nullptr)
        return engine_no_provider();
    if (!engine_arg_u64(args, "token", &token)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_event_unsubscribe(ctx, token);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_event_publish(const SaoSdkContext* ctx, const engine_json& args,
                             sdk_context_call_request* request) {
    std::string topic;
    if (ctx == nullptr || ctx->event == nullptr || ctx->event->publish == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "topic", &topic)) return SAO_ERR_INVALID_ARGUMENT;
    const auto payload = args.find("payload");
    std::string serialized;
    if (!serialize_payload(payload == args.end() ? engine_json::object() : *payload,
                           &serialized)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_event_publish(
        ctx, topic.c_str(), reinterpret_cast<const uint8_t*>(serialized.data()),
        serialized.size());
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── hotkey.* ─────────────────────────────────────────────────
// SaoSdkHotkeySpec 路径（sao_sdk_register_hotkey），回调走
// request->hotkey_callback typed slot。

int32_t invoke_hotkey_register(const SaoSdkContext* ctx, const engine_json& args,
                               sdk_context_call_request* request) {
    std::string binding_id;
    std::string default_key;
    if (ctx == nullptr || ctx->hotkey == nullptr || ctx->hotkey->register_hotkey == nullptr)
        return engine_no_provider();
    if ((!engine_arg_string(args, "id", &binding_id) &&
         !engine_arg_string(args, "name", &binding_id)) ||
        request->hotkey_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    uint64_t explicit_key = 0;
    uint64_t explicit_modifiers = 0;
    const bool has_virtual_key = args.contains("virtual_key");
    const bool has_modifiers = args.contains("modifiers");
    if ((has_virtual_key && !engine_arg_u64(args, "virtual_key", &explicit_key)) ||
        (has_modifiers && !engine_arg_u64(args, "modifiers", &explicit_modifiers))) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    uint32_t virtual_key = 0;
    uint32_t modifiers = 0;
    if (has_virtual_key) {
        if (explicit_key > 0xffu || explicit_modifiers > 0xffu) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        virtual_key = static_cast<uint32_t>(explicit_key);
        modifiers = static_cast<uint32_t>(explicit_modifiers);
    } else {
        if (!engine_arg_string(args, "default_key", &default_key) &&
            !engine_arg_string(args, "key", &default_key)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (!parse_hotkey(default_key, &virtual_key, &modifiers)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    SaoSdkHotkeySpec spec{};
    spec.binding_id_utf8 = binding_id.c_str();
    spec.virtual_key = virtual_key;
    spec.modifiers = modifiers;
    if (!engine_arg_bool(args, "enforce_ctrl_prefix", &spec.enforce_ctrl_prefix, false, true) ||
        !engine_arg_bool(args, "prevent_default", &spec.prevent_default, false, true) ||
        !engine_arg_bool(args, "allow_repeat", &spec.allow_repeat, false, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    sao_sdk_hotkey_id_t handle = 0;
    const int32_t status = sao_sdk_register_hotkey(
        ctx, &spec, request->hotkey_callback, request->callback_user_data, &handle);
    return status == SAO_SDK_OK ? engine_result(request, static_cast<uint64_t>(handle)) : status;
}

int32_t invoke_hotkey_unregister(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    uint64_t id = 0;
    if (ctx == nullptr || ctx->hotkey == nullptr || ctx->hotkey->unregister_hotkey == nullptr)
        return engine_no_provider();
    if (!engine_arg_u64(args, "id", &id)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_unregister_hotkey(ctx, id);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── tts.* ────────────────────────────────────────────────────
// speak(text_utf8, volume, rate)：volume 0..1、rate -10..10（SAPI 约定）。

int32_t invoke_tts_speak(const SaoSdkContext* ctx, const engine_json& args,
                         sdk_context_call_request* request) {
    std::string text;
    double volume = 1.0;
    double rate = 0.0;
    if (ctx == nullptr || ctx->tts == nullptr || ctx->tts->speak == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "text", &text)) return SAO_ERR_INVALID_ARGUMENT;
    if (!engine_arg_f64(args, "volume", &volume, 1.0, true)) return SAO_ERR_INVALID_ARGUMENT;
    // "speed" 是 "rate" 的别名——只在 rate 缺省时接受。
    const bool has_rate = args.contains("rate");
    const bool has_speed = !has_rate && args.contains("speed");
    if ((has_rate && !engine_arg_f64(args, "rate", &rate, 0.0, true)) ||
        (has_speed && !engine_arg_f64(args, "speed", &rate, 0.0, true))) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(volume) || !std::isfinite(rate)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status =
        sao_sdk_tts_speak(ctx, text.c_str(), static_cast<float>(volume), static_cast<float>(rate));
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_tts_stop(const SaoSdkContext* ctx, const engine_json& /*args*/,
                        sdk_context_call_request* request) {
    if (ctx == nullptr || ctx->tts == nullptr || ctx->tts->stop == nullptr)
        return engine_no_provider();
    const int32_t status = sao_sdk_tts_stop(ctx);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── banner.* ─────────────────────────────────────────────────

int32_t invoke_banner_show(const SaoSdkContext* ctx, const engine_json& args,
                           sdk_context_call_request* request) {
    std::string text;
    uint32_t duration_ms = 3000;
    uint32_t argb_color = 0;
    if (ctx == nullptr || ctx->banner == nullptr || ctx->banner->show == nullptr)
        return engine_no_provider();
    if (!engine_arg_string(args, "text", &text)) return SAO_ERR_INVALID_ARGUMENT;
    if (!engine_arg_u32(args, "duration_ms", &duration_ms, 3000, true) ||
        !engine_arg_u32(args, "argb_color", &argb_color, 0, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_banner_show(ctx, text.c_str(), duration_ms, argb_color);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── sound.* ──────────────────────────────────────────────────
// sao_sdk_sound_* 导出忽略 ctx 内部（路由到平台 XAudio2 mixer /
// AddFontResourceExW / level-up overlay），探测仅要求有效 ctx。

int32_t invoke_sound_procedural(const SaoSdkContext* ctx, const engine_json& args,
                                sdk_context_call_request* request) {
    int32_t kind = 0;
    if (ctx == nullptr) return engine_no_provider();
    if (!engine_arg_i32(args, "kind", &kind) || kind < SAO_SDK_SOUND_BEEP_LEVELUP ||
        kind > SAO_SDK_SOUND_BEEP_CONFIRM) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_sound_procedural(ctx, static_cast<sao_sdk_sound_kind_t>(kind));
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_sound_play_wav(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    std::string path;
    if (ctx == nullptr) return engine_no_provider();
    if (!engine_arg_string(args, "path", &path)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_sound_play_wav(ctx, path.c_str());
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_sound_load_font(const SaoSdkContext* ctx, const engine_json& args,
                               sdk_context_call_request* request) {
    std::string path;
    if (ctx == nullptr) return engine_no_provider();
    if (!engine_arg_string(args, "path", &path)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_sound_load_font(ctx, path.c_str());
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_sound_flash_overlay(const SaoSdkContext* ctx, const engine_json& args,
                                   sdk_context_call_request* request) {
    uint32_t duration_ms = 0;
    if (ctx == nullptr) return engine_no_provider();
    if (!engine_arg_u32(args, "duration_ms", &duration_ms)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_sound_flash_overlay(ctx, duration_ms);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── notify.* ─────────────────────────────────────────────────
// dispatch_notification 语义：text = title empty ? message : "title: message"。

int32_t invoke_notify_show(const SaoSdkContext* ctx, const engine_json& args,
                           sdk_context_call_request* request, double default_duration_s) {
    std::string title;
    std::string message;
    double duration_s = default_duration_s;
    uint32_t argb_color = 0xffffffffu;
    if (!engine_arg_string(args, "title", &title, true)) return SAO_ERR_INVALID_ARGUMENT;
    if (!engine_arg_string(args, "message", &message)) return SAO_ERR_INVALID_ARGUMENT;
    if (!engine_arg_f64(args, "duration_s", &duration_s, default_duration_s, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(duration_s) || duration_s < 0.0 ||
        duration_s >
            static_cast<double>(std::numeric_limits<uint32_t>::max()) / 1000.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!engine_arg_u32(args, "argb_color", &argb_color, 0xffffffffu, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const std::string text = title.empty() ? message : title + ": " + message;
    SaoSdkNotifySpec spec{};
    spec.text_utf8 = text.c_str();
    spec.duration_ms = static_cast<uint32_t>(std::ceil(duration_s * 1000.0));
    spec.argb_color = argb_color;
    sao_sdk_notify_token_t token = 0;
    const int32_t status = sao_sdk_notify_show(ctx, &spec, &token);
    return status == SAO_SDK_OK ? engine_result(request, token) : status;
}

int32_t invoke_notify_show_entry(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    return invoke_notify_show(ctx, args, request, 60.0);
}

int32_t invoke_notify_toast(const SaoSdkContext* ctx, const engine_json& args,
                            sdk_context_call_request* request) {
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    return invoke_notify_show(ctx, args, request, 3.0);
}

int32_t invoke_notify_dismiss(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    uint64_t token = 0;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_u64(args, "token", &token)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_notify_dismiss(ctx, token);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── dialog.* ─────────────────────────────────────────────────
// sao_sdk_dialog_* 是 provider-backed 自由函数；按钮回调经
// engine_callback 通用通道以 "dialog.result" 上报 {dialog,button,input}。

int32_t invoke_dialog_show(const SaoSdkContext* ctx, const engine_json& args,
                           sdk_context_call_request* request) {
    std::string message;
    std::string title;
    std::string input_prompt;
    std::string input_default;
    int32_t kind = SAO_SDK_DIALOG_INFO;
    int32_t input_max_length = 0;
    bool dismiss_on_focus_out = false;
    bool dismiss_on_esc = false;
    if (!engine_arg_string(args, "message", &message) || request->engine_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_string(args, "title", &title, true) ||
        !engine_arg_string(args, "input_prompt", &input_prompt, true) ||
        !engine_arg_string(args, "input_default", &input_default, true) ||
        !engine_arg_i32(args, "kind", &kind, SAO_SDK_DIALOG_INFO, true) ||
        !engine_arg_i32(args, "input_max_length", &input_max_length, 0, true) ||
        !engine_arg_bool(args, "dismiss_on_focus_out", &dismiss_on_focus_out, false, true) ||
        !engine_arg_bool(args, "dismiss_on_esc", &dismiss_on_esc, false, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (kind < SAO_SDK_DIALOG_INFO || kind > SAO_SDK_DIALOG_INPUT) return SAO_ERR_INVALID_ARGUMENT;

    auto* bridge = new MiscDialogBridge{request->engine_callback, request->callback_user_data};
    int32_t status = sao_plugins_binding_track_callback(bridge, &misc_dialog_bridge_release);
    if (status != SAO_OK) {
        delete bridge;
        return status;
    }
    SaoSdkDialogSpec spec{};
    spec.kind = kind;
    spec.title_utf8 = title.empty() ? nullptr : title.c_str();
    spec.message_utf8 = message.c_str();
    spec.input_prompt_utf8 = input_prompt.empty() ? nullptr : input_prompt.c_str();
    spec.input_default_utf8 = input_default.empty() ? nullptr : input_default.c_str();
    spec.input_max_length = input_max_length;
    spec.dismiss_on_focus_out = dismiss_on_focus_out;
    spec.dismiss_on_esc = dismiss_on_esc;
    sao_sdk_dialog_token_t token = 0;
    status = sao_sdk_dialog_show(ctx, &spec, &misc_dialog_forward, bridge, &token);
    if (status != SAO_SDK_OK) {
        // 桥只经 track_callback 登记；未交给 provider 即失败时由
        // release_all_callbacks/显式 release 回收，不能裸 delete。
        return status;
    }
    return engine_result(request, token);
}

int32_t invoke_dialog_dismiss(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    uint64_t token = 0;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_u64(args, "token", &token)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_dialog_dismiss(ctx, token);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── timer.* ──────────────────────────────────────────────────
// dispatch_timer 语义：seconds>0、≤u32max/1000，interval_ms=ceil(s*1000) 且
// 下限 1 ms；回调走 request->timer_callback typed slot。

int32_t invoke_timer_register(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    double seconds = 0.0;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_f64(args, "seconds", &seconds) ||
        request->timer_callback == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(seconds) || seconds <= 0.0 ||
        seconds > static_cast<double>(std::numeric_limits<uint32_t>::max()) / 1000.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const uint32_t interval_ms =
        static_cast<uint32_t>(std::max(1.0, std::ceil(seconds * 1000.0)));
    sao_sdk_timer_token_t token = 0;
    const int32_t status = sao_sdk_timer_register(
        ctx, interval_ms, request->timer_callback, request->callback_user_data, &token);
    return status == SAO_SDK_OK ? engine_result(request, token) : status;
}

int32_t invoke_timer_unregister(const SaoSdkContext* ctx, const engine_json& args,
                                sdk_context_call_request* request) {
    uint64_t token = 0;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_u64(args, "token", &token)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_timer_unregister(ctx, token);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── overlay.* ────────────────────────────────────────────────
// provider-backed sao_sdk_overlay_* 导出（不同于 ctx->ui->set_overlay 槽，
// 后者归 ui 组）。

int32_t invoke_overlay_set(const SaoSdkContext* ctx, const engine_json& args,
                           sdk_context_call_request* request) {
    std::string surface;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_string(args, "surface", &surface)) return SAO_ERR_INVALID_ARGUMENT;
    const auto spec = args.find("spec");
    if (spec == args.end()) return SAO_ERR_INVALID_ARGUMENT;
    std::string serialized;
    if (!serialize_payload(*spec, &serialized)) return SAO_ERR_INVALID_ARGUMENT;
    SaoSdkOverlaySpec overlay_spec{};
    overlay_spec.surface_id_utf8 = surface.c_str();
    overlay_spec.spec_json_utf8 = reinterpret_cast<const uint8_t*>(serialized.data());
    overlay_spec.spec_len = serialized.size();
    sao_sdk_overlay_token_t token = 0;
    const int32_t status = sao_sdk_overlay_set(ctx, &overlay_spec, &token);
    return status == SAO_SDK_OK ? engine_result(request, token) : status;
}

int32_t invoke_overlay_clear(const SaoSdkContext* ctx, const engine_json& args,
                             sdk_context_call_request* request) {
    uint64_t token = 0;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_u64(args, "token", &token)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_overlay_clear(ctx, token);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_overlay_clear_surface(const SaoSdkContext* ctx, const engine_json& args,
                                     sdk_context_call_request* request) {
    std::string surface;
    if (!probe_provider_bound(ctx)) return engine_no_provider();
    if (!engine_arg_string(args, "surface", &surface)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_overlay_clear_surface(ctx, surface.c_str());
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── version.* ────────────────────────────────────────────────

int32_t invoke_version_query(const SaoSdkContext* ctx, const engine_json& /*args*/,
                             sdk_context_call_request* request) {
    const uint32_t sdk_abi = sao_sdk_abi_version();
    const uint32_t ctx_abi = ctx != nullptr ? ctx->abi_version : 0;
    engine_json result;
    result["sdk_abi_version"] = sdk_abi;
    result["sdk_major"] = sdk_abi >> 16u;
    result["sdk_minor"] = sdk_abi & 0xffffu;
    result["ctx_abi_version"] = ctx_abi;
    result["ctx_major"] = ctx_abi >> 16u;
    result["ctx_minor"] = ctx_abi & 0xffffu;
    return engine_result(request, result);
}

// ── context.* / *.provider_status ────────────────────────────
// *_provider_status 返回底层原始 int 状态码（SAO_SDK_OK=0 表示可用）。

int32_t invoke_context_plugin_id(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                 sdk_context_call_request* request) {
    if (ctx == nullptr) return engine_no_provider();
    const char* plugin_id = nullptr;
    const int32_t status = sao_sdk_context_get_plugin_id(ctx, &plugin_id);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, plugin_id == nullptr ? std::string{} : std::string(plugin_id));
}

int32_t invoke_context_base_dir(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                sdk_context_call_request* request) {
    if (ctx == nullptr) return engine_no_provider();
    const char* base_dir = nullptr;
    const int32_t status = sao_sdk_context_get_base_dir(ctx, &base_dir);
    if (status != SAO_SDK_OK) return status;
    return engine_result(request, base_dir == nullptr ? std::string{} : std::string(base_dir));
}

int32_t invoke_context_plugin_version(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                      sdk_context_call_request* request) {
    if (ctx == nullptr || ctx->ctx_impl == nullptr) return engine_no_provider();
    return engine_result(request, ctx->plugin_version_utf8 == nullptr
                                      ? std::string{}
                                      : std::string(ctx->plugin_version_utf8));
}

// dispatch_property 语义：web/assets 路径由 base_dir 派生。
int32_t invoke_context_derived_path(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                    sdk_context_call_request* request,
                                    const char* suffix) {
    if (ctx == nullptr) return engine_no_provider();
    const char* base_dir = nullptr;
    const int32_t status = sao_sdk_context_get_base_dir(ctx, &base_dir);
    if (status != SAO_SDK_OK) return status;
    std::string path = base_dir == nullptr ? "" : base_dir;
    path += suffix;
    return engine_result(request, path);
}

int32_t invoke_context_web_path(const SaoSdkContext* ctx, const engine_json& args,
                                sdk_context_call_request* request) {
    return invoke_context_derived_path(ctx, args, request, "/web");
}

int32_t invoke_context_assets_path(const SaoSdkContext* ctx, const engine_json& args,
                                   sdk_context_call_request* request) {
    return invoke_context_derived_path(ctx, args, request, "/assets");
}

int32_t invoke_context_provider_status(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                       sdk_context_call_request* request) {
    if (ctx == nullptr) return engine_no_provider();
    return engine_result(request, sao_sdk_context_provider_status(ctx));
}

int32_t invoke_context_bind_platform_services(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                              sdk_context_call_request* request) {
    if (ctx == nullptr) return engine_no_provider();
    const int32_t status =
        sao_sdk_context_bind_platform_services(const_cast<SaoSdkContext*>(ctx));
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_mem_provider_status(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                   sdk_context_call_request* request) {
    if (ctx == nullptr) return engine_no_provider();
    return engine_result(request, sao_sdk_context_memory_provider_status(ctx));
}

int32_t invoke_net_provider_status(const SaoSdkContext* ctx, const engine_json& /*args*/,
                                   sdk_context_call_request* request) {
    if (ctx == nullptr) return engine_no_provider();
    return engine_result(request, sao_sdk_context_net_provider_status(ctx));
}

int32_t invoke_render_gpu_provider_status(const SaoSdkContext* /*ctx*/,
                                          const engine_json& /*args*/,
                                          sdk_context_call_request* request) {
    return engine_result(request, sao_sdk_platform_render_gpu_provider_status());
}

// ── platform.* ───────────────────────────────────────────────
// platform_panels.h / platform_internal.h 中脚本可调用（纯标量参数）的导出。

int32_t invoke_platform_open_panel(const SaoSdkContext* /*ctx*/, const engine_json& args,
                                   sdk_context_call_request* request) {
    std::string name;
    if (!engine_arg_string(args, "name", &name)) return SAO_ERR_INVALID_ARGUMENT;
    const int32_t status = sao_sdk_platform_open_panel(name.c_str());
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

int32_t invoke_platform_apply_streaming_mode(const SaoSdkContext* /*ctx*/,
                                             const engine_json& args,
                                             sdk_context_call_request* request) {
    bool enabled = false;
    if (!engine_arg_bool(args, "enabled", &enabled)) return SAO_ERR_INVALID_ARGUMENT;
    bool applied = false;
    const int32_t status = sao_sdk_platform_apply_streaming_mode(enabled ? 1 : 0, &applied);
    return status == SAO_SDK_OK ? engine_result(request, applied) : status;
}

int32_t invoke_platform_render_dispatch(const SaoSdkContext* /*ctx*/, const engine_json& args,
                                        sdk_context_call_request* request) {
    std::string surface;
    int32_t hook_point = 0;
    uint64_t monotonic_time_ns = 0;
    int32_t viewport_x = 0;
    int32_t viewport_y = 0;
    int32_t viewport_width = 0;
    int32_t viewport_height = 0;
    uint32_t dispatch_flags = 0;
    if (!engine_arg_string(args, "surface", &surface, true) ||
        !engine_arg_i32(args, "hook_point", &hook_point) ||
        !engine_arg_u64(args, "monotonic_time_ns", &monotonic_time_ns) ||
        !engine_arg_i32(args, "viewport_x", &viewport_x) ||
        !engine_arg_i32(args, "viewport_y", &viewport_y) ||
        !engine_arg_i32(args, "viewport_width", &viewport_width) ||
        !engine_arg_i32(args, "viewport_height", &viewport_height) ||
        !engine_arg_u32(args, "dispatch_flags", &dispatch_flags)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = sao_sdk_platform_render_dispatch(
        surface.empty() ? nullptr : surface.c_str(), hook_point, monotonic_time_ns, viewport_x,
        viewport_y, viewport_width, viewport_height, dispatch_flags);
    return status == SAO_SDK_OK ? engine_result(request, true) : status;
}

// ── 组表 ─────────────────────────────────────────────────────

const sdk_engine_function_desc kEngineFnsMisc[] = {
    // config.* — ctx->config 槽
    {"config.get_bool", kArgsKey, 1, &invoke_config_get_bool, &probe_config_get_bool},
    {"config.get_int", kArgsKey, 1, &invoke_config_get_int, &probe_config_get_int},
    {"config.get_double", kArgsKey, 1, &invoke_config_get_double, &probe_config_get_double},
    {"config.get_string", kArgsKey, 1, &invoke_config_get_string, &probe_config_get_string},
    {"config.set_bool", kArgsKeyValue, 2, &invoke_config_set_bool, &probe_config_set_bool},
    {"config.set_int", kArgsKeyValue, 2, &invoke_config_set_int, &probe_config_set_int},
    {"config.set_double", kArgsKeyValue, 2, &invoke_config_set_double, &probe_config_set_double},
    {"config.set_string", kArgsKeyValue, 2, &invoke_config_set_string, &probe_config_set_string},
    // event.* — ctx->event 槽；subscribe 用 request->event_callback
    {"event.subscribe", kArgsTopic, 1, &invoke_event_subscribe, &probe_event_subscribe},
    {"event.unsubscribe", kArgsToken, 1, &invoke_event_unsubscribe, &probe_event_unsubscribe},
    {"event.publish", kArgsTopicPayload, 2, &invoke_event_publish, &probe_event_publish},
    // hotkey.* — ctx->hotkey → provider_hotkey_*，provider 依赖
    {"hotkey.register", kArgsHotkeyRegister, 7, &invoke_hotkey_register, &probe_hotkey_register},
    {"hotkey.unregister", kArgsHotkeyId, 1, &invoke_hotkey_unregister, &probe_hotkey_unregister},
    // tts.* — ctx->tts → provider_tts_*，provider 依赖
    {"tts.speak", kArgsTtsSpeak, 3, &invoke_tts_speak, &probe_tts_speak},
    {"tts.stop", nullptr, 0, &invoke_tts_stop, &probe_tts_stop},
    // banner.* — ctx->banner → sao_sdk_notify_show，provider 依赖
    {"banner.show", kArgsBannerShow, 3, &invoke_banner_show, &probe_banner_show},
    // sound.* — sao_sdk_sound_* free export（ctx 参数被实现忽略）
    {"sound.procedural", kArgsSoundKind, 1, &invoke_sound_procedural, &probe_ctx},
    {"sound.play_wav", kArgsPath, 1, &invoke_sound_play_wav, &probe_ctx},
    {"sound.load_font", kArgsPath, 1, &invoke_sound_load_font, &probe_ctx},
    {"sound.flash_overlay", kArgsDurationMs, 1, &invoke_sound_flash_overlay, &probe_ctx},
    // notify.* — provider-backed sao_sdk_notify_*
    {"notify.show", kArgsNotifyShow, 4, &invoke_notify_show_entry, &probe_provider_bound},
    {"notify.toast", kArgsNotifyToast, 1, &invoke_notify_toast, &probe_provider_bound},
    {"notify.dismiss", kArgsToken, 1, &invoke_notify_dismiss, &probe_provider_bound},
    // dialog.* — provider-backed sao_sdk_dialog_*，结果经 "dialog.result" 通道
    {"dialog.show", kArgsDialogShow, 8, &invoke_dialog_show, &probe_provider_bound},
    {"dialog.dismiss", kArgsToken, 1, &invoke_dialog_dismiss, &probe_provider_bound},
    // timer.* — provider-backed sao_sdk_timer_*，request->timer_callback
    {"timer.register", kArgsSeconds, 1, &invoke_timer_register, &probe_provider_bound},
    {"timer.unregister", kArgsToken, 1, &invoke_timer_unregister, &probe_provider_bound},
    // overlay.* — provider-backed sao_sdk_overlay_* free export
    {"overlay.set", kArgsOverlaySet, 2, &invoke_overlay_set, &probe_provider_bound},
    {"overlay.clear", kArgsToken, 1, &invoke_overlay_clear, &probe_provider_bound},
    {"overlay.clear_surface", kArgsSurface, 1, &invoke_overlay_clear_surface,
     &probe_provider_bound},
    // version.* — sao_sdk_version.h / sao_sdk_abi_version()
    {"version.query", nullptr, 0, &invoke_version_query, &probe_ctx},
    // context.* — sao_sdk_context_* 查询/绑定导出 + 通用 provider 状态
    {"context.plugin_id", nullptr, 0, &invoke_context_plugin_id, &probe_ctx},
    {"context.plugin_version", nullptr, 0, &invoke_context_plugin_version, &probe_ctx},
    {"context.base_dir", nullptr, 0, &invoke_context_base_dir, &probe_ctx},
    {"context.web_path", nullptr, 0, &invoke_context_web_path, &probe_ctx},
    {"context.assets_path", nullptr, 0, &invoke_context_assets_path, &probe_ctx},
    {"context.bind_platform_services", nullptr, 0, &invoke_context_bind_platform_services,
     &probe_ctx},
    {"context.provider_status", nullptr, 0, &invoke_context_provider_status, &probe_ctx},
    // 域级 provider 状态查询（raw int）
    {"mem.provider_status", nullptr, 0, &invoke_mem_provider_status, &probe_ctx},
    {"net.provider_status", nullptr, 0, &invoke_net_provider_status, &probe_ctx},
    {"render_gpu.provider_status", nullptr, 0, &invoke_render_gpu_provider_status, &probe_always},
    // platform.* — sao_sdk_platform_panels.h / platform_internal.h 标量导出
    {"platform.open_panel", kArgsPanelName, 1, &invoke_platform_open_panel, &probe_always},
    {"platform.apply_streaming_mode", kArgsEnabled, 1, &invoke_platform_apply_streaming_mode,
     &probe_always},
    {"platform.render_dispatch", kArgsRenderDispatch, 8, &invoke_platform_render_dispatch,
     &probe_always},
};

} // namespace

const sdk_engine_group_table kEngineGroupMisc = {
    kEngineFnsMisc,
    sizeof(kEngineFnsMisc) / sizeof(kEngineFnsMisc[0]),
    nullptr, // 组 probe：无共享前提，全部经 desc.availability 惰性探测
};

} // namespace sao::plugins::sdk_binding
