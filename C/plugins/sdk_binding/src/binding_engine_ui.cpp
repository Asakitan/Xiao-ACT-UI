// binding_engine_ui.cpp — 反射引擎面 SaoSdkUiTable 组表：让五种脚本宿主经
// `ctx.engine` 调用 SaoSdkUiTable 的每个槽位与相关 sao_sdk_* 导出。
//
// 槽位直连策略：
//   - legacy 槽（register_panel / set_panel_spec / set_overlay /
//     unregister_render_hook / register_render_hook_clock / request_redraw）
//     在 probe 保证槽非空后直接调 `ctx->ui->slot(ctx->ctx_impl, ...)`。
//   - typed 扩展槽（register_ui_panel / unregister_ui_panel /
//     panel_add/update/remove_widget）必须走 sao_sdk_* 导出：导出内部经
//     `sao_sdk_ui_table_copy_slot(..., SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR)`
//     做 declared_size/minor 门控；裸读这类槽会越过旧表声明大小。
//   - engine 的 `ui.set_overlay` 调 RAW vtable 槽——按 surface 的 overlay
//     账本留在 legacy dispatch set_overlay arm（`state->overlays` 由该路径
//     维护）；engine 侧清理由 "ui.clear_overlay" /
//     "ui.overlay_clear_surface" 走 `sao_sdk_overlay_clear_surface`。
//   - legacy `register_render_hook` 槽（void* hook_fn）平台端恒
//     SAO_SDK_ERR_UNSUPPORTED；`ui.register_render_hook` 因此映射到
//     `sao_sdk_register_render_hook_ex`（spec: surface/hook_point/priority），
//     回调经堆 trampoline 打进 `request->engine_callback` 的
//     "ui.render_hook" 子通道。trampoline 只捕获 fn ptr + callback_user_data，
//     绝不捕获 request。

#include "sao/plugins/sdk_binding/binding_engine.h"

#include "sao/sdk/sao_sdk.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

namespace sao::plugins::sdk_binding {
namespace {

constexpr const char* kEngineUiRenderHookChannel = "ui.render_hook";

// 入参已由 engine dispatcher 的受限 SAX 解析过；这里只做 dump + 总量护栏。
bool dump_engine_json(const engine_json& value, std::string* out) noexcept {
    if (out == nullptr) return false;
    try {
        *out = value.dump();
        return !out->empty() && out->size() <= kMaximumBindingJsonBytes;
    } catch (...) {
        if (out != nullptr) out->clear();
        return false;
    }
}

// 槽位探测：表存在 + （可能缩短的）declared_size 覆盖该槽 + 槽值非空。
// 所有槽都是同一调用约定的函数指针，宽度 == sizeof(void*)。
bool ui_slot_ready(const SaoSdkContext* ctx, size_t offset, size_t size,
                   uint32_t minor) noexcept {
    if (ctx == nullptr || ctx->ui == nullptr || size != sizeof(void*)) return false;
    if (sao_sdk_ui_table_slot_status(ctx, offset, size, minor) != SAO_SDK_OK) return false;
    const auto* base = reinterpret_cast<const unsigned char*>(ctx->ui);
    void* slot = nullptr;
    std::memcpy(&slot, base + offset, sizeof(slot));
    return slot != nullptr;
}

#define SAO_UI_SLOT_PROBE(name, member, minor)                                        \
    bool name(const SaoSdkContext* ctx) noexcept {                                   \
        return ui_slot_ready(ctx, offsetof(SaoSdkUiTable, member),                   \
                             sizeof(((SaoSdkUiTable*)nullptr)->member), (minor));    \
    }

SAO_UI_SLOT_PROBE(probe_ui_register_panel, register_panel, 0u)
SAO_UI_SLOT_PROBE(probe_ui_set_panel_spec, set_panel_spec, 0u)
SAO_UI_SLOT_PROBE(probe_ui_set_overlay, set_overlay, 0u)
SAO_UI_SLOT_PROBE(probe_ui_request_redraw, request_redraw, 0u)
SAO_UI_SLOT_PROBE(probe_ui_unregister_render_hook, unregister_render_hook, 0u)
SAO_UI_SLOT_PROBE(probe_ui_register_render_hook_clock, register_render_hook_clock, 0u)
SAO_UI_SLOT_PROBE(probe_ui_register_ui_panel, register_ui_panel,
                  SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR)
SAO_UI_SLOT_PROBE(probe_ui_unregister_ui_panel, unregister_ui_panel,
                  SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR)
SAO_UI_SLOT_PROBE(probe_ui_panel_add_widget, panel_add_widget,
                  SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR)
SAO_UI_SLOT_PROBE(probe_ui_panel_update_widget, panel_update_widget,
                  SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR)
SAO_UI_SLOT_PROBE(probe_ui_panel_remove_widget, panel_remove_widget,
                  SAO_SDK_UI_TABLE_TYPED_CONTEXT_MINOR)

bool probe_always(const SaoSdkContext*) noexcept { return true; }

bool probe_ui_group(const SaoSdkContext* ctx) noexcept {
    return ctx != nullptr && ctx->ui != nullptr;
}

// overlay_* / register_render_hook(_ex) 走 provider 直链而非某个可见 ui 槽；
// 用同族 wire 槽的非空作为 catalog `available` 的代理探测。
sdk_engine_probe_fn const kProbeOverlayFamily = &probe_ui_set_overlay;
sdk_engine_probe_fn const kProbeRenderHookFamily = &probe_ui_register_render_hook_clock;

// ── render-hook 堆 trampoline ────────────────────────────────────────
// 捕获 engine_callback fn ptr + callback_user_data（绝不捕获 request）。
// token→state 注册表使 ui.unregister_render_hook 在 SDK 注销成功后能精确
// 释放对应 trampoline；ctx teardown 未注销的残余条目随进程回收。

struct render_hook_emit_state {
    sdk_context_engine_callback_fn emit;
    void* emit_user_data;
};

std::mutex g_render_hook_mutex;
std::unordered_map<sao_sdk_hook_token_t, render_hook_emit_state*> g_render_hooks;

bool remember_render_hook(sao_sdk_hook_token_t token,
                          render_hook_emit_state* state) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_render_hook_mutex);
        g_render_hooks.emplace(token, state);
        return true;
    } catch (...) {
        return false;
    }
}

render_hook_emit_state* forget_render_hook(sao_sdk_hook_token_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_render_hook_mutex);
        const auto it = g_render_hooks.find(token);
        if (it == g_render_hooks.end()) return nullptr;
        render_hook_emit_state* state = it->second;
        g_render_hooks.erase(it);
        return state;
    } catch (...) {
        return nullptr;
    }
}

sao_sdk_status_t SAO_SDK_CALL render_hook_emit_trampoline(
    int32_t hook_point, const SaoSdkRenderHookPayload* payload,
    void* user_data) noexcept {
    auto* state = static_cast<render_hook_emit_state*>(user_data);
    if (state == nullptr || state->emit == nullptr || payload == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    try {
        engine_json event = engine_json::object();
        event["hook_point"] = hook_point;
        event["frame_time_us"] = payload->frame_time_us;
        event["frame_index"] = payload->frame_index;
        event["frame_delta_us"] = payload->frame_delta_us;
        event["viewport_x_px"] = payload->viewport_x_px;
        event["viewport_y_px"] = payload->viewport_y_px;
        event["viewport_width_px"] = payload->viewport_width_px;
        event["viewport_height_px"] = payload->viewport_height_px;
        event["dispatch_flags"] = payload->dispatch_flags;
        event["reserved"] = payload->reserved;
        const std::string serialized = event.dump();
        if (serialized.empty() || serialized.size() > kMaximumBindingJsonBytes) {
            return SAO_SDK_ERR_INTERNAL;
        }
        state->emit(kEngineUiRenderHookChannel,
                    reinterpret_cast<const uint8_t*>(serialized.data()),
                    serialized.size(), state->emit_user_data);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

// surface == nullptr → 纯 clock 槽（全部 surface、priority 0）。
int32_t register_render_hook_trampolined(const SaoSdkContext* ctx,
                                         sdk_context_call_request* request,
                                         const char* surface_utf8,
                                         int32_t hook_point, float priority) {
    if (request->engine_callback == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    auto* state =
        new (std::nothrow) render_hook_emit_state{request->engine_callback,
                                                  request->callback_user_data};
    if (state == nullptr) return SAO_ERR_OS_CALL_FAILED;
    sao_sdk_hook_token_t token = 0;
    sao_sdk_status_t status;
    if (surface_utf8 == nullptr) {
        status = sao_sdk_register_render_hook(ctx, hook_point,
                                              &render_hook_emit_trampoline, state,
                                              &token);
    } else {
        SaoSdkRenderHookSpec spec{};
        spec.surface_id_utf8 = surface_utf8;
        spec.hook_point = hook_point;
        spec.priority = priority;
        status = sao_sdk_register_render_hook_ex(
            ctx, &spec, &render_hook_emit_trampoline, state, &token);
    }
    if (status != SAO_SDK_OK) {
        delete state;
        return status;
    }
    if (!remember_render_hook(token, state)) {
        (void)sao_sdk_unregister_render_hook(ctx, token);
        delete state;
        return SAO_ERR_OS_CALL_FAILED;
    }
    return engine_result(request, token);
}

// ── spec 填充辅助 ────────────────────────────────────────────────────

// SaoSdkPanelDescriptor：flat 参数对象 → ABI 描述体。字符串存活在 caller
// 作用域的 std::string 里；widget props 同理（对象形式 dump 成 JSON 串）。
int32_t fill_panel_descriptor(const engine_json& args, SaoSdkPanelDescriptor* out,
                              std::string* id_storage,
                              std::string* title_storage) {
    if (out == nullptr || id_storage == nullptr || title_storage == nullptr ||
        !args.is_object())
        return SAO_ERR_INVALID_ARGUMENT;
    SaoSdkPanelDescriptor descriptor{};
    if (!engine_arg_string(args, "id", id_storage) &&
        !engine_arg_string(args, "panel_id", id_storage)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (id_storage->empty()) return SAO_ERR_INVALID_ARGUMENT;
    if (!engine_arg_string(args, "title", title_storage, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    descriptor.panel_id_utf8 = id_storage->c_str();
    descriptor.title_utf8 = title_storage->empty() ? nullptr : title_storage->c_str();
    if (!engine_arg_i32(args, "default_x_px", &descriptor.default_x_px, 0, true) ||
        !engine_arg_i32(args, "default_y_px", &descriptor.default_y_px, 0, true) ||
        !engine_arg_i32(args, "default_width_px", &descriptor.default_width_px, 0, true) ||
        !engine_arg_i32(args, "default_height_px", &descriptor.default_height_px, 0, true) ||
        !engine_arg_i32(args, "min_width_px", &descriptor.min_width_px, 0, true) ||
        !engine_arg_i32(args, "min_height_px", &descriptor.min_height_px, 0, true) ||
        !engine_arg_bool(args, "movable", &descriptor.movable, true, true) ||
        !engine_arg_bool(args, "resizable", &descriptor.resizable, true, true) ||
        !engine_arg_bool(args, "show_titlebar", &descriptor.show_titlebar, true, true) ||
        !engine_arg_bool(args, "show_close_button", &descriptor.show_close_button, true,
                         true) ||
        !engine_arg_bool(args, "visible", &descriptor.visible, true, true) ||
        !engine_arg_bool(args, "remember_geometry", &descriptor.remember_geometry, false,
                         true) ||
        !engine_arg_bool(args, "modal", &descriptor.modal, false, true) ||
        !engine_arg_bool(args, "overlay_style", &descriptor.overlay_style, false, true) ||
        !engine_arg_i32(args, "z_class", &descriptor.z_class, 0, true) ||
        !engine_arg_i32(args, "z_within_class", &descriptor.z_within_class, 0, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    double opacity = 0.0;
    if (!engine_arg_f64(args, "initial_opacity", &opacity, 0.0, true) ||
        !std::isfinite(opacity)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    descriptor.initial_opacity = static_cast<float>(opacity); // 0 → 平台默认 1.0
    descriptor.struct_size = sizeof(SaoSdkPanelDescriptor);
    *out = descriptor;
    return SAO_OK;
}

int32_t fill_widget_spec(const engine_json& spec_json, SaoSdkWidgetSpec* out,
                         std::string* widget_id_storage, std::string* text_storage,
                         std::string* props_storage) {
    if (out == nullptr || widget_id_storage == nullptr || text_storage == nullptr ||
        props_storage == nullptr || !spec_json.is_object()) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    SaoSdkWidgetSpec spec{};
    if (!engine_arg_i32(spec_json, "kind", &spec.kind)) return SAO_ERR_INVALID_ARGUMENT;
    if (!engine_arg_string(spec_json, "widget_id", widget_id_storage, true) ||
        !engine_arg_string(spec_json, "text", text_storage, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const auto props = spec_json.find("props");
    if (props != spec_json.end()) {
        if (props->is_string()) {
            *props_storage = props->get<std::string>();
        } else if (!dump_engine_json(*props, props_storage)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }
    if (!engine_arg_i32(spec_json, "x_px", &spec.x_px, 0, true) ||
        !engine_arg_i32(spec_json, "y_px", &spec.y_px, 0, true) ||
        !engine_arg_i32(spec_json, "width_px", &spec.width_px, 0, true) ||
        !engine_arg_i32(spec_json, "height_px", &spec.height_px, 0, true) ||
        !engine_arg_i32(spec_json, "z_order", &spec.z_order, 0, true)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    double value = 0.0;
    double max_value = 0.0;
    if (!engine_arg_f64(spec_json, "value", &value, 0.0, true) ||
        !engine_arg_f64(spec_json, "max_value", &max_value, 0.0, true) ||
        !std::isfinite(value) || !std::isfinite(max_value)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    spec.value = static_cast<float>(value);
    spec.max_value = static_cast<float>(max_value);
    spec.widget_id_utf8 = widget_id_storage->empty() ? nullptr : widget_id_storage->c_str();
    spec.text_utf8 = text_storage->empty() ? nullptr : text_storage->c_str();
    spec.props_json_utf8 = props_storage->empty()
                               ? nullptr
                               : reinterpret_cast<const uint8_t*>(props_storage->data());
    spec.props_len = props_storage->size();
    spec.struct_size = sizeof(SaoSdkWidgetSpec);
    *out = spec;
    return SAO_OK;
}

sao_sdk_ui_panel_t panel_from_u64(uint64_t handle) noexcept {
    return reinterpret_cast<sao_sdk_ui_panel_t>(static_cast<uintptr_t>(handle));
}

sao_sdk_ui_widget_t widget_from_u64(uint64_t handle) noexcept {
    return reinterpret_cast<sao_sdk_ui_widget_t>(static_cast<uintptr_t>(handle));
}

uint64_t panel_to_u64(sao_sdk_ui_panel_t panel) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(panel));
}

uint64_t widget_to_u64(sao_sdk_ui_widget_t widget) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(widget));
}

// ── invoker ──────────────────────────────────────────────────────────

int32_t invoke_ui_register_panel(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    try {
        if (ctx == nullptr || ctx->ui == nullptr || ctx->ui->register_panel == nullptr) {
            return engine_no_provider();
        }
        std::string id;
        if (!engine_arg_string(args, "id", &id)) return SAO_ERR_INVALID_ARGUMENT;
        std::string title;
        if (!engine_arg_string(args, "title", &title, true)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (title.empty()) title = id;
        const auto spec = args.find("spec");
        std::string serialized;
        if (!dump_engine_json(spec == args.end() ? engine_json::object() : *spec,
                              &serialized)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        sao_sdk_ui_panel_t panel = nullptr;
        const auto status = ctx->ui->register_panel(
            ctx->ctx_impl, id.c_str(), title.c_str(),
            reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size(),
            request->panel_action_callback, request->callback_user_data, &panel);
        return status == SAO_SDK_OK ? engine_result(request, panel_to_u64(panel))
                                    : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_set_panel_spec(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    try {
        if (ctx == nullptr || ctx->ui == nullptr || ctx->ui->set_panel_spec == nullptr) {
            return engine_no_provider();
        }
        uint64_t handle = 0;
        const auto spec = args.find("spec");
        if (!engine_arg_u64(args, "panel", &handle) || spec == args.end()) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        std::string serialized;
        if (!dump_engine_json(*spec, &serialized)) return SAO_ERR_INVALID_ARGUMENT;
        const auto status = ctx->ui->set_panel_spec(
            ctx->ctx_impl, panel_from_u64(handle),
            reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// Engine 路由调 RAW vtable 槽：按 surface 的 overlay 账本由 legacy
// set_overlay arm 维护（见文件头注释）。
int32_t invoke_ui_set_overlay(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    try {
        if (ctx == nullptr || ctx->ui == nullptr || ctx->ui->set_overlay == nullptr) {
            return engine_no_provider();
        }
        std::string surface;
        if (!engine_arg_string(args, "surface", &surface)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto spec = args.find("spec");
        if (spec == args.end()) return SAO_ERR_INVALID_ARGUMENT;
        std::string serialized;
        if (!dump_engine_json(*spec, &serialized)) return SAO_ERR_INVALID_ARGUMENT;
        const auto status = ctx->ui->set_overlay(
            ctx->ctx_impl, surface.c_str(),
            reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_request_redraw(const SaoSdkContext* ctx, const engine_json& args,
                                 sdk_context_call_request* request) {
    try {
        if (ctx == nullptr || ctx->ui == nullptr || ctx->ui->request_redraw == nullptr) {
            return engine_no_provider();
        }
        std::string surface;
        if (!engine_arg_string(args, "surface", &surface, true)) surface.clear();
        const auto status = ctx->ui->request_redraw(
            ctx->ctx_impl, surface.empty() ? nullptr : surface.c_str());
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_clear_overlay(const SaoSdkContext* ctx, const engine_json& args,
                                sdk_context_call_request* request) {
    try {
        std::string surface;
        if (!engine_arg_string(args, "surface", &surface)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto status = sao_sdk_overlay_clear_surface(ctx, surface.c_str());
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_register_render_hook(const SaoSdkContext* ctx,
                                       const engine_json& args,
                                       sdk_context_call_request* request) {
    try {
        std::string surface;
        if (!engine_arg_string(args, "surface", &surface) &&
            !engine_arg_string(args, "surface_id", &surface)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        int32_t hook_point = 0;
        double priority = 0.0;
        if (!engine_arg_i32(args, "hook_point", &hook_point) ||
            hook_point < 0 || hook_point > SAO_SDK_HOOK_AFTER_PRESENT ||
            !engine_arg_f64(args, "priority", &priority, 0.0, true) ||
            !std::isfinite(priority)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        return register_render_hook_trampolined(
            ctx, request, surface.c_str(), hook_point, static_cast<float>(priority));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_register_render_hook_clock(const SaoSdkContext* ctx,
                                             const engine_json& args,
                                             sdk_context_call_request* request) {
    try {
        int32_t hook_point = 0;
        if (!engine_arg_i32(args, "hook_point", &hook_point) || hook_point < 0 ||
            hook_point > SAO_SDK_HOOK_AFTER_PRESENT) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        return register_render_hook_trampolined(ctx, request, nullptr, hook_point, 0.0F);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_unregister_render_hook(const SaoSdkContext* ctx,
                                         const engine_json& args,
                                         sdk_context_call_request* request) {
    try {
        uint64_t token = 0;
        if (!engine_arg_u64(args, "hook", &token) || token == 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto status = sao_sdk_unregister_render_hook(ctx, token);
        if (status != SAO_SDK_OK) return status;
        // 仅当 SDK 注销确实成功后释放本 TU 持有的 trampoline。
        if (render_hook_emit_state* state = forget_render_hook(token)) delete state;
        return engine_result(request, true);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_register_ui_panel(const SaoSdkContext* ctx, const engine_json& args,
                                    sdk_context_call_request* request) {
    try {
        SaoSdkPanelDescriptor descriptor{};
        std::string id;
        std::string title;
        const int32_t fill =
            fill_panel_descriptor(args, &descriptor, &id, &title);
        if (fill != SAO_OK) return fill;
        sao_sdk_ui_panel_t panel = nullptr;
        const auto status = sao_sdk_register_ui_panel(ctx, &descriptor, &panel);
        return status == SAO_SDK_OK ? engine_result(request, panel_to_u64(panel))
                                    : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_unregister_ui_panel(const SaoSdkContext* ctx, const engine_json& args,
                                      sdk_context_call_request* request) {
    try {
        uint64_t handle = 0;
        if (!engine_arg_u64(args, "panel", &handle)) return SAO_ERR_INVALID_ARGUMENT;
        const auto status = sao_sdk_unregister_ui_panel(ctx, panel_from_u64(handle));
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// spec 既接受 {"panel":..,"spec":{...}} 也接受直接把 widget 字段平铺在 args。
const engine_json* widget_spec_args(const engine_json& args) {
    const auto spec = args.find("spec");
    return spec != args.end() ? &*spec : &args;
}

int32_t invoke_ui_panel_add_widget(const SaoSdkContext* ctx, const engine_json& args,
                                   sdk_context_call_request* request) {
    try {
        uint64_t handle = 0;
        if (!engine_arg_u64(args, "panel", &handle)) return SAO_ERR_INVALID_ARGUMENT;
        SaoSdkWidgetSpec spec{};
        std::string widget_id;
        std::string text;
        std::string props;
        const int32_t fill =
            fill_widget_spec(*widget_spec_args(args), &spec, &widget_id, &text, &props);
        if (fill != SAO_OK) return fill;
        sao_sdk_ui_widget_t widget = nullptr;
        const auto status =
            sao_sdk_panel_add_widget(ctx, panel_from_u64(handle), &spec, &widget);
        return status == SAO_SDK_OK ? engine_result(request, widget_to_u64(widget))
                                    : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_panel_update_widget(const SaoSdkContext* ctx, const engine_json& args,
                                      sdk_context_call_request* request) {
    try {
        uint64_t panel_handle = 0;
        uint64_t widget_handle = 0;
        if (!engine_arg_u64(args, "panel", &panel_handle) ||
            !engine_arg_u64(args, "widget", &widget_handle)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        SaoSdkWidgetSpec spec{};
        std::string widget_id;
        std::string text;
        std::string props;
        const int32_t fill =
            fill_widget_spec(*widget_spec_args(args), &spec, &widget_id, &text, &props);
        if (fill != SAO_OK) return fill;
        const auto status =
            sao_sdk_panel_update_widget(ctx, panel_from_u64(panel_handle),
                                        widget_from_u64(widget_handle), &spec);
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_panel_remove_widget(const SaoSdkContext* ctx, const engine_json& args,
                                      sdk_context_call_request* request) {
    try {
        uint64_t panel_handle = 0;
        uint64_t widget_handle = 0;
        if (!engine_arg_u64(args, "panel", &panel_handle) ||
            (!engine_arg_u64(args, "widget_id", &widget_handle) &&
             !engine_arg_u64(args, "widget", &widget_handle))) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto status =
            sao_sdk_panel_remove_widget(ctx, panel_from_u64(panel_handle),
                                        widget_from_u64(widget_handle));
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// ── sao_sdk_* 自由导出包装 ──────────────────────────────────────────

int32_t invoke_ui_overlay_set(const SaoSdkContext* ctx, const engine_json& args,
                              sdk_context_call_request* request) {
    try {
        std::string surface;
        if (!engine_arg_string(args, "surface", &surface)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto spec = args.find("spec");
        if (spec == args.end()) return SAO_ERR_INVALID_ARGUMENT;
        std::string serialized;
        if (!dump_engine_json(*spec, &serialized)) return SAO_ERR_INVALID_ARGUMENT;
        SaoSdkOverlaySpec overlay_spec{};
        overlay_spec.surface_id_utf8 = surface.c_str();
        overlay_spec.spec_json_utf8 =
            reinterpret_cast<const uint8_t*>(serialized.data());
        overlay_spec.spec_len = serialized.size();
        sao_sdk_overlay_token_t overlay = 0;
        const auto status = sao_sdk_overlay_set(ctx, &overlay_spec, &overlay);
        return status == SAO_SDK_OK ? engine_result(request, overlay) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_overlay_clear(const SaoSdkContext* ctx, const engine_json& args,
                                sdk_context_call_request* request) {
    try {
        uint64_t overlay = 0;
        if (!engine_arg_u64(args, "overlay", &overlay) || overlay == 0) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto status = sao_sdk_overlay_clear(ctx, overlay);
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_request_redraw_surface(const SaoSdkContext* ctx,
                                         const engine_json& args,
                                         sdk_context_call_request* request) {
    try {
        std::string surface;
        if (!engine_arg_string(args, "surface", &surface, true)) surface.clear();
        const auto status = sao_sdk_request_redraw_surface(
            ctx, surface.empty() ? nullptr : surface.c_str());
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_request_redraw_panel(const SaoSdkContext* ctx,
                                       const engine_json& args,
                                       sdk_context_call_request* request) {
    try {
        uint64_t handle = 0;
        if (!engine_arg_u64(args, "panel", &handle)) return SAO_ERR_INVALID_ARGUMENT;
        const auto status = sao_sdk_request_redraw(ctx, panel_from_u64(handle));
        return status == SAO_SDK_OK ? engine_result(request, true) : status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_table_status(const SaoSdkContext* ctx, const engine_json&,
                               sdk_context_call_request* request) {
    try {
        return engine_result(request, sao_sdk_ui_table_status(ctx));
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_table_slot_status_common(const SaoSdkContext* ctx,
                                           const engine_json& args,
                                           sdk_context_call_request* request,
                                           bool authorize_only) {
    try {
        uint64_t slot_offset = 0;
        uint64_t slot_size = 0;
        uint32_t minor = 0;
        if (!engine_arg_u64(args, "slot_offset", &slot_offset) ||
            !engine_arg_u64(args, "slot_size", &slot_size) ||
            !engine_arg_u32(args, "required_context_minor", &minor, 0, true) ||
            slot_offset > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
            slot_size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const auto status =
            authorize_only
                ? sao_sdk_ui_table_authorize_slot(ctx, static_cast<size_t>(slot_offset),
                                                  static_cast<size_t>(slot_size), minor)
                : sao_sdk_ui_table_slot_status(ctx, static_cast<size_t>(slot_offset),
                                               static_cast<size_t>(slot_size), minor);
        return engine_result(request, status);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t invoke_ui_table_slot_status(const SaoSdkContext* ctx, const engine_json& args,
                                    sdk_context_call_request* request) {
    return invoke_ui_table_slot_status_common(ctx, args, request, false);
}

int32_t invoke_ui_table_authorize_slot(const SaoSdkContext* ctx, const engine_json& args,
                                       sdk_context_call_request* request) {
    return invoke_ui_table_slot_status_common(ctx, args, request, true);
}

// ── 目录表 ───────────────────────────────────────────────────────────

const char* const kArgsRegisterPanel[] = {"id", "title", "spec"};
const char* const kArgsPanelSpec[] = {"panel", "spec"};
const char* const kArgsSurfaceSpec[] = {"surface", "spec"};
const char* const kArgsSurface[] = {"surface"};
const char* const kArgsRenderHook[] = {"surface", "hook_point", "priority"};
const char* const kArgsHookPoint[] = {"hook_point"};
const char* const kArgsHook[] = {"hook"};
const char* const kArgsPanelDescriptor[] = {
    "id",           "title",         "default_x_px",      "default_y_px",
    "default_width_px", "default_height_px", "min_width_px", "min_height_px",
    "movable",      "resizable",     "show_titlebar",     "show_close_button",
    "visible",      "remember_geometry", "modal",         "overlay_style",
    "z_class",      "z_within_class", "initial_opacity",
};
const char* const kArgsPanel[] = {"panel"};
const char* const kArgsPanelUpdateWidget[] = {"panel", "widget", "spec"};
const char* const kArgsPanelRemoveWidget[] = {"panel", "widget_id"};
const char* const kArgsOverlay[] = {"overlay"};
const char* const kArgsSlotStatus[] = {"slot_offset", "slot_size",
                                       "required_context_minor"};

const sdk_engine_function_desc kEngineUiDescs[] = {
    {"ui.register_panel", kArgsRegisterPanel,
     static_cast<uint32_t>(std::size(kArgsRegisterPanel)), &invoke_ui_register_panel,
     &probe_ui_register_panel},
    {"ui.set_panel_spec", kArgsPanelSpec,
     static_cast<uint32_t>(std::size(kArgsPanelSpec)), &invoke_ui_set_panel_spec,
     &probe_ui_set_panel_spec},
    {"ui.set_overlay", kArgsSurfaceSpec,
     static_cast<uint32_t>(std::size(kArgsSurfaceSpec)), &invoke_ui_set_overlay,
     &probe_ui_set_overlay},
    {"ui.request_redraw", kArgsSurface,
     static_cast<uint32_t>(std::size(kArgsSurface)), &invoke_ui_request_redraw,
     &probe_ui_request_redraw},
    {"ui.clear_overlay", kArgsSurface,
     static_cast<uint32_t>(std::size(kArgsSurface)), &invoke_ui_clear_overlay,
     kProbeOverlayFamily},
    {"ui.register_render_hook", kArgsRenderHook,
     static_cast<uint32_t>(std::size(kArgsRenderHook)),
     &invoke_ui_register_render_hook, kProbeRenderHookFamily},
    {"ui.register_render_hook_clock", kArgsHookPoint,
     static_cast<uint32_t>(std::size(kArgsHookPoint)),
     &invoke_ui_register_render_hook_clock, &probe_ui_register_render_hook_clock},
    {"ui.unregister_render_hook", kArgsHook,
     static_cast<uint32_t>(std::size(kArgsHook)), &invoke_ui_unregister_render_hook,
     &probe_ui_unregister_render_hook},
    {"ui.register_ui_panel", kArgsPanelDescriptor,
     static_cast<uint32_t>(std::size(kArgsPanelDescriptor)),
     &invoke_ui_register_ui_panel, &probe_ui_register_ui_panel},
    {"ui.unregister_ui_panel", kArgsPanel,
     static_cast<uint32_t>(std::size(kArgsPanel)), &invoke_ui_unregister_ui_panel,
     &probe_ui_unregister_ui_panel},
    {"ui.panel_add_widget", kArgsPanelSpec,
     static_cast<uint32_t>(std::size(kArgsPanelSpec)), &invoke_ui_panel_add_widget,
     &probe_ui_panel_add_widget},
    {"ui.panel_update_widget", kArgsPanelUpdateWidget,
     static_cast<uint32_t>(std::size(kArgsPanelUpdateWidget)),
     &invoke_ui_panel_update_widget, &probe_ui_panel_update_widget},
    {"ui.panel_remove_widget", kArgsPanelRemoveWidget,
     static_cast<uint32_t>(std::size(kArgsPanelRemoveWidget)),
     &invoke_ui_panel_remove_widget, &probe_ui_panel_remove_widget},
    {"ui.overlay_set", kArgsSurfaceSpec,
     static_cast<uint32_t>(std::size(kArgsSurfaceSpec)), &invoke_ui_overlay_set,
     kProbeOverlayFamily},
    {"ui.overlay_clear", kArgsOverlay,
     static_cast<uint32_t>(std::size(kArgsOverlay)), &invoke_ui_overlay_clear,
     kProbeOverlayFamily},
    {"ui.overlay_clear_surface", kArgsSurface,
     static_cast<uint32_t>(std::size(kArgsSurface)), &invoke_ui_clear_overlay,
     kProbeOverlayFamily},
    {"ui.request_redraw_surface", kArgsSurface,
     static_cast<uint32_t>(std::size(kArgsSurface)),
     &invoke_ui_request_redraw_surface, &probe_ui_request_redraw},
    {"ui.request_redraw_panel", kArgsPanel,
     static_cast<uint32_t>(std::size(kArgsPanel)), &invoke_ui_request_redraw_panel,
     &probe_ui_request_redraw},
    {"ui.table_status", nullptr, 0, &invoke_ui_table_status, &probe_always},
    {"ui.table_slot_status", kArgsSlotStatus,
     static_cast<uint32_t>(std::size(kArgsSlotStatus)), &invoke_ui_table_slot_status,
     &probe_always},
    {"ui.table_authorize_slot", kArgsSlotStatus,
     static_cast<uint32_t>(std::size(kArgsSlotStatus)),
     &invoke_ui_table_authorize_slot, &probe_always},
};

} // namespace

const sdk_engine_group_table kEngineGroupUi = {
    kEngineUiDescs,
    std::size(kEngineUiDescs),
    &probe_ui_group,
};

} // namespace sao::plugins::sdk_binding
